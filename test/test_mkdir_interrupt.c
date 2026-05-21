#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

int main(void)
{
    int i = 0;
    char dirname[64];

    while (1) {
        snprintf(dirname, sizeof(dirname), "test_dir_%d", i++);

        printf("mkdir %s\n", dirname);
        mkdir(dirname, 0777);
    }

    return 0;
}