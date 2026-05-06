#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <limits.h>

#define IOCTL_ADD_SYSCALL           _IOW('a', 1, int)
#define IOCTL_REMOVE_SYSCALL        _IOW('a', 2, int)

#define IOCTL_ADD_UID               _IOW('a', 3, int)
#define IOCTL_REMOVE_UID            _IOW('a', 4, int)

#define IOCTL_ADD_PROGRAM_NAME      _IOW('a', 5, char *)
#define IOCTL_REMOVE_PROGRAM_NAME   _IOW('a', 6, char *)

#define IOCTL_ENABLE_MONITOR        _IO('a', 7)
#define IOCTL_DISABLE_MONITOR       _IO('a', 8)

#define IOCTL_SET_MAX               _IOW('a', 9, int)

static int read_int(const char *prompt, int *value)
{
    printf("%s", prompt);

    if (scanf("%d", value) != 1) {
        while (getchar() != '\n');
        return -1;
    }

    return 0;
}

static int read_string(const char *prompt, char *buf, size_t size)
{
    printf("%s", prompt);

    if (scanf("%4095s", buf) != 1)
        return -1;

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
        printf("9) Set MAX calls per second\n");
        printf("10) Exit\n");
        printf("Scelta: ");

        if (scanf("%d", &choice) != 1) {
            printf("Input non valido\n");
            while (getchar() != '\n');
            continue;
        }

        switch (choice) {

        case 1:
            if (read_int("Numero syscall da aggiungere: ", &value) < 0) {
                printf("Numero non valido\n");
                break;
            }

            if (ioctl(fd, IOCTL_ADD_SYSCALL, &value) < 0)
                perror("ioctl ADD_SYSCALL");
            else
                printf("Syscall aggiunta: %d\n", value);

            break;

        case 2:
            if (read_int("Numero syscall da rimuovere: ", &value) < 0) {
                printf("Numero non valido\n");
                break;
            }

            if (ioctl(fd, IOCTL_REMOVE_SYSCALL, &value) < 0)
                perror("ioctl REMOVE_SYSCALL");
            else
                printf("Syscall rimossa: %d\n", value);

            break;

        case 3:
            if (read_int("UID da aggiungere: ", &value) < 0 || value < 0) {
                printf("UID non valido\n");
                break;
            }

            if (ioctl(fd, IOCTL_ADD_UID, &value) < 0)
                perror("ioctl ADD_UID");
            else
                printf("UID aggiunto: %d\n", value);

            break;

        case 4:
            if (read_int("UID da rimuovere: ", &value) < 0 || value < 0) {
                printf("UID non valido\n");
                break;
            }

            if (ioctl(fd, IOCTL_REMOVE_UID, &value) < 0)
                perror("ioctl REMOVE_UID");
            else
                printf("UID rimosso: %d\n", value);

            break;

        case 5:
            if (read_string("Path assoluto programma da aggiungere: ",
                            path, sizeof(path)) < 0) {
                printf("Path non valido\n");
                break;
            }

            if (ioctl(fd, IOCTL_ADD_PROGRAM_NAME, path) < 0)
                perror("ioctl ADD_PROGRAM_NAME");
            else
                printf("Programma aggiunto: %s\n", path);

            break;

        case 6:
            if (read_string("Path assoluto programma da rimuovere: ",
                            path, sizeof(path)) < 0) {
                printf("Path non valido\n");
                break;
            }

            if (ioctl(fd, IOCTL_REMOVE_PROGRAM_NAME, path) < 0)
                perror("ioctl REMOVE_PROGRAM_NAME");
            else
                printf("Programma rimosso: %s\n", path);

            break;

        case 7:
            if (ioctl(fd, IOCTL_ENABLE_MONITOR) < 0)
                perror("ioctl ENABLE_MONITOR");
            else
                printf("Monitor abilitato\n");

            break;

        case 8:
            if (ioctl(fd, IOCTL_DISABLE_MONITOR) < 0)
                perror("ioctl DISABLE_MONITOR");
            else
                printf("Monitor disabilitato\n");

            break;

        case 9:
            if (read_int("Nuovo MAX chiamate/sec: ", &value) < 0 || value <= 0) {
                printf("MAX non valido\n");
                break;
            }

            if (ioctl(fd, IOCTL_SET_MAX, &value) < 0)
                perror("ioctl SET_MAX");
            else
                printf("MAX impostato a: %d\n", value);

            break;

        case 10:
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