#include "daemon.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "daemonize: fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid > 0)
        _exit(0);               /* parent exits */

    if (setsid() < 0) {
        fprintf(stderr, "daemonize: setsid failed: %s\n", strerror(errno));
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "daemonize: second fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid > 0)
        _exit(0);

    umask(0);

    if (chdir("/") < 0) {
        fprintf(stderr, "daemonize: chdir failed: %s\n", strerror(errno));
        return -1;
    }

    /* close all std fds and reopen to /dev/null */
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    int fd = open("/dev/null", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "daemonize: open /dev/null: %s\n", strerror(errno));
        return -1;
    }
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
        close(fd);

    return 0;
}
