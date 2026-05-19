#define _POSIX_C_SOURCE 200809L

#include "signal_handler.h"
#include "logger.h"
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static ProcessManager *g_pm = NULL;
volatile sig_atomic_t  g_sigchld_flag = 0;

static void handle_sigchld(int sig)
{
    (void)sig;
    g_sigchld_flag = 1;
}

static void handle_sigint(int sig)
{
    (void)sig;
    if (g_pm)
        g_pm->running = 0;
}

static void handle_sigterm(int sig)
{
    (void)sig;
    if (g_pm)
        g_pm->running = 0;
}

static void handle_sighup(int sig)
{
    (void)sig;
    if (g_pm)
        g_pm->restart_all = 1;
}

static int setup_one(int signum, void (*handler)(int), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handler;
    sa.sa_flags   = flags;

    if (sigaction(signum, &sa, NULL) < 0) {
        log_write(LOG_ERROR, "sigaction(%d): %s", signum, strerror(errno));
        return -1;
    }
    return 0;
}

int signal_setup(ProcessManager *pm)
{
    g_pm = pm;
    g_sigchld_flag = 0;

    if (setup_one(SIGCHLD, handle_sigchld, SA_NOCLDSTOP | SA_RESTART) < 0)
        return -1;

    if (setup_one(SIGINT,  handle_sigint,  0) < 0)
        return -1;

    if (setup_one(SIGTERM, handle_sigterm, 0) < 0)
        return -1;

    if (setup_one(SIGHUP,  handle_sighup,  0) < 0)
        return -1;

    signal(SIGPIPE, SIG_IGN);

    log_write(LOG_INFO, "Signal handlers installed (signal-safe)");
    return 0;
}

void signal_teardown(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_DFL;

    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    signal(SIGPIPE, SIG_DFL);

    g_pm = NULL;
}
