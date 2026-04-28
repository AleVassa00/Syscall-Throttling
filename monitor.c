#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/wait.h>
#include <linux/atomic.h>

#include "monitor.h"

#define WINDOW_NS 1000000000ULL

static atomic_t counter;
static u64 window_start;

static int max_calls = 100;
static int monitor_enabled = 0;

static wait_queue_head_t wait_queue;

// STATS
static atomic_t blocked_threads;
static int peak_blocked;
static u64 total_blocked;
static u64 block_events;
static u64 peak_delay;

// ==============================

int monitor_init(void) {
    atomic_set(&counter, 0);
    window_start = ktime_get_ns();
    init_waitqueue_head(&wait_queue);

    atomic_set(&blocked_threads, 0);
    peak_blocked = 0;
    total_blocked = 0;
    block_events = 0;
    peak_delay = 0;

    return 0;
}

void monitor_cleanup(void) {}

// ==============================

void monitor_enable(void) { monitor_enabled = 1; }
void monitor_disable(void) { monitor_enabled = 0; }
int monitor_is_enabled(void) { return monitor_enabled; }

void monitor_set_max(int max) { max_calls = max; }

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

void monitor_block_current(void) {

    u64 start = ktime_get_ns();

    atomic_inc(&blocked_threads);

    if (atomic_read(&blocked_threads) > peak_blocked)
        peak_blocked = atomic_read(&blocked_threads);

    wait_event_interruptible(wait_queue,
        atomic_read(&counter) < max_calls
    );

    atomic_dec(&blocked_threads);

    u64 end = ktime_get_ns();
    u64 delay = end - start;

    if (delay > peak_delay)
        peak_delay = delay;

    total_blocked += atomic_read(&blocked_threads);
    block_events++;
}

// ==============================

unsigned long long get_peak_delay(void) { return peak_delay; }
int get_peak_blocked(void) { return peak_blocked; }

unsigned long long get_avg_blocked(void) {
    if (block_events == 0) return 0;
    return total_blocked / block_events;
}