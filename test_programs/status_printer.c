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

int main(void)
{
    signal(SIGTERM, handle);
    signal(SIGINT,  handle);

    printf("status_printer[%d]: printing status every 2 seconds\n", getpid());

    int count = 0;
    while (running) {
        printf("status_printer[%d]: heartbeat %d\n", getpid(), count++);
        fflush(stdout);
        sleep(2);
    }

    printf("status_printer[%d]: exiting\n", getpid());
    return 0;
}
