#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(void)
{
    for (int i = 0; i < 50; i++) {
        syscall(SYS_getpid);
        printf("test %d\n", i);
        fflush(stdout);
    }

    return 0;
}