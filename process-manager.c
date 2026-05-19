#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_PROCESSES     32
#define NAME_MAX          64
#define PATH_MAX          256
#define MAX_ARGS          8
#define RESTART_DELAY_US  500000
#define LOG_PATH          "logs/manager.log"
#define MAX_LOG_LINE      4096

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} LogLevel;

typedef enum {
    STATE_STOPPED    = 0,
    STATE_RUNNING,
    STATE_CRASHED,
    STATE_RESTARTING
} ProcessState;

typedef struct {
    char         name[NAME_MAX];
    char         path[PATH_MAX];
    char         argv[MAX_ARGS][NAME_MAX];
    int          argc;
    pid_t        pid;
    ProcessState state;
    int          restart_count;
    int          max_restarts;
    int          restart_delay_ms;
    int          auto_built;
} ManagedProcess;

typedef struct {
    ManagedProcess procs[MAX_PROCESSES];
    int            count;
    int            running;
    int            restart_all;
    int            verbose;
} ProcessManager;

static ProcessManager *g_pm = NULL;
volatile sig_atomic_t  g_sigchld_flag = 0;
static FILE *log_file = NULL;
static int  g_verbose = 0;
static const char *level_str[] = {
    "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};
static const char *state_str[] = {
    "STOPPED", "RUNNING", "CRASHED", "RESTARTING"
};

static int  log_init(void);
static void log_write(LogLevel level, const char *fmt, ...);
static void log_close(void);
static int  daemonize(void);
static int  pm_init(ProcessManager *pm);
static int  pm_add(ProcessManager *pm, const char *name, const char *path,
            char *const argv[], int argc);
static int  pm_start(ProcessManager *pm, const char *name);
static int  pm_start_all(ProcessManager *pm);
static int  pm_stop(ProcessManager *pm, const char *name);
static int  pm_stop_all(ProcessManager *pm);
static int  pm_kill(ProcessManager *pm, const char *name);
static int  pm_restart(ProcessManager *pm, const char *name);
static int  pm_restart_all(ProcessManager *pm);
static void pm_check_children(ProcessManager *pm);
static void pm_shutdown(ProcessManager *pm);
static int  pm_get_index(ProcessManager *pm, const char *name);
static void pm_print_status(ProcessManager *pm);
static int  config_load(ProcessManager *pm, const char *path);
static int  signal_setup(ProcessManager *pm);
static void signal_teardown(void);
static int  ensure_executable(ManagedProcess *p);
static int  auto_build(const char *src_path, const char *out_path);
static void write_log_safe(int fd, const char *msg);

static int log_init(void)
{
    log_file = fopen(LOG_PATH, "a");
    if (!log_file) {
        fprintf(stderr, "[FATAL] log_init: failed to open %s: %s\n",
                LOG_PATH, strerror(errno));
        return -1;
    }
    setbuf(log_file, NULL);
    log_write(LOG_INFO, "=== Process Manager started ===");
    return 0;
}

static void log_write(LogLevel level, const char *fmt, ...)
{
    if (!log_file && level > LOG_DEBUG) {
        fprintf(stderr, "[NO_LOG] ");
        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        fprintf(stderr, "\n");
        return;
    }

    if (level == LOG_DEBUG && !g_verbose)
        return;

    time_t     now = time(NULL);
    struct tm *tm  = localtime(&now);
    char       timestamp[64];
    strftime(timestamp, sizeof timestamp, "%Y-%m-%d %H:%M:%S", tm);

    char logbuf[MAX_LOG_LINE];
    int  len = snprintf(logbuf, sizeof(logbuf),
                        "[%s] [%s] ", timestamp, level_str[level]);

    va_list args;
    va_start(args, fmt);
    len += vsnprintf(logbuf + len, sizeof(logbuf) - len, fmt, args);
    va_end(args);

    if (log_file) {
        fprintf(log_file, "%s\n", logbuf);
    }

    if (level <= LOG_WARN) {
        fprintf(stderr, "%s\n", logbuf);
    }
}

static void log_close(void)
{
    if (log_file) {
        log_write(LOG_INFO, "=== Process Manager shutting down ===");
        fclose(log_file);
        log_file = NULL;
    }
}

static int daemonize(void)
{
    log_write(LOG_DEBUG, "daemonize: first fork()");
    pid_t pid = fork();
    if (pid < 0) {
        log_write(LOG_ERROR, "daemonize: first fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid > 0) {
        log_write(LOG_DEBUG, "daemonize: parent (pid %d) exiting", getpid());
        _exit(0);
    }

    log_write(LOG_DEBUG, "daemonize: creating new session with setsid()");
    if (setsid() < 0) {
        log_write(LOG_ERROR, "daemonize: setsid failed: %s", strerror(errno));
        return -1;
    }

    log_write(LOG_DEBUG, "daemonize: second fork()");
    pid = fork();
    if (pid < 0) {
        log_write(LOG_ERROR, "daemonize: second fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid > 0) {
        log_write(LOG_DEBUG, "daemonize: intermediate parent (pid %d) exiting", getpid());
        _exit(0);
    }

    umask(0);

    log_write(LOG_DEBUG, "daemonize: changing to root directory");
    if (chdir("/") < 0) {
        log_write(LOG_ERROR, "daemonize: chdir(/) failed: %s", strerror(errno));
        return -1;
    }

    log_write(LOG_DEBUG, "daemonize: closing stdin/stdout/stderr");
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    int fd = open("/dev/null", O_RDWR);
    if (fd < 0) {
        log_write(LOG_ERROR, "daemonize: open /dev/null failed: %s", strerror(errno));
        return -1;
    }
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
        close(fd);

    log_write(LOG_INFO, "Daemonized successfully (pid %d)", getpid());
    return 0;
}

static int pm_init(ProcessManager *pm)
{
    memset(pm, 0, sizeof(*pm));
    pm->running     = 1;
    pm->restart_all = 0;
    pm->count       = 0;
    pm->verbose     = g_verbose;
    log_write(LOG_INFO, "Process manager initialised (max %d processes)", MAX_PROCESSES);
    return 0;
}

static int pm_add(ProcessManager *pm, const char *name, const char *path,
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
    p->argc             = n;
    p->pid              = -1;
    p->state            = STATE_STOPPED;
    p->restart_count    = 0;
    p->max_restarts     = 5;
    p->restart_delay_ms = 2000;
    p->auto_built       = 0;

    pm->count++;
    log_write(LOG_INFO, "Added process '%s' -> %s (%d args, max_restarts=%d, delay=%dms)",
              name, path, n, p->max_restarts, p->restart_delay_ms);
    return 0;
}

static int ensure_executable(ManagedProcess *p)
{
    struct stat st;

    log_write(LOG_DEBUG, "ensure_executable: checking '%s'", p->path);

    if (access(p->path, F_OK) == 0) {
        if (access(p->path, X_OK) == 0) {
            log_write(LOG_DEBUG, "ensure_executable: '%s' exists and is executable", p->path);
            return 0;
        }
        log_write(LOG_ERROR, "ensure_executable: '%s' exists but is NOT executable", p->path);
        return -1;
    }

    log_write(LOG_WARN, "ensure_executable: '%s' not found, checking for source...", p->path);

    size_t path_len = strlen(p->path);
    if (path_len < 3) {
        log_write(LOG_ERROR, "ensure_executable: path too short: '%s'", p->path);
        return -1;
    }

    char src_path[PATH_MAX];
    strncpy(src_path, p->path, sizeof(src_path) - 1);
    src_path[sizeof(src_path) - 1] = '\0';

    char *dot = strrchr(src_path, '.');
    if (!dot) {
        log_write(LOG_DEBUG, "ensure_executable: no extension in path, appending .c");
        size_t slen = strlen(src_path);
        if (slen + 3 > sizeof(src_path) - 1) {
            log_write(LOG_ERROR, "ensure_executable: path too long for .c suffix");
            return -1;
        }
        strcat(src_path, ".c");
    } else {
        size_t ext_len = strlen(dot);
        if (ext_len >= 2 && ext_len <= 4) {
            strcpy(dot, ".c");
        } else {
            log_write(LOG_DEBUG, "ensure_executable: unrecognised extension, trying with .c suffix");
            size_t slen = strlen(src_path);
            if (slen + 3 > sizeof(src_path) - 1) {
                log_write(LOG_ERROR, "ensure_executable: path too long");
                return -1;
            }
            strcat(src_path, ".c");
        }
    }

    log_write(LOG_DEBUG, "ensure_executable: looking for source '%s'", src_path);

    if (access(src_path, F_OK) != 0) {
        log_write(LOG_ERROR, "ensure_executable: neither '%s' nor '%s' exist",
                  p->path, src_path);
        return -1;
    }

    log_write(LOG_INFO, "ensure_executable: auto-building '%s' from '%s'", p->path, src_path);

    int ret = auto_build(src_path, p->path);
    if (ret == 0) {
        p->auto_built = 1;
        log_write(LOG_INFO, "ensure_executable: auto-build SUCCESS for '%s'", p->path);
        return 0;
    }

    log_write(LOG_ERROR, "ensure_executable: auto-build FAILED for '%s'", p->path);
    return -1;
}

static int auto_build(const char *src_path, const char *out_path)
{
    char cmd[PATH_MAX + 64];
    int  n = snprintf(cmd, sizeof(cmd), "gcc -Wall -O2 -std=c11 -o '%s' '%s' 2>&1",
                      out_path, src_path);
    if (n < 0 || (size_t)n >= sizeof(cmd)) {
        log_write(LOG_ERROR, "auto_build: command too long");
        return -1;
    }

    log_write(LOG_INFO, "auto_build: running: %s", cmd);

    int status = system(cmd);
    if (status == -1) {
        log_write(LOG_ERROR, "auto_build: system() failed: %s", strerror(errno));
        return -1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        if (access(out_path, X_OK) == 0) {
            log_write(LOG_DEBUG, "auto_build: compiled executable verified: '%s'", out_path);
            return 0;
        }
        log_write(LOG_ERROR, "auto_build: executable '%s' not found after successful compile",
                  out_path);
        return -1;
    }

    log_write(LOG_ERROR, "auto_build: compilation failed with status %d", status);
    return -1;
}

static void launch_process(ManagedProcess *p)
{
    char child_log[256];
    int  child_log_len;

    child_log_len = snprintf(child_log, sizeof(child_log),
                             "[CHILD %s] Entered launch_process (pid %d, ppid %d)\n",
                             p->name, getpid(), getppid());
    write_log_safe(STDERR_FILENO, child_log);

    char *args[MAX_ARGS + 1];
    for (int i = 0; i < p->argc; i++)
        args[i] = p->argv[i];
    args[p->argc] = NULL;

    child_log_len = snprintf(child_log, sizeof(child_log),
                             "[CHILD %s] Calling execvp(path='%s', argv[0]='%s')\n",
                             p->name, p->path, args[0]);
    write_log_safe(STDERR_FILENO, child_log);

    execvp(p->path, args);

    int saved_errno = errno;

    child_log_len = snprintf(child_log, sizeof(child_log),
                             "[CHILD %s] execvp FAILED: %s (errno=%d)\n",
                             p->name, strerror(saved_errno), saved_errno);
    write_log_safe(STDERR_FILENO, child_log);

    fprintf(stderr, "[FATAL] [CHILD %s] execvp('%s'): %s\n",
            p->name, p->path, strerror(saved_errno));

    _exit(127);
}

static void write_log_safe(int fd, const char *msg)
{
    size_t len = strlen(msg);
    size_t written = 0;
    while (written < len) {
        ssize_t ret = write(fd, msg + written, len - written);
        if (ret <= 0)
            break;
        written += (size_t)ret;
    }
}

static int pm_start(ProcessManager *pm, const char *name)
{
    int idx = pm_get_index(pm, name);
    if (idx < 0) {
        log_write(LOG_ERROR, "pm_start: unknown process '%s'", name);
        return -1;
    }

    ManagedProcess *p = &pm->procs[idx];

    if (p->state == STATE_RUNNING) {
        log_write(LOG_WARN, "pm_start: '%s' already running (pid %d)", name, p->pid);
        return 0;
    }

    if (ensure_executable(p) != 0) {
        log_write(LOG_ERROR, "pm_start: '%s' cannot be launched - executable not available", name);
        p->state = STATE_STOPPED;
        return -1;
    }

    log_write(LOG_DEBUG, "pm_start: Forking process '%s'", name);
    pid_t pid = fork();
    if (pid < 0) {
        log_write(LOG_ERROR, "pm_start: fork failed for '%s': %s", name, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        log_write(LOG_DEBUG, "pm_start: CHILD process for '%s' (pid %d)", name, getpid());
        launch_process(p);
    }

    log_write(LOG_DEBUG, "pm_start: PARENT received child PID %d for '%s'", pid, name);

    p->pid   = pid;
    p->state = STATE_RUNNING;
    p->restart_count = 0;

    log_write(LOG_INFO, "Started '%s' (pid %d)", name, pid);
    return 0;
}

static int pm_start_all(ProcessManager *pm)
{
    int started = 0;
    int errors  = 0;

    log_write(LOG_INFO, "pm_start_all: attempting to start %d processes", pm->count);

    for (int i = 0; i < pm->count; i++) {
        log_write(LOG_DEBUG, "pm_start_all: starting '%s'", pm->procs[i].name);
        if (pm_start(pm, pm->procs[i].name) == 0)
            started++;
        else
            errors++;
    }

    log_write(LOG_INFO, "pm_start_all: %d started, %d errors", started, errors);
    return started;
}

static int pm_stop(ProcessManager *pm, const char *name)
{
    int idx = pm_get_index(pm, name);
    if (idx < 0) {
        log_write(LOG_ERROR, "pm_stop: unknown process '%s'", name);
        return -1;
    }

    ManagedProcess *p = &pm->procs[idx];

    if (p->state != STATE_RUNNING) {
        log_write(LOG_WARN, "pm_stop: '%s' not running (state=%s)", name, state_str[p->state]);
        return 0;
    }

    pid_t pid = p->pid;
    log_write(LOG_DEBUG, "pm_stop: sending SIGTERM to '%s' (pid %d)", name, pid);

    if (kill(pid, SIGTERM) < 0) {
        log_write(LOG_ERROR, "pm_stop: kill SIGTERM '%s' (pid %d): %s", name, pid, strerror(errno));
    }

    int status;
    for (int waited = 0; waited < 50; waited++) {
        pid_t ret = waitpid(pid, &status, WNOHANG);
        if (ret == pid) {
            log_write(LOG_DEBUG, "pm_stop: waitpid caught '%s' (pid %d)", name, pid);
            if (WIFEXITED(status))
                log_write(LOG_INFO, "'%s' (pid %d) exited with code %d",
                          name, pid, WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                log_write(LOG_INFO, "'%s' (pid %d) terminated by signal %d",
                          name, pid, WTERMSIG(status));
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

static int pm_stop_all(ProcessManager *pm)
{
    int stopped = 0;
    int errors  = 0;

    log_write(LOG_INFO, "pm_stop_all: stopping %d processes", pm->count);

    for (int i = 0; i < pm->count; i++) {
        if (pm->procs[i].state == STATE_RUNNING) {
            log_write(LOG_DEBUG, "pm_stop_all: stopping '%s'", pm->procs[i].name);
            if (pm_stop(pm, pm->procs[i].name) == 0)
                stopped++;
            else
                errors++;
        }
    }

    log_write(LOG_INFO, "pm_stop_all: %d stopped, %d errors", stopped, errors);
    return stopped;
}

static int pm_kill(ProcessManager *pm, const char *name)
{
    int idx = pm_get_index(pm, name);
    if (idx < 0) {
        log_write(LOG_ERROR, "pm_kill: unknown process '%s'", name);
        return -1;
    }

    ManagedProcess *p = &pm->procs[idx];

    if (p->state != STATE_RUNNING)
        return 0;

    pid_t pid = p->pid;
    log_write(LOG_WARN, "pm_kill: force-killing '%s' (pid %d) with SIGKILL", name, pid);

    if (kill(pid, SIGKILL) < 0) {
        log_write(LOG_ERROR, "kill SIGKILL '%s': %s", name, strerror(errno));
        return -1;
    }

    waitpid(pid, NULL, 0);
    p->pid   = -1;
    p->state = STATE_STOPPED;
    log_write(LOG_INFO, "'%s' force-killed", name);
    return 0;
}

static int pm_restart(ProcessManager *pm, const char *name)
{
    log_write(LOG_DEBUG, "pm_restart: restarting '%s'", name);

    int idx = pm_get_index(pm, name);
    if (idx < 0) {
        log_write(LOG_ERROR, "pm_restart: unknown process '%s'", name);
        return -1;
    }

    ManagedProcess *p = &pm->procs[idx];
    int delay = p->restart_delay_ms > 0 ? p->restart_delay_ms : 500;

    log_write(LOG_INFO, "Restarting '%s' (delay=%dms, restarts=%d/%d)",
              name, delay, p->restart_count, p->max_restarts);
    pm_stop(pm, name);
    usleep(delay * 1000);
    return pm_start(pm, name);
}

static int pm_restart_all(ProcessManager *pm)
{
    log_write(LOG_INFO, "pm_restart_all: restarting all processes");
    pm_stop_all(pm);
    usleep(RESTART_DELAY_US);
    return pm_start_all(pm);
}

static void pm_check_children(ProcessManager *pm)
{
    int status;
    pid_t pid;

    log_write(LOG_DEBUG, "pm_check_children: checking for terminated children");

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        log_write(LOG_DEBUG, "pm_check_children: reaped pid %d", pid);

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
                log_write(LOG_DEBUG, "'%s' state -> %s (exit code %d)",
                          p->name, state_str[p->state], code);
            } else if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                log_write(LOG_WARN, "'%s' (pid %d) killed by signal %d (%s)",
                          p->name, pid, sig, strsignal(sig));
                p->state = STATE_CRASHED;
                log_write(LOG_DEBUG, "'%s' state -> CRASHED (signal %d)",
                          p->name, sig);
            } else {
                log_write(LOG_WARN, "'%s' (pid %d) died from unknown cause (status=%d)",
                          p->name, pid, status);
                p->state = STATE_CRASHED;
            }

            p->pid = -1;

            if (p->state == STATE_CRASHED && pm->running) {
                if (p->restart_count < p->max_restarts) {
                    p->restart_count++;
                    p->state = STATE_RESTARTING;
                    int delay_us = p->restart_delay_ms * 1000;
                    log_write(LOG_WARN, "Auto-restarting '%s' (%d/%d, delay=%dms)",
                              p->name, p->restart_count, p->max_restarts,
                              p->restart_delay_ms);
                    log_write(LOG_DEBUG, "Auto-restart: sleeping %d us before fork", delay_us);
                    usleep(delay_us);

                    log_write(LOG_DEBUG, "Auto-restart: forking '%s'", p->name);
                    pid_t new_pid = fork();
                    if (new_pid < 0) {
                        log_write(LOG_ERROR, "auto-restart fork '%s': %s",
                                  p->name, strerror(errno));
                        p->state = STATE_CRASHED;
                    } else if (new_pid == 0) {
                        log_write(LOG_DEBUG, "Auto-restart CHILD: '%s' (pid %d)",
                                  p->name, getpid());
                        launch_process(p);
                    } else {
                        p->pid   = new_pid;
                        p->state = STATE_RUNNING;
                        log_write(LOG_INFO, "Auto-restarted '%s' (new pid %d, attempt %d)",
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
            log_write(LOG_WARN, "pm_check_children: reaped unknown child pid %d", pid);
    }

    if (pid < 0 && errno != ECHILD) {
        log_write(LOG_ERROR, "pm_check_children: waitpid error: %s", strerror(errno));
    }
}

static void pm_shutdown(ProcessManager *pm)
{
    log_write(LOG_INFO, "pm_shutdown: shutting down process manager");
    pm->running = 0;
    pm_stop_all(pm);
    log_write(LOG_INFO, "pm_shutdown: complete");
}

static int pm_get_index(ProcessManager *pm, const char *name)
{
    if (!name)
        return -1;

    for (int i = 0; i < pm->count; i++) {
        if (strcmp(pm->procs[i].name, name) == 0)
            return i;
    }
    return -1;
}

static void pm_print_status(ProcessManager *pm)
{
    char buf[4096];
    int  off = 0;

    off += snprintf(buf + off, sizeof(buf) - off,
                    "===== Process Manager Status (%d registered) =====\n", pm->count);
    off += snprintf(buf + off, sizeof(buf) - off,
                    "%-20s %-10s %-8s %-6s %s\n",
                    "NAME", "STATE", "PID", "RST", "AUTO-BUILT");
    off += snprintf(buf + off, sizeof(buf) - off,
                    "%-20s %-10s %-8s %-6s %s\n",
                    "----", "-----", "---", "---", "----------");

    for (int i = 0; i < pm->count; i++) {
        ManagedProcess *p = &pm->procs[i];

        off += snprintf(buf + off, sizeof(buf) - off,
                        "%-20s %-10s %-8d %d/%-2d %s\n",
                        p->name, state_str[p->state], (int)p->pid,
                        p->restart_count, p->max_restarts,
                        p->auto_built ? "YES" : "no");
    }

    off += snprintf(buf + off, sizeof(buf) - off,
                    "=================================================\n");

    log_write(LOG_INFO, "Status report:\n%s", buf);
    printf("%s", buf);
}

static int config_load(ProcessManager *pm, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        log_write(LOG_ERROR, "config_load: cannot open '%s': %s",
                  path, strerror(errno));
        return -1;
    }

    char line[512];
    int  line_no = 0;
    int  loaded = 0;

    log_write(LOG_INFO, "config_load: reading configuration from '%s'", path);

    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;

        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';

        char name[64], prog_path[256];
        int  max_restarts, delay_ms;
        char extra[256];

        int n = sscanf(p, "%63[^:]:%255[^:]:%d:%d:%255s",
                       name, prog_path, &max_restarts, &delay_ms, extra);

        if (n < 4) {
            log_write(LOG_WARN, "config_load: line %d malformed, skipping: %s", line_no, p);
            continue;
        }

        log_write(LOG_DEBUG, "config_load: line %d: name='%s' path='%s' restarts=%d delay=%d",
                  line_no, name, prog_path, max_restarts, delay_ms);

        char *argv[] = { prog_path, NULL };
        if (pm_add(pm, name, prog_path, argv, 1) == 0) {
            int idx = pm->count - 1;
            pm->procs[idx].max_restarts     = max_restarts;
            pm->procs[idx].restart_delay_ms = delay_ms;
            loaded++;

            log_write(LOG_DEBUG, "config_load: validating executable for '%s'", name);
            ensure_executable(&pm->procs[idx]);
        }
    }

    fclose(fp);
    log_write(LOG_INFO, "config_load: loaded %d/%d lines from '%s'",
              loaded, line_no, path);
    return loaded;
}

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

    const char *sig_names[] = {
        [SIGCHLD] = "SIGCHLD", [SIGINT] = "SIGINT",
        [SIGTERM] = "SIGTERM", [SIGHUP] = "SIGHUP"
    };
    const char *sn = (signum >= 0 && (size_t)signum < sizeof(sig_names)/sizeof(sig_names[0]))
                     ? sig_names[signum] : NULL;
    log_write(LOG_DEBUG, "Installed handler for %s (flags=0x%x)",
              sn ? sn : "SIGNAL", flags);
    return 0;
}

static int signal_setup(ProcessManager *pm)
{
    g_pm = pm;
    g_sigchld_flag = 0;

    log_write(LOG_INFO, "signal_setup: installing signal handlers");

    if (setup_one(SIGCHLD, handle_sigchld, SA_NOCLDSTOP | SA_RESTART) < 0)
        return -1;

    if (setup_one(SIGINT,  handle_sigint,  0) < 0)
        return -1;

    if (setup_one(SIGTERM, handle_sigterm, 0) < 0)
        return -1;

    if (setup_one(SIGHUP,  handle_sighup,  0) < 0)
        return -1;

    signal(SIGPIPE, SIG_IGN);

    log_write(LOG_INFO, "Signal handlers installed (SIGCHLD, SIGINT, SIGTERM, SIGHUP, SIGPIPE)");
    return 0;
}

static void signal_teardown(void)
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
    log_write(LOG_DEBUG, "Signal handlers restored to defaults");
}

static void print_startup_banner(void)
{
    printf("\n");
    printf("============================================\n");
    printf("  Process Manager with Signal Control\n");
    printf("============================================\n");
    printf("  PID: %d\n", getpid());
    printf("  CWD: ");
    fflush(stdout);
    system("pwd");
    printf("--------------------------------------------\n");
}

int main(int argc, char **argv)
{
    int run_as_daemon = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0)
            run_as_daemon = 1;
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
            g_verbose = 1;
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  -d, --daemon    Run as daemon\n");
            printf("  -v, --verbose   Enable DEBUG-level logging\n");
            printf("  -h, --help      Show this help\n");
            return 0;
        }
    }

    if (log_init() != 0) {
        fprintf(stderr, "FATAL: Failed to initialise logger\n");
        return 1;
    }

    if (g_verbose)
        log_write(LOG_INFO, "Verbose mode enabled");

    print_startup_banner();

    log_write(LOG_INFO, "Starting Process Manager (pid %d)", getpid());

    if (run_as_daemon) {
        log_write(LOG_INFO, "Daemonising ...");
        if (daemonize() < 0) {
            log_write(LOG_FATAL, "Daemonisation failed");
            log_close();
            return 1;
        }
    }

    log_write(LOG_INFO, "Initialising process manager");
    ProcessManager pm;
    pm_init(&pm);

    log_write(LOG_INFO, "Loading configuration from config/processes.conf");
    int cfg_count = config_load(&pm, "config/processes.conf");

    if (cfg_count <= 0) {
        log_write(LOG_WARN, "No processes configured, nothing to supervise");
        log_write(LOG_INFO, "Running in idle mode (send SIGHUP to reload)");
    } else {
        log_write(LOG_INFO, "Configured %d process(es) for supervision", cfg_count);

        printf("\n--- Configured Processes ---\n");
        for (int i = 0; i < pm.count; i++) {
            ManagedProcess *p = &pm.procs[i];
            printf("  [%d] %-15s -> %-35s [restarts=%d, delay=%dms] %s\n",
                   i, p->name, p->path, p->max_restarts, p->restart_delay_ms,
                   p->auto_built ? "[AUTO-BUILT]" : "");
        }
        printf("---------------------------\n\n");
    }

    log_write(LOG_INFO, "Setting up signal handlers");
    if (signal_setup(&pm) < 0) {
        log_write(LOG_FATAL, "Signal setup failed");
        pm_shutdown(&pm);
        log_close();
        return 1;
    }

    log_write(LOG_INFO, "Starting all configured processes");
    pm_start_all(&pm);
    pm_print_status(&pm);

    log_write(LOG_INFO, "Entering main supervision loop");
    printf("\n[supervisor] Entering main loop. Try: pgrep -a sleeper\n");
    printf("[supervisor] Send SIGTERM/SIGINT to exit, SIGHUP to restart all\n\n");

    while (pm.running) {
        if (g_sigchld_flag) {
            g_sigchld_flag = 0;
            pm_check_children(&pm);
        }
        if (pm.restart_all) {
            pm.restart_all = 0;
            log_write(LOG_INFO, "SIGHUP: restarting all processes");
            pm_restart_all(&pm);
            pm_print_status(&pm);
        }
        usleep(50000);
    }

    log_write(LOG_INFO, "Main loop exited, shutting down");
    pm_shutdown(&pm);
    signal_teardown();
    log_close();
    printf("\n[supervisor] Process Manager stopped.\n");
    return 0;
}
