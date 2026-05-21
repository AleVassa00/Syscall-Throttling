# Syscall Throttling Monitor

## Overview

Questo progetto implementa un **Linux Kernel Module (LKM)** per il monitoraggio e il throttling di system call su architettura **x86-64**.

Il modulo consente di registrare dinamicamente:

* numeri di syscall;
* effective user ID;
* programmi eseguibili;

tramite un device driver accessibile da userspace mediante `ioctl()`.

Quando una syscall registrata viene invocata da un UID registrato oppure da un programma registrato, il modulo applica un limite massimo `MAX` al numero di invocazioni consentite in una finestra temporale di 1 secondo. Se il limite viene superato, il thread chiamante viene temporaneamente bloccato prima dell'esecuzione effettiva della syscall.

Il monitor può essere abilitato o disabilitato dinamicamente. Quando è disabilitato, le syscall vengono lasciate proseguire senza alcun limite.

---

## Development Environment

Il progetto è stato sviluppato e testato sul seguente ambiente:

- OS: Ubuntu 24.04 LTS
- Architecture: x86_64
- Kernel: Linux 6.17.0-23-generic
- GCC: gcc-13
- Build system: kbuild

## Project Structure

Il progetto è suddiviso in più componenti, ciascuno con una responsabilità specifica.

```text
main.c
  Inizializzazione e cleanup del modulo.

probe.c / probe.h
  Risoluzione dinamica di simboli kernel non esportati, come kallsyms_lookup_name.

syscall_hook.c / syscall_hook.h
  Hooking delle syscall, patch del dispatcher x64_sys_call e gestione dei puntatori originali.

monitor.c / monitor.h
  Logica di throttling: finestra temporale, contatore globale, timer, wait queue.

registry.c / registry.h
  Registry di UID, programmi e syscall monitorate.

stats.c / stats.h
  Raccolta e calcolo delle statistiche richieste dalla specifica.

device.c / device.h
  Device driver e gestione delle ioctl da userspace.
```

---

## Device Driver and IOCTL Interface

Il modulo espone un device `/dev/syscall_monitor`, registrato tramite interfaccia misc device.

Il device supporta operazioni `ioctl()` per:

* aggiungere o rimuovere syscall monitorate;
* aggiungere o rimuovere UID monitorati;
* aggiungere o rimuovere programmi monitorati;
* abilitare o disabilitare il monitor;
* impostare il valore `MAX`;
* leggere le statistiche;
* visualizzare le liste correnti di UID, programmi e syscall registrate.

Le operazioni di modifica della configurazione sono consentite solo a processi con effective UID pari a `0`, come richiesto dalla specifica.

---

## Syscall Hooking Design

La gestione degli hook è implementata in `syscall_hook.c`.

Per ogni syscall registrata, il modulo salva il puntatore originale e sostituisce la corrispondente entry nella `sys_call_table` con un wrapper generico.

La struttura usata per rappresentare un hook è:

```c
struct syscall_hook {
    int nr;
    unsigned long original;
    bool active;
};
```

Dove:

* `nr` è il numero della syscall;
* `original` è il puntatore alla syscall originale;
* `active` indica se l'hook è attualmente attivo.

Gli hook sono salvati in un array indicizzato direttamente dal numero di syscall:

```c
static struct syscall_hook *hooks[MAX_HOOKS];
```

Questa scelta permette un lookup in tempo costante `O(1)`:

```text
nr syscall -> hooks[nr] -> puntatore originale
```

Il wrapper generico esegue le seguenti operazioni:

1. recupera il numero della syscall da `regs->orig_ax`;
2. recupera il puntatore originale salvato;
3. invoca la logica del monitor tramite `should_block(nr)`;
4. se il monitor consente la prosecuzione, invoca la syscall originale;
5. se il monitor restituisce un errore, ad esempio per segnale durante l'attesa, la syscall originale non viene invocata.

Il flusso logico è:

```text
userspace syscall
    -> x64_sys_call
    -> dispatcher patchato
    -> generic_syscall_hook()
    -> should_block()
    -> syscall originale
```

---

## Dispatcher Patch

Sui kernel x86-64 moderni non è sempre sufficiente modificare direttamente la `sys_call_table`, perché il dispatcher può non effettuare un normale dispatch indiretto tramite la tabella.

Per questo si applica una patch ai primi byte di `x64_sys_call`, inserendo un salto relativo verso una funzione custom `call()`.

La funzione `call()` recupera da `sys_call_table[nr]` il puntatore della syscall da eseguire e salta verso di essa tramite thunk indiretto:

```asm
mov (%1, %0, 8), %rax
jmp __x86_indirect_thunk_rax
```

Questa scelta consente di ripristinare un dispatch basato sulla syscall table, permettendo al modulo di intercettare le syscall registrate.

Prima di modificare codice kernel read-only, il modulo:

* disabilita temporaneamente la preemption;
* salva i registri `CR0` e `CR4`;
* rimuove temporaneamente il bit Write Protect da `CR0`;
* disabilita temporaneamente CET se presente;
* applica la patch;
* ripristina le protezioni originali.

Il modulo salva inoltre i byte originali sovrascritti, così da poter ripristinare `x64_sys_call` durante il cleanup.

---

## Throttling Algorithm

La logica di throttling è implementata in `monitor.c`.

Il monitor usa una finestra temporale di durata `WINDOW_MS`, pari a 1000 ms.

Le variabili principali sono:

```c
static unsigned long global_count;
static unsigned long window_generation;
static unsigned long max_calls;
static bool monitor_enabled;
```

* `global_count` indica quante syscall monitorate sono state lasciate passare nella finestra corrente;
* `window_generation` identifica la finestra temporale corrente;
* `max_calls` rappresenta il valore `MAX` configurato;
* `monitor_enabled` indica se il monitor è attivo.

Quando una syscall monitorata viene invocata, il monitor tenta di consumare uno slot tramite `try_consume_slot()`.

Se:

```text
global_count < max_calls
```

allora il contatore viene incrementato e la syscall può proseguire.

Se invece il limite è stato raggiunto, il thread viene messo in attesa su una wait queue tramite:

```c
wait_event_interruptible(...)
```

Il thread si risveglia quando:

* viene aperta una nuova finestra temporale;
* il monitor viene disabilitato;
* arriva un segnale.

---

## Timer and Window Generation

Il cambio finestra è gestito tramite un high-resolution timer (`hrtimer`).

A ogni scadenza del timer:

```c
global_count = 0;
window_generation++;
```

Successivamente vengono risvegliati alcuni thread in attesa tramite la wait queue.

`window_generation` funge da identificatore logico della finestra corrente. Ogni thread bloccato salva la generation corrente prima di andare in sleep. Al risveglio controlla se la generation è cambiata.

Questo evita di affidarsi esclusivamente al wakeup e permette di distinguere:

* un vero cambio finestra;
* un wakeup spurio;
* una disabilitazione del monitor;
* un'interruzione dovuta a segnale.

---

## Signal Handling During Throttling

Il thread bloccato usa `wait_event_interruptible()`, quindi può essere risvegliato da un segnale, ad esempio `SIGINT` generato da `Ctrl+C`.

Se l'attesa viene interrotta da un segnale, il monitor restituisce il codice d'errore ricevuto dalla wait. Il wrapper generico propaga tale errore e non invoca la syscall originale.

Questa scelta è coerente con la semantica del progetto: il thread bloccato non ha ancora eseguito la syscall, quindi se l'attesa viene interrotta la syscall non deve essere eseguita successivamente.

---

## Registry Design

Il registry mantiene tre insiemi distinti:

* syscall monitorate;
* UID monitorati;
* programmi monitorati.

### Syscall Registry

Le syscall monitorate sono rappresentate tramite bitmap.

Le operazioni di add/remove usano primitive atomiche:

```c
test_and_set_bit()
test_and_clear_bit()
```

Questo consente di evitare lock espliciti per la bitmap.

La lettura nel path caldo usa:

```c
test_bit()
```

quindi il controllo se una syscall è monitorata è molto efficiente.

### UID Registry

Gli UID monitorati sono memorizzati in una hash table.

La sincronizzazione usa:

* RCU per i lettori;
* spinlock per gli scrittori;
* `kfree_rcu()` per la liberazione sicura dei nodi rimossi.

Questa scelta ottimizza il path caldo, perché il lookup dell'UID può avvenire senza acquisire lock pesanti.

### Program Registry

I programmi sono registrati tramite la coppia:

```text
(device, inode)
```

anziché tramite il path testuale.

Questo evita problemi dovuti a path rinominati, link simbolici o ambiguità tra file con lo stesso nome.

Anche il registry dei programmi usa:

* RCU per i lettori;
* spinlock per gli scrittori;
* `kfree_rcu()` per la rimozione sicura.

---

## Program Identification

Quando un programma viene registrato, il path ricevuto da userspace viene risolto tramite `kern_path()`.

Da questo path vengono estratti:

```c
inode->i_sb->s_dev
inode->i_ino
```

Durante il runtime, quando una syscall viene invocata, il monitor recupera l'eseguibile del task corrente tramite `current->mm->exe_file` e ne estrae nuovamente `dev` e `ino`.

Il confronto avviene quindi tra coppie `(dev, ino)`.

La lettura dell'eseguibile corrente avviene dentro una sezione RCU per evitare problemi di concorrenza legati alla possibile sostituzione o liberazione delle strutture `mm` / `exe_file`.

---

## Synchronization Strategy

Il progetto usa diversi meccanismi di sincronizzazione in base al tipo di dato condiviso.

### Hook Table

La tabella interna degli hook è protetta tramite `rwlock`.

* il wrapper legge spesso il mapping syscall -> originale;
* add/remove hook avvengono raramente tramite ioctl.

Questo rende naturale l'uso di un read/write lock.

### Registry

Il registry UID/programmi usa RCU per le letture e spinlock per le modifiche.

Questo riduce l'overhead nel path caldo del monitor.

### Monitor Runtime State

`global_count` e `window_generation` sono protetti da spinlock, perché sono aggiornati sia dai thread che invocano syscall sia dalla callback del timer.

La sequenza:

```c
if (global_count < max_calls)
    global_count++;
```

è una read-modify-write e deve essere atomica.

### Lockless Flags

`monitor_enabled` e `max_calls` vengono letti nel path caldo senza acquisire lock. Per questo si usano `READ_ONCE()` e `WRITE_ONCE()`.

Queste primitive impediscono al compilatore di ottimizzare in modo non corretto letture e scritture concorrenti.

### Statistics

Le statistiche sono protette da uno spinlock dedicato `stats_lock`.

Questo garantisce consistenza tra campi correlati, come:

* peak delay;
* nome processo associato;
* UID associato;
* numero corrente e massimo di thread bloccati.

---

## Statistics

La specifica richiede di fornire informazioni relative a:

* peak delay per l'esecuzione effettiva di una syscall;
* programma e UID associati al peak delay;
* numero medio di thread bloccati;
* numero massimo di thread bloccati.

### Peak Delay

Il peak delay rappresenta il massimo tempo di attesa osservato tra:

```text
inizio blocco del thread
fine blocco del thread
```

Quando un thread bloccato ottiene finalmente uno slot oppure viene risvegliato per monitor disabilitato/segnale, viene calcolato il delay e confrontato con il massimo corrente.

### Average Blocked Threads

La media dei thread bloccati viene calcolata tramite integrazione temporale.

Il modulo mantiene:

```c
blocked_time_sum_ns
blocking_period_sum_ns
```

Dove:

* `blocked_time_sum_ns` rappresenta la somma di `tempo * thread_bloccati`;
* `blocking_period_sum_ns` rappresenta il tempo totale in cui almeno un thread era bloccato.

La media viene calcolata come:

```text
avg = blocked_time_sum_ns / blocking_period_sum_ns
```

Il valore viene scalato per 1000 per mantenere tre cifre decimali senza usare floating point nel kernel.

---

## Module Lifetime Protection

Il wrapper generico usa il reference counting del modulo tramite:

```c
try_module_get(THIS_MODULE)
module_put(THIS_MODULE)
```

Questo evita che il modulo venga scaricato mentre un thread sta ancora eseguendo codice del wrapper o del monitor.

Se il modulo è in uso, `rmmod` fallisce con modulo occupato.

---

# Build

Il progetto usa il sistema di build standard dei Linux Kernel Module tramite `kbuild`.

Il `Makefile` principale definisce:

```
obj-m += syscall_monitor.o

syscall_monitor-objs := \
    main.o \
    syscall_hook.o \
    probe.o \
    device.o \
    monitor.o \
    registry.o \
    stats.o
```

Il modulo viene compilato contro gli header del kernel corrente:

```
KDIR := /lib/modules/$(shell uname -r)/build
```

## Compilation

Per compilare il modulo:

```
make
```

Questo genera:

```
syscall_monitor.ko
```

------

## Module Loading

Per caricare il modulo nel kernel:

```
make load
```

oppure:

```
sudo insmod syscall_monitor.ko
```

------

## Module Removal

Per rimuovere il modulo:

```
make remove
```

oppure:

```
sudo rmmod syscall_monitor
```

------

## Full Rebuild

Per pulire, ricompilare e caricare automaticamente il modulo:

```
make complete
```

equivalente a:

```
make clean && make && make load
```

------

## Cleanup

Per rimuovere i file generati dalla compilazione:

```
make clean
```

Il comando elimina:

```
*.o
*.ko
*.mod
*.mod.c
*.symvers
*.order
```

------

## Kernel Headers

Il progetto richiede gli header del kernel installati.

```
sudo apt install linux-headers-$(uname -r)
```



## Userspace Configurator

Il progetto include un programma userspace per configurare il monitor tramite ioctl.

Esempio di uso:

```bash
sudo ./configurator
```

Operazioni tipiche:

```text
1. Add syscall number
2. Add UID
3. Set MAX calls per window
4. Enable monitor
5. Get stats
```

Esempio per monitorare `getpid`:

```text
Add syscall number: 39
Add UID: <uid>
Set MAX: 1
Enable monitor
```

---

## Testing

| Test                   | Scopo                                          | Syscall |
| ---------------------- | ---------------------------------------------- | ------- |
| test_basic.c           | verifica hooking base                          | write   |
| test2.c                | stress single-thread controllato               | getpid  |
| test_multithread.c     | concorrenza e statistiche thread bloccati      | getpid  |
| test_sleep.c           | unload del modulo e reference count            | getpid  |
| test_mkdir_interrupt.c | interruzione con Ctrl+C e syscall non eseguita | mkdir   |

### Module Unload Test

Per testare la protezione del modulo:

1. avviare un test che genera syscall monitorate;
2. provare a rimuovere il modulo con `rmmod`.

Se un thread è dentro il wrapper o il monitor, il modulo risulta in uso e non viene scaricato.

