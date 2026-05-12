#include <unistd.h>
#include <sys/syscall.h>

int main(void)
{
    syscall(SYS_write, 1, "A\n", 2);
    syscall(SYS_write, 1, "B\n", 2);
    return 0;
}