#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/kdev_t.h>
#include <linux/errno.h>
#include <linux/rcupdate.h>
#include <linux/wait.h>
#include <linux/kernel.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <asm/unistd.h>


#include "monitor.h"
#include "registry.h"
#include "stats.h"

#define DEFAULT_MAX_CALLS 10
#define WINDOW_MS 1000

static bool monitor_enabled;
static unsigned long max_calls = DEFAULT_MAX_CALLS;

/* Global monitor counter */
static DEFINE_SPINLOCK(counter_lock);
static unsigned long global_count;
static unsigned long window_generation;

/* Wait queue + high-resolution timer */
static DECLARE_WAIT_QUEUE_HEAD(throttle_wq);
static struct hrtimer window_timer;


/*Recupera device e inode dell'eseguibile associato al task corrente
 La funzione legge current->mm->exe_file all'interno di una sezione RCU per evitare problemi di concorrenza
 legati alla possibile sostituzione o liberazione delle strutture mm/exe_file durante la lettura
 L'identificazione del programma avviene tramite:
 - device del filesystem
 - inode dell'eseguibile
*/
static int get_current_exe_inode(dev_t *dev, unsigned long *ino)
{
    struct inode *inode;

    if (!dev || !ino)
        return -EINVAL;

    rcu_read_lock();

    if (!current->mm || !current->mm->exe_file) {
        rcu_read_unlock();
        return -ENOENT;
    }

    inode = file_inode(current->mm->exe_file);
    if (!inode) {
        rcu_read_unlock();
        return -ENOENT;
    }

    *dev = inode->i_sb->s_dev;
    *ino = inode->i_ino;

    rcu_read_unlock();

    return 0;
}
/*Callback del timer ad alta risoluzione.
 La callback definisce il cambio di finestra temporale:
 - resetta il contatore globale
 - incrementa la generation corrente
 - riattiva il timer periodico
 I thread bloccati vengono risvegliati tramite wait queue.
*/

static enum hrtimer_restart window_timer_callback(struct hrtimer *t)
{
    unsigned long flags;

    spin_lock_irqsave(&counter_lock, flags);

    if (READ_ONCE(monitor_enabled)) {
        global_count = 0;
        window_generation++;

        hrtimer_forward_now(&window_timer, ms_to_ktime(WINDOW_MS));

        spin_unlock_irqrestore(&counter_lock, flags);

        wake_up_nr(&throttle_wq, (int)READ_ONCE(max_calls));

        return HRTIMER_RESTART;
    }

    spin_unlock_irqrestore(&counter_lock, flags);

    wake_up_all(&throttle_wq);

    return HRTIMER_NORESTART;
}
/*Tenta di consumare uno slot disponibile nella finestra corrente
 La funzione protegge global_count tramite spinlock perché:
 - più thread possono invocare syscall contemporaneamente
 - il timer può resettare il contatore in parallelo
 Restituisce:
 - 1 se lo slot viene assegnato
 - 0 se il limite MAX è stato raggiunto
*/

static int try_consume_slot(void)
{
    unsigned long flags;
    int allowed = 0;

    spin_lock_irqsave(&counter_lock, flags);

    if (global_count < READ_ONCE(max_calls)) {
        global_count++;
        allowed = 1;
    }

    spin_unlock_irqrestore(&counter_lock, flags);

    return allowed;
}

/*Abilita il monitor syscall throttling
 - resetta il contatore globale
 - avvia il timer periodico
 - inizializza una nuova finestra logica
*/

void monitor_enable(void)
{
    unsigned long flags;

    spin_lock_irqsave(&counter_lock, flags);

    if (READ_ONCE(monitor_enabled)) {
        spin_unlock_irqrestore(&counter_lock, flags);
        printk(KERN_INFO "[MONITOR] already ENABLED\n");
        return;
    }

    global_count = 0;
    window_generation++;
    WRITE_ONCE(monitor_enabled, true);

    hrtimer_start(&window_timer,ms_to_ktime(WINDOW_MS),HRTIMER_MODE_REL);

    spin_unlock_irqrestore(&counter_lock, flags);

    stats_on_monitor_start();

    printk(KERN_INFO "[MONITOR] ENABLED\n");
}
/*Disabilita il monitor syscall throttling
 - cancella il timer
 - risveglia eventuali thread bloccati
*/

void monitor_disable(void)
{
    unsigned long flags;

    spin_lock_irqsave(&counter_lock, flags);

    if (!READ_ONCE(monitor_enabled)) {
        spin_unlock_irqrestore(&counter_lock, flags);
        printk(KERN_INFO "[MONITOR] already DISABLED\n");
        return;
    }

    WRITE_ONCE(monitor_enabled, false);

    spin_unlock_irqrestore(&counter_lock, flags);

    hrtimer_cancel(&window_timer);
    wake_up_all(&throttle_wq);
    stats_on_monitor_stop();

    printk(KERN_INFO "[MONITOR] DISABLED\n");
}

/*Aggiorna dinamicamente il parametro MAX.
 MAX rappresenta il numero massimo di syscall consentite all'interno della finestra temporale corrente.
 Il cambio di configurazione provoca anche il reset delle statistiche runtime.
*/
void monitor_set_max(unsigned long val)
{
    if (val == 0)
        return;

    WRITE_ONCE(max_calls, val);

    stats_reset(READ_ONCE(monitor_enabled));
    printk(KERN_INFO "[MONITOR] MAX set to %lu, stats reset\n", val);
}



/*Verifica se il task corrente deve essere monitorato
 Il controllo avviene in due fasi:
 - verifica syscall monitorata
 - verifica UID oppure programma monitorato
 Restituisce:
 - 1 se il task deve essere sottoposto a throttling
 - 0 altrimenti
*/
static int monitor_match_current_task(int nr)
{
    kuid_t uid;
    dev_t dev;
    unsigned long ino;
    int ret;

    if (!is_syscall_monitored(nr))
        return 0;

    uid = current_euid();

    if (is_uid_monitored(uid)) {
        return 1;
    }

    ret = get_current_exe_inode(&dev, &ino);
    if (ret) {
        pr_info("[MONITOR] get_current_exe_inode failed ret=%d comm=%s\n",ret, current->comm);
        return 0;
    }

    if (is_prog_inode_monitored(dev, ino))
        return 1;

    return 0;
}



/*Implementa il meccanismo di throttling vero e proprio.
 Il thread:
 - tenta di consumare uno slot nella finestra corrente
 - se il limite è raggiunto entra in wait queue
 - viene risvegliato al cambio finestra
 La sincronizzazione utilizza:
 - wait queue
 - window_generation
 - timer periodico
 Le statistiche di blocco vengono aggiornate sia all'ingresso che all'uscita dalla fase di attesa.
*/
static int monitor_throttle_current(int nr)
{
    int was_blocked = 0;
    ktime_t block_start = ktime_get();
    unsigned long my_generation;
    int ret;

    while (READ_ONCE(monitor_enabled)) {

        if (try_consume_slot()) {

            if (was_blocked) {

                s64 delay_ns = ktime_to_ns(ktime_sub(ktime_get(), block_start));
                stats_on_block_end((u64)delay_ns, current_euid(), current->comm);
                pr_info("[THROTTLE] pid=%d syscall=%d waited_ms=%lld\n",current->pid, nr, delay_ns / 1000000);
            }

            return 0;
        }

        // prima volta che non trova slot
        if (!was_blocked) {

            unsigned long blocked_now, peak_now;
            was_blocked = 1;
            block_start = ktime_get();
            stats_on_block_start(&blocked_now, &peak_now);
            pr_info("[THROTTLE] pid=%d first_block blocked=%lu peak=%lu\n",current->pid,blocked_now,peak_now);
        }

        my_generation = READ_ONCE(window_generation);
        ret = wait_event_interruptible(throttle_wq,READ_ONCE(window_generation) != my_generation ||!READ_ONCE(monitor_enabled));
        if (ret) {

            if (was_blocked) {

                s64 delay_ns = ktime_to_ns(ktime_sub(ktime_get(), block_start));
                stats_on_block_end((u64)delay_ns, current_euid(), current->comm);
            }

            pr_info("[THROTTLE] interrupted pid=%d prog=%s\n",current->pid, current->comm);
            return ret;
        }
    }

    /* monitor disabilitato */
    if (was_blocked) {
        s64 delay_ns = ktime_to_ns(
            ktime_sub(ktime_get(), block_start));
        stats_on_block_end((u64)delay_ns, current_euid(), current->comm);
    }

    return 0;
}

/*Punto di ingresso principale del monitor.
 La funzione:
 - verifica se il monitor è attivo
 - controlla se il task corrente deve essere monitorato
 - applica eventualmente il throttling
 Viene invocata dal wrapper generico delle syscall hookate.
*/

int should_block(int nr)
{
    if (!READ_ONCE(monitor_enabled))
        return 0;

    if (!monitor_match_current_task(nr))
        return 0;

    return monitor_throttle_current(nr);
}


/*Inizializzazione Monitor 
 -Si utilizza una variabile booleana per tracciare lo stato del monitor inizializzata a false di default 
 -Definisco una variabile che rappresenta il numero massimo di chiamate all'interno della finestra settata di default a DEFAULT_MAX_CALLS
 -Inizializzo il contatore globale e finestra;
 -Setup del timer
 -Inizializzazione delle stats
*/


int monitor_init(void)
{
    WRITE_ONCE(monitor_enabled, false);
    WRITE_ONCE(max_calls, DEFAULT_MAX_CALLS);

    global_count = 0;
    window_generation = 0;

    hrtimer_setup(&window_timer,
                  window_timer_callback,
                  CLOCK_MONOTONIC,
                  HRTIMER_MODE_REL);

    stats_init();

    printk(KERN_INFO "[MONITOR] initialized default OFF max=%lu\n",
           READ_ONCE(max_calls));

    return 0;
}

/* CLEANUP MONITOR */


void monitor_cleanup(void)
{
    struct monitor_stats s;

    WRITE_ONCE(monitor_enabled, false);

    hrtimer_cancel(&window_timer);
    wake_up_all(&throttle_wq);

    stats_on_monitor_stop();
    stats_get(&s);

    printk(KERN_INFO "[MONITOR] cleanup\n");

    printk(KERN_INFO
       "[STATS] peak_delay_ns=%llu peak_delay_us=%llu peak_delay_ms=%llu "
       "peak_uid=%d peak_prog=%s "
       "avg_global=%llu.%03llu avg_throttle=%llu.%03llu "
       "total_blocked=%lu peak_blocked=%lu\n",
       s.peak_delay_ns,
       s.peak_delay_us,
       s.peak_delay_ms,
       s.peak_delay_uid,
       s.peak_delay_comm,
       s.avg_blocked_threads_global_x1000 / 1000,
       s.avg_blocked_threads_global_x1000 % 1000,
       s.avg_blocked_threads_during_throttle_x1000 / 1000,
       s.avg_blocked_threads_during_throttle_x1000 % 1000,
       s.blocked_threads_total,
       s.peak_blocked_threads
       );
}