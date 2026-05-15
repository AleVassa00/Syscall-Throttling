#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <linux/sched.h>



#define IOCTL_ADD_SYSCALL           _IOW('a', 1, int)
#define IOCTL_REMOVE_SYSCALL        _IOW('a', 2, int)

#define IOCTL_ADD_UID               _IOW('a', 3, int)
#define IOCTL_REMOVE_UID            _IOW('a', 4, int)

#define IOCTL_ADD_PROGRAM_NAME      _IOW('a', 5, char *)
#define IOCTL_REMOVE_PROGRAM_NAME   _IOW('a', 6, char *)

#define IOCTL_ENABLE_MONITOR        _IO('a', 7)
#define IOCTL_DISABLE_MONITOR       _IO('a', 8)

#define IOCTL_SET_MAX               _IOW('a', 9, int)

#define IOCTL_LIST_UIDS      _IOR('a', 11, struct uid_list)
#define IOCTL_LIST_PROGRAMS  _IOR('a', 12, struct prog_list)
#define IOCTL_LIST_SYSCALLS  _IOR('a', 13, struct syscall_list)

#define IOCTL_GET_STATS _IOR('a', 10, struct monitor_stats)

#define MAX_UIDS     64
#define MAX_PROGS    64
#define MAX_SYSCALLS 512

struct monitor_stats {
    unsigned long long peak_delay_ns;
    unsigned long long peak_delay_us;
    unsigned long long peak_delay_ms;
    int peak_delay_uid;
    char peak_delay_comm[16];
    unsigned long blocked_threads_total;
    unsigned long currently_blocked;
    unsigned long peak_blocked_threads;
    unsigned long long blocked_time_sum_ns;  
    unsigned long long monitor_time_ns;      
    unsigned long avg_blocked_threads;
};


struct uid_list {
    int count;
    int uids[MAX_UIDS];
};

struct prog_entry {
    unsigned int major;
    unsigned int minor;
    unsigned long ino;
};

struct prog_list {
    int count;
    struct prog_entry entries[MAX_PROGS];
};

struct syscall_list {
    int count;
    int nrs[MAX_SYSCALLS];
};

static void clear_stdin_line(void)
{
    int c;

    while ((c = getchar()) != '\n' && c != EOF)
        ;
}

static int read_int(const char *prompt, int *value)
{
    printf("%s", prompt);
    fflush(stdout);

    if (scanf("%d", value) != 1) {
        clear_stdin_line();
        return -1;
    }

    clear_stdin_line();
    return 0;
}

static int read_string(const char *prompt, char *buf, size_t size)
{
    printf("%s", prompt);
    fflush(stdout);

    if (scanf("%4095s", buf) != 1) {
        clear_stdin_line();
        return -1;
    }

    clear_stdin_line();
    buf[size - 1] = '\0';

    return 0;
}

int main(void)
{
    int fd;
    int choice;
    int value;
    char path[PATH_MAX];

    fd = open("/dev/syscall_monitor", O_RDWR);
    if (fd < 0) {
        perror("open /dev/syscall_monitor");
        return 1;
    }

    while (1) {
        printf("\n=== SYSCALL MONITOR CONFIGURATOR ===\n");
        printf("1) Add syscall number\n");
        printf("2) Remove syscall number\n");
        printf("3) Add UID\n");
        printf("4) Remove UID\n");
        printf("5) Add program by executable path\n");
        printf("6) Remove program by executable path\n");
        printf("7) Enable monitor\n");
        printf("8) Disable monitor\n");
        printf("9) Set MAX calls per window\n");
        printf("10) Get stats\n");
        printf("11) List registered UIDs\n");
        printf("12) List registered IOCTL_LIST_PROGRAMS\n");
        printf("13) List registered syscalls\n"); 
        printf("14) Exit\n");   /* era 11 */
        if (read_int("Scelta: ", &choice) < 0) {
            printf("Input non valido\n");
            continue;
        }

        switch (choice) {
        case 1:
            if (read_int("Numero syscall da aggiungere: ", &value) < 0) {
                printf("Numero non valido\n");
                break;
            }

            if (ioctl(fd, IOCTL_ADD_SYSCALL, &value) < 0) {
                perror("ioctl ADD_SYSCALL");
            } else {
                printf("Syscall aggiunta: %d\n", value);
            }
            break;

        case 2:
            if (read_int("Numero syscall da rimuovere: ", &value) < 0) {
                printf("Numero non valido\n");
                break;
            }

            if (ioctl(fd, IOCTL_REMOVE_SYSCALL, &value) < 0) {
                perror("ioctl REMOVE_SYSCALL");
            } else {
                printf("Syscall rimossa: %d\n", value);
            }
            break;

        case 3:
            if (read_int("UID da aggiungere: ", &value) < 0 || value < 0) {
                printf("UID non valido\n");
                break;
            }

            if (ioctl(fd, IOCTL_ADD_UID, &value) < 0) {
                perror("ioctl ADD_UID");
            } else {
                printf("UID aggiunto: %d\n", value);
            }
            break;

        case 4:
            if (read_int("UID da rimuovere: ", &value) < 0 || value < 0) {
                printf("UID non valido\n");
                break;
            }

            if (ioctl(fd, IOCTL_REMOVE_UID, &value) < 0) {
                perror("ioctl REMOVE_UID");
            } else {
                printf("UID rimosso: %d\n", value);
            }
            break;

        case 5:
            if (read_string("Path assoluto programma da aggiungere: ",
                            path, sizeof(path)) < 0) {
                printf("Path non valido\n");
                break;
            }

            if (ioctl(fd, IOCTL_ADD_PROGRAM_NAME, path) < 0) {
                perror("ioctl ADD_PROGRAM_NAME");
            } else {
                printf("Programma aggiunto: %s\n", path);
            }
            break;

        case 6:
            if (read_string("Path assoluto programma da rimuovere: ",
                            path, sizeof(path)) < 0) {
                printf("Path non valido\n");
                break;
            }

            if (ioctl(fd, IOCTL_REMOVE_PROGRAM_NAME, path) < 0) {
                perror("ioctl REMOVE_PROGRAM_NAME");
            } else {
                printf("Programma rimosso: %s\n", path);
            }
            break;

        case 7:
            if (ioctl(fd, IOCTL_ENABLE_MONITOR) < 0) {
                perror("ioctl ENABLE_MONITOR");
            } else {
                printf("Monitor abilitato\n");
            }
            break;

        case 8:
            if (ioctl(fd, IOCTL_DISABLE_MONITOR) < 0) {
                perror("ioctl DISABLE_MONITOR");
            } else {
                printf("Monitor disabilitato\n");
            }
            break;

        case 9:
            if (read_int("Nuovo MAX chiamate per finestra: ", &value) < 0 ||
                value <= 0) {
                printf("MAX non valido\n");
                break;
            }

            if (ioctl(fd, IOCTL_SET_MAX, &value) < 0) {
                perror("ioctl SET_MAX");
            } else {
                printf("MAX impostato a: %d\n", value);
            }
            break;
        case 10:
        {
            struct monitor_stats stats;

            if (ioctl(fd, IOCTL_GET_STATS, &stats) < 0) {
            perror("ioctl GET_STATS");
            } else {
                printf("\n=== MONITOR STATS ===\n");
                printf("Peak delay: %llu ns (%llu us, %llu ms)\n",
                    stats.peak_delay_ns,
                    stats.peak_delay_us,
                    stats.peak_delay_ms);

                printf("Peak delay process: %s\n", stats.peak_delay_comm);
                printf("Peak delay UID: %d\n", stats.peak_delay_uid);

                printf("Blocked total: %lu\n", stats.blocked_threads_total);
                printf("Currently blocked: %lu\n", stats.currently_blocked);
                printf("Peak blocked threads: %lu\n", stats.peak_blocked_threads);
                printf("Average blocked threads: %lu\n", stats.avg_blocked_threads);
            }

            break;
        }
        case 11: {
            struct uid_list list;
            int i;

            if (ioctl(fd, IOCTL_LIST_UIDS, &list) < 0) {
                perror("ioctl LIST_UIDS");
                break;
            }

            printf("\n=== REGISTERED UIDs (%d) ===\n", list.count);

            if (list.count == 0) {
                printf("  (none)\n");
                break;
            }

            for (i = 0; i < list.count; i++)
                printf("  [%d] UID=%d\n", i, list.uids[i]);

            break;
        }

        case 12: {
            struct prog_list list;
            int i;

            if (ioctl(fd, IOCTL_LIST_PROGRAMS, &list) < 0) {
                perror("ioctl LIST_PROGRAMS");
                break;
            }

            printf("\n=== REGISTERED PROGRAMS (%d) ===\n", list.count);

            if (list.count == 0) {
                printf("  (none)\n");
                break;
            }

            for (i = 0; i < list.count; i++)
                printf("  [%d] dev=%u:%u ino=%lu\n",
                       i,
                       list.entries[i].major,
                       list.entries[i].minor,
                       list.entries[i].ino);

            break;
        }

        case 13: {
            struct syscall_list list;
            int i;

            if (ioctl(fd, IOCTL_LIST_SYSCALLS, &list) < 0) {
                perror("ioctl LIST_SYSCALLS");
                break;
            }

            printf("\n=== REGISTERED SYSCALLS (%d) ===\n", list.count);

            if (list.count == 0) {
                printf("  (none)\n");
                break;
            }

            for (i = 0; i < list.count; i++)
                printf("  [%d] nr=%d\n", i, list.nrs[i]);

            break;
        }

        case 14:
            close(fd);
            return 0;

        default:
            printf("Scelta non valida\n");
            break;
        }
    }

    close(fd);
    return 0;
}