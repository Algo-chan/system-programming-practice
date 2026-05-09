#include "logger.h"
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    if (log_init() != 0) {
        fprintf(stderr, "Failed to initialise logger\n");
        return 1;
    }

    log_write(LOG_DEBUG, "This is a DEBUG message");
    log_write(LOG_INFO,  "This is an INFO message");
    log_write(LOG_WARN,  "This is a WARN message");
    log_write(LOG_ERROR, "This is an ERROR message");
    log_write(LOG_FATAL, "This is a FATAL message");
    log_write(LOG_INFO,  "Process manager test complete");

    log_close();
    printf("Logger test complete. Check logs/manager.log\n");
    return 0;
}
