#include <stdio.h>
#include <unistd.h>

int main(void)
{
    while (1) {
        getpid();
        getpid();   
        sleep(5);   
    }
}