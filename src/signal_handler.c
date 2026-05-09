#include "signal_handler.h"
#include "logger.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

static ProcessManager *g_pm = NULL;

static void handle_sigint(int sig)
{
    (void)sig;
    log_write(LOG_INFO, "Received SIGINT, shutting down");
    if (g_pm) pm_shutdown(g_pm);
}

static void handle_sigterm(int sig)
{
    (void)sig;
    log_write(LOG_INFO, "Received SIGTERM, shutting down");
    if (g_pm) pm_shutdown(g_pm);
}

static void handle_sigchld(int sig)
{
    (void)sig;
    if (g_pm) pm_check_children(g_pm);
}

static void handle_sighup(int sig)
{
    (void)sig;
    log_write(LOG_INFO, "Received SIGHUP, restarting all");
    if (g_pm) pm_restart_all(g_pm);
}

int signal_setup(ProcessManager *pm)
{
    g_pm = pm;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = handle_sigint;
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        log_write(LOG_ERROR, "sigaction SIGINT: %s", strerror(errno));
        return -1;
    }

    sa.sa_handler = handle_sigterm;
    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        log_write(LOG_ERROR, "sigaction SIGTERM: %s", strerror(errno));
        return -1;
    }

    sa.sa_handler = handle_sigchld;
    sa.sa_flags   = SA_NOCLDSTOP | SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        log_write(LOG_ERROR, "sigaction SIGCHLD: %s", strerror(errno));
        return -1;
    }

    sa.sa_handler = handle_sighup;
    sa.sa_flags   = 0;
    if (sigaction(SIGHUP, &sa, NULL) < 0) {
        log_write(LOG_ERROR, "sigaction SIGHUP: %s", strerror(errno));
        return -1;
    }

    log_write(LOG_INFO, "Signal handlers installed");
    return 0;
}

void signal_teardown(void)
{
    g_pm = NULL;
    log_write(LOG_INFO, "Signal handlers removed");
}
