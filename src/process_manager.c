#include "process_manager.h"
#include "logger.h"
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdio.h>

int pm_init(ProcessManager *pm)
{
    memset(pm, 0, sizeof(*pm));
    pm->running     = 1;
    pm->restart_all = 0;
    pm->count       = 0;
    pm->sigchld_pending = 0;
    log_write(LOG_INFO, "Process manager initialised");
    return 0;
}

int pm_add(ProcessManager *pm, const char *name, const char *path,
           char *const argv[], int argc)
{
    if (!name || !path) {
        log_write(LOG_ERROR, "pm_add: NULL name or path");
        return -1;
    }
    if (pm->count >= MAX_PROCESSES) {
        log_write(LOG_ERROR, "pm_add: max processes (%d) reached", MAX_PROCESSES);
        return -1;
    }
    if (pm_get_index(pm, name) >= 0) {
        log_write(LOG_WARN, "pm_add: duplicate name '%s', ignored", name);
        return -1;
    }

    ManagedProcess *p = &pm->procs[pm->count];

    strncpy(p->name, name, NAME_MAX - 1);
    p->name[NAME_MAX - 1] = '\0';

    strncpy(p->path, path, PATH_MAX - 1);
    p->path[PATH_MAX - 1] = '\0';

    int n = argc < MAX_ARGS ? argc : MAX_ARGS;
    for (int i = 0; i < n; i++) {
        strncpy(p->argv[i], argv[i], NAME_MAX - 1);
        p->argv[i][NAME_MAX - 1] = '\0';
    }
    p->argc            = n;
    p->pid             = -1;
    p->state           = STATE_STOPPED;
    p->restart_count   = 0;
    p->max_restarts    = 5;
    p->restart_delay_ms = 2000;

    pm->count++;
    log_write(LOG_INFO, "Added process '%s' -> %s (%d args)", name, path, n);
    return 0;
}

static int launch_process(ManagedProcess *p)
{
    char *args[MAX_ARGS + 1];
    for (int i = 0; i < p->argc; i++)
        args[i] = p->argv[i];
    args[p->argc] = NULL;

    execvp(p->path, args);

    log_write(LOG_ERROR, "execvp '%s' failed: %s", p->name, strerror(errno));
    _exit(127);
}

int pm_start(ProcessManager *pm, const char *name)
{
    int idx = pm_get_index(pm, name);
    if (idx < 0) {
        log_write(LOG_ERROR, "start: unknown process '%s'", name);
        return -1;
    }

    ManagedProcess *p = &pm->procs[idx];

    if (p->state == STATE_RUNNING) {
        log_write(LOG_WARN, "start: '%s' already running (pid %d)", name, p->pid);
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        log_write(LOG_ERROR, "fork failed for '%s': %s", name, strerror(errno));
        return -1;
    }

    if (pid == 0)
        launch_process(p);

    p->pid   = pid;
    p->state = STATE_RUNNING;
    p->restart_count = 0;

    log_write(LOG_INFO, "Started '%s' (pid %d)", name, pid);
    return 0;
}

int pm_start_all(ProcessManager *pm)
{
    int started = 0;
    int errors  = 0;

    for (int i = 0; i < pm->count; i++) {
        if (pm_start(pm, pm->procs[i].name) == 0)
            started++;
        else
            errors++;
    }

    log_write(LOG_INFO, "start_all: %d started, %d errors", started, errors);
    return started;
}

int pm_stop(ProcessManager *pm, const char *name)
{
    int idx = pm_get_index(pm, name);
    if (idx < 0) {
        log_write(LOG_ERROR, "stop: unknown process '%s'", name);
        return -1;
    }

    ManagedProcess *p = &pm->procs[idx];

    if (p->state != STATE_RUNNING) {
        log_write(LOG_WARN, "stop: '%s' not running (state=%d)", name, p->state);
        return 0;
    }

    pid_t pid = p->pid;

    log_write(LOG_INFO, "Stopping '%s' (pid %d) with SIGTERM", name, pid);
    if (kill(pid, SIGTERM) < 0) {
        log_write(LOG_ERROR, "kill SIGTERM '%s': %s", name, strerror(errno));
    }

    int status;
    for (int waited = 0; waited < 50; waited++) {
        pid_t ret = waitpid(pid, &status, WNOHANG);
        if (ret == pid) {
            if (WIFEXITED(status))
                log_write(LOG_INFO, "'%s' exited with code %d", name, WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                log_write(LOG_INFO, "'%s' terminated by signal %d", name, WTERMSIG(status));
            p->pid   = -1;
            p->state = STATE_STOPPED;
            return 0;
        }
        usleep(10000);
    }

    log_write(LOG_WARN, "'%s' (pid %d) did not exit after SIGTERM, sending SIGKILL", name, pid);
    if (kill(pid, SIGKILL) < 0) {
        log_write(LOG_ERROR, "kill SIGKILL '%s': %s", name, strerror(errno));
        p->pid   = -1;
        p->state = STATE_STOPPED;
        return -1;
    }

    waitpid(pid, NULL, 0);
    p->pid   = -1;
    p->state = STATE_STOPPED;
    log_write(LOG_INFO, "'%s' force-killed with SIGKILL", name);
    return 0;
}

int pm_stop_all(ProcessManager *pm)
{
    int stopped = 0;
    int errors  = 0;

    for (int i = 0; i < pm->count; i++) {
        if (pm->procs[i].state == STATE_RUNNING) {
            if (pm_stop(pm, pm->procs[i].name) == 0)
                stopped++;
            else
                errors++;
        }
    }

    log_write(LOG_INFO, "stop_all: %d stopped, %d errors", stopped, errors);
    return stopped;
}

int pm_kill(ProcessManager *pm, const char *name)
{
    int idx = pm_get_index(pm, name);
    if (idx < 0) {
        log_write(LOG_ERROR, "kill: unknown process '%s'", name);
        return -1;
    }

    ManagedProcess *p = &pm->procs[idx];

    if (p->state != STATE_RUNNING)
        return 0;

    pid_t pid = p->pid;
    log_write(LOG_WARN, "Force-killing '%s' (pid %d) with SIGKILL", name, pid);

    if (kill(pid, SIGKILL) < 0) {
        log_write(LOG_ERROR, "kill SIGKILL '%s': %s", name, strerror(errno));
        return -1;
    }

    waitpid(pid, NULL, 0);
    p->pid   = -1;
    p->state = STATE_STOPPED;
    return 0;
}

int pm_restart(ProcessManager *pm, const char *name)
{
    int idx = pm_get_index(pm, name);
    if (idx < 0) {
        log_write(LOG_ERROR, "restart: unknown process '%s'", name);
        return -1;
    }

    ManagedProcess *p = &pm->procs[idx];
    int delay = p->restart_delay_ms > 0 ? p->restart_delay_ms : 500;

    log_write(LOG_INFO, "Restarting '%s' (delay=%dms)", name, delay);
    pm_stop(pm, name);
    usleep(delay * 1000);
    return pm_start(pm, name);
}

int pm_restart_all(ProcessManager *pm)
{
    log_write(LOG_INFO, "Restarting all processes");
    pm_stop_all(pm);
    usleep(RESTART_DELAY_US);
    return pm_start_all(pm);
}

void pm_check_children(ProcessManager *pm)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int found = 0;
        for (int i = 0; i < pm->count; i++) {
            if (pm->procs[i].pid != pid)
                continue;

            ManagedProcess *p = &pm->procs[i];
            found = 1;

            if (WIFEXITED(status)) {
                int code = WEXITSTATUS(status);
                log_write(LOG_INFO, "'%s' (pid %d) exited with code %d",
                          p->name, pid, code);
                p->state = (code == 0) ? STATE_STOPPED : STATE_CRASHED;
            } else if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                log_write(LOG_WARN, "'%s' (pid %d) killed by signal %d (%s)",
                          p->name, pid, sig, strsignal(sig));
                p->state = STATE_CRASHED;
            }

            p->pid = -1;

            if (p->state == STATE_CRASHED && pm->running) {
                if (p->restart_count < p->max_restarts) {
                    p->restart_count++;
                    p->state = STATE_RESTARTING;
                    int delay = p->restart_delay_ms * 1000;
                    log_write(LOG_WARN, "Auto-restarting '%s' (%d/%d, delay=%dms)",
                              p->name, p->restart_count, p->max_restarts,
                              p->restart_delay_ms);
                    usleep(delay);

                    pid_t new_pid = fork();
                    if (new_pid < 0) {
                        log_write(LOG_ERROR, "auto-restart fork '%s': %s",
                                  p->name, strerror(errno));
                        p->state = STATE_CRASHED;
                    } else if (new_pid == 0) {
                        launch_process(p);
                    } else {
                        p->pid   = new_pid;
                        p->state = STATE_RUNNING;
                        log_write(LOG_INFO, "Auto-restarted '%s' (pid %d, attempt %d)",
                                  p->name, new_pid, p->restart_count);
                    }
                } else {
                    log_write(LOG_ERROR, "'%s' exceeded max restarts (%d), giving up",
                              p->name, p->max_restarts);
                }
            }
            break;
        }

        if (!found)
            log_write(LOG_WARN, "Reaped unknown child pid %d", pid);
    }

    pm->sigchld_pending = 0;
}

void pm_shutdown(ProcessManager *pm)
{
    log_write(LOG_INFO, "Shutting down process manager");
    pm->running = 0;
    pm_stop_all(pm);
    log_write(LOG_INFO, "Process manager shutdown complete");
}

int pm_get_index(ProcessManager *pm, const char *name)
{
    if (!name)
        return -1;

    for (int i = 0; i < pm->count; i++) {
        if (strcmp(pm->procs[i].name, name) == 0)
            return i;
    }
    return -1;
}

void pm_print_status(ProcessManager *pm)
{
    char buf[2048];
    int  off = 0;

    off += snprintf(buf + off, sizeof(buf) - off,
                    "Process Manager Status (%d registered)\n", pm->count);
    off += snprintf(buf + off, sizeof(buf) - off,
                    "%-20s %-10s %-6s %s\n",
                    "NAME", "STATE", "PID", "RESTARTS");
    off += snprintf(buf + off, sizeof(buf) - off,
                    "%-20s %-10s %-6s %s\n",
                    "----", "-----", "---", "--------");

    for (int i = 0; i < pm->count; i++) {
        ManagedProcess *p = &pm->procs[i];
        const char *s;

        switch (p->state) {
            case STATE_STOPPED:    s = "STOPPED";   break;
            case STATE_RUNNING:    s = "RUNNING";   break;
            case STATE_CRASHED:    s = "CRASHED";   break;
            case STATE_RESTARTING: s = "RESTART";   break;
            default:               s = "UNKNOWN";   break;
        }

        off += snprintf(buf + off, sizeof(buf) - off,
                        "%-20s %-10s %-6d %d/%d\n",
                        p->name, s, (int)p->pid,
                        p->restart_count, p->max_restarts);
    }

    log_write(LOG_INFO, "Status report:\n%s", buf);
}
