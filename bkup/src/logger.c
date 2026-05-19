#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>

static FILE *log_file = NULL;
static const char *level_str[] = {
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR",
    "FATAL"
};

int log_init(void)
{
    log_file = fopen(LOG_PATH, "a");
    if (!log_file) {
        fprintf(stderr, "log_init: failed to open %s: %s\n",
                LOG_PATH, strerror(errno));
        return -1;
    }
    setbuf(log_file, NULL);
    log_write(LOG_INFO, "Logger initialised");
    return 0;
}

void log_write(LogLevel level, const char *fmt, ...)
{
    if (!log_file) return;

    time_t     now = time(NULL);
    struct tm *tm  = localtime(&now);
    char       timestamp[64];
    strftime(timestamp, sizeof timestamp, "%Y-%m-%d %H:%M:%S", tm);

    fprintf(log_file, "[%s] [%s] ", timestamp, level_str[level]);

    va_list args;
    va_start(args, fmt);
    vfprintf(log_file, fmt, args);
    va_end(args);

    fprintf(log_file, "\n");
}

void log_close(void)
{
    if (log_file) {
        log_write(LOG_INFO, "Logger shutting down");
        fclose(log_file);
        log_file = NULL;
    }
}
