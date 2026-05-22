#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

#define BASE_DIR "mkdir_test_dirs"

int main(void)
{
    int i = 0;
    char dirname[256];
    if (mkdir(BASE_DIR, 0777) < 0 && errno != EEXIST) {
        perror("mkdir base dir");
        return 1;
    }

    while (1) {
        snprintf(dirname, sizeof(dirname), "%s/test_dir_%d", BASE_DIR, i++);

        printf("mkdir %s\n", dirname);

        if (mkdir(dirname, 0777) < 0) {
            perror("mkdir");
        }
    }

    return 0;
}