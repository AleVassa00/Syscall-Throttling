#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/wait.h>
#include <linux/atomic.h>

#include "monitor.h"

#define MODNAME "syscall_monitor"
#define WINDOW_NS 1000000000ULL // 1 secondo

// ==============================
// GLOBAL STATE
// ==============================

static atomic_t counter; //sto settando un contatore globale
static u64 window_start;

static int max_calls = 100;
static int monitor_enabled = 0;

static wait_queue_head_t wait_queue;

// ==============================
// INIT
// ==============================

int monitor_init(void) {
    atomic_set(&counter, 0);
    window_start = ktime_get_ns();
    init_waitqueue_head(&wait_queue);
    monitor_enabled = 0; //parto col monitor disabilitato

    printk(KERN_INFO "%s: monitor initialized\n", MODNAME);
    return 0;
}

// ==============================
// CONTROL
// ==============================

void monitor_enable(void)
{
    monitor_enabled = 1;
}

void monitor_disable(void)
{
    monitor_enabled = 0;
}
int monitor_is_enabled(void) {
    return monitor_enabled;
}

void monitor_set_max(int max) {
    max_calls = max;
}

// ==============================
// LOGIC
// ==============================

int monitor_should_throttle(void) {

    u64 now = ktime_get_ns();

    if (now - window_start >= WINDOW_NS) {
        window_start = now;
        atomic_set(&counter, 0);
        wake_up_all(&wait_queue);
    }

    if (atomic_read(&counter) >= max_calls)
        return 1;

    atomic_inc(&counter);
    return 0;
}

// ==============================
// BLOCK
// ==============================

void monitor_block_current(void) {
    wait_event_interruptible(wait_queue,
        atomic_read(&counter) < max_calls
    );
}
void monitor_cleanup(void) {
    printk(KERN_INFO "%s: monitor cleaned\n", MODNAME);
}