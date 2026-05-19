#ifndef LOGGER_H
#define LOGGER_H

#define LOG_PATH "logs/manager.log"

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} LogLevel;

int  log_init(void);
void log_write(LogLevel level, const char *fmt, ...);
void log_close(void);

#endif
