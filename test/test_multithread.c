#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <pthread.h>

#define NUM_THREADS 21

static void *worker(void *arg)
{
    long id = (long)arg;

    syscall(SYS_getpid);

    printf("[THREAD %02ld] done\n", id);

    return NULL;
}

int main(void)
{
    pthread_t threads[NUM_THREADS];

    printf("[TEST] creating %d threads\n", NUM_THREADS);

    for (long i = 0; i < NUM_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, worker, (void *)i) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    printf("[TEST] completed\n");

    return 0;
}