#include <unistd.h>
#include <sys/syscall.h>

int main(void)
{
    while (1) {
        syscall(SYS_getpid);
    }
}