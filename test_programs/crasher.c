#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

int main(void)
{
    srand(time(NULL) ^ getpid());
    int lifetime = 3 + (rand() % 5);

    printf("crasher[%d]: will crash in %d seconds\n", getpid(), lifetime);
    sleep(lifetime);

    printf("crasher[%d]: crashing now (SIGSEGV)\n", getpid());
    fflush(stdout);

    int *p = NULL;
    *p = 42;

    return 1;
}
