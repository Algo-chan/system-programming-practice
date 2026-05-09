#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>

static volatile int running = 1;

static void handle(int sig)
{
    (void)sig;
    running = 0;
}

int main(int argc, char **argv)
{
    int seconds = 60;
    if (argc > 1)
        seconds = atoi(argv[1]);

    signal(SIGTERM, handle);
    signal(SIGINT, handle);

    printf("sleeper[%d]: running for %d seconds\n", getpid(), seconds);

    while (running && seconds > 0) {
        sleep(1);
        seconds--;
    }

    printf("sleeper[%d]: exiting\n", getpid());
    return 0;
}
