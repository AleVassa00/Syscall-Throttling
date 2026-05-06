#include <stdio.h>
#include <unistd.h>

int main(void)
{
    int i;

    for (i = 0; i < 100000; i++) {
        getpid();

        if (i % 1000 == 0)
            printf("i=%d\n", i);
    }

    return 0;
}