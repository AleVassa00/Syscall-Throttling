#include <unistd.h>
#include <stdlib.h>
#include <sys/syscall.h>

int main(int argc, char **argv){
    int max = atoi (argv[1]);
    for (int i=0;i<max;i++){
        syscall(SYS_getpid);
    }
    return 0;
}