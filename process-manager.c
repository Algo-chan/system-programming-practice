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
#include <sys/prctl.h>

#define MAX_PROCESSES     32
#define NAME_MAX          64
#define PATH_MAX          256
#define MAX_ARGS          8
#define LOG_PATH          "logs/manager.log"
#define MAX_LOG_LINE      4096
#define MAX_BACKOFF_MS    30000
#define MAX_RESTART_ATEMP 5

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} LogLevel;

typedef enum {
    STATE_STOPPED   = 0,
    STATE_RUNNING   = 1,
    STATE_CRASHED   = 2
} WorkerState;

typedef enum {
    POLICY_ALWAYS     = 0,
    POLICY_ON_FAILURE = 1,
    POLICY_NEVER      = 2
} RestartPolicy;

static const char *policy_str[] = {
    "always", "on-failure", "never"
};

static const char *state_str[] = {
    "STOPPED", "RUNNING", "CRASHED"
};

typedef struct {
    char         name[NAME_MAX];
    char         path[PATH_MAX];
    char         argv[MAX_ARGS][NAME_MAX];
    int          argc;
    pid_t        pid;
    WorkerState  state;
    int          restart_count;
    int          max_restarts;
    int          base_delay_ms;
    int          alive;
    time_t       last_restart_time;
    RestartPolicy policy;
} Worker;

typedef struct {
    Worker       workers[MAX_PROCESSES];
    int          count;
    volatile int running;
    volatile int restart_all;
    volatile int shutting_down;
    int          verbose;
    int          daemon_mode;
} SupervisorState;

static FILE *log_file = NULL;
static int  g_verbose;
static const char *level_str[] = {
    "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

static int  log_init(void);
static void log_write(LogLevel level, const char *fmt, ...);
static void log_close(void);
static int  daemonize(void);
static int  config_load(Worker *workers, int *count, const char *path);
static int  ensure_executable(const char *path);
static int  auto_build(const char *src, const char *out);
static void write_all(int fd, const char *msg);
static void log_exit_reason(const char *name, pid_t pid, int status, int level);

static int log_init(void)
{
    log_file = fopen(LOG_PATH, "a");
    if (!log_file) {
        fprintf(stderr, "[FATAL] log_init: %s: %s\n", LOG_PATH, strerror(errno));
        return -1;
    }
    setbuf(log_file, NULL);
    log_write(LOG_INFO, "=== process-manager started ===");
    return 0;
}

static void log_write(LogLevel level, const char *fmt, ...)
{
    if (!log_file && level > LOG_DEBUG) {
        fprintf(stderr, "[NOLOG] ");
        va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
        fprintf(stderr, "\n");
        return;
    }
    if (level == LOG_DEBUG && !g_verbose)
        return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[64]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", tm);

    char buf[MAX_LOG_LINE];
    int off = snprintf(buf, sizeof buf, "[%s] [%s] ", ts, level_str[level]);

    va_list ap; va_start(ap, fmt);
    off += vsnprintf(buf + off, sizeof buf - off, fmt, ap);
    va_end(ap);

    if (log_file) fprintf(log_file, "%s\n", buf);
    if (level <= LOG_WARN) fprintf(stderr, "%s\n", buf);
}

static void log_close(void)
{
    if (log_file) {
        log_write(LOG_INFO, "=== process-manager stopped ===");
        fclose(log_file);
        log_file = NULL;
    }
}

static void write_all(int fd, const char *msg)
{
    size_t len = strlen(msg), pos = 0;
    while (pos < len) {
        ssize_t r = write(fd, msg + pos, len - pos);
        if (r <= 0) break;
        pos += (size_t)r;
    }
}

static void log_exit_reason(const char *name, pid_t pid, int status, int level)
{
    if (WIFEXITED(status)) {
        int c = WEXITSTATUS(status);
        log_write(level, "EXIT: '%s' pid=%d WIFEXITED code=%d", name, pid, c);
    } else if (WIFSIGNALED(status)) {
        int s = WTERMSIG(status);
        log_write(level, "EXIT: '%s' pid=%d WIFSIGNALED sig=%d(%s)%s",
                  name, pid, s, strsignal(s), WCOREDUMP(status) ? " COREDUMP" : "");
    } else {
        log_write(level, "EXIT: '%s' pid=%d status=0x%x", name, pid, status);
    }
}

static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) { log_write(LOG_ERROR, "daemonize: fork: %s", strerror(errno)); return -1; }
    if (pid > 0) _exit(0);

    if (setsid() < 0) { log_write(LOG_ERROR, "daemonize: setsid: %s", strerror(errno)); return -1; }

    pid = fork();
    if (pid < 0) { log_write(LOG_ERROR, "daemonize: fork2: %s", strerror(errno)); return -1; }
    if (pid > 0) _exit(0);

    umask(0);
    if (chdir("/") < 0) { log_write(LOG_ERROR, "daemonize: chdir: %s", strerror(errno)); return -1; }

    close(STDIN_FILENO); close(STDOUT_FILENO); close(STDERR_FILENO);
    int fd = open("/dev/null", O_RDWR);
    if (fd < 0) return -1;
    dup2(fd, STDIN_FILENO); dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) close(fd);

    log_write(LOG_INFO, "daemonized pid=%d", getpid());
    return 0;
}

static int ensure_executable(const char *path)
{
    if (access(path, F_OK) == 0) {
        if (access(path, X_OK) == 0) return 0;
        log_write(LOG_ERROR, "not executable: %s", path);
        return -1;
    }
    size_t plen = strlen(path);
    if (plen < 3) return -1;

    char src[PATH_MAX];
    strncpy(src, path, sizeof src - 1); src[sizeof src - 1] = 0;
    char *dot = strrchr(src, '.');
    if (dot && strlen(dot) >= 2 && strlen(dot) <= 4)
        strcpy(dot, ".c");
    else
        strcat(src, ".c");

    if (access(src, F_OK) != 0) {
        log_write(LOG_ERROR, "no binary(%s) nor source(%s)", path, src);
        return -1;
    }
    log_write(LOG_INFO, "auto-building %s from %s", path, src);
    return auto_build(src, path);
}

static int auto_build(const char *src, const char *out)
{
    char cmd[PATH_MAX + 80];
    snprintf(cmd, sizeof cmd, "gcc -Wall -O2 -std=c11 -o '%s' '%s' 2>&1", out, src);
    int st = system(cmd);
    if (st == -1) return -1;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && access(out, X_OK) == 0) {
        log_write(LOG_INFO, "auto-build ok: %s", out);
        return 0;
    }
    log_write(LOG_ERROR, "auto-build failed for %s", out);
    return -1;
}

static int config_load(Worker *workers, int *count, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) { log_write(LOG_ERROR, "config: cannot open %s", path); return -1; }

    char line[512]; int lineno = 0, loaded = 0;
    while (fgets(line, sizeof line, fp)) {
        lineno++;
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == 0) continue;
        char *nl = strchr(p, '\n'); if (nl) *nl = 0;

        char name[64]={0}, prog[256]={0}, pol[32]={0};
        int mr = 3, dm = 2000;
        int n = sscanf(p, "%63[^:]:%255[^:]:%d:%d:%31s", name, prog, &mr, &dm, pol);
        if (n < 2) { log_write(LOG_WARN, "config:%d malformed: %s", lineno, p); continue; }
        if (n < 3) mr = 3;
        if (n < 4) dm = 2000;

        RestartPolicy policy = POLICY_ALWAYS;
        if (strcmp(pol, "on-failure") == 0) policy = POLICY_ON_FAILURE;
        else if (strcmp(pol, "never") == 0) policy = POLICY_NEVER;

        if (*count >= MAX_PROCESSES) {
            log_write(LOG_ERROR, "config:%d max workers (%d) reached", lineno, MAX_PROCESSES);
            continue;
        }
        Worker *w = &workers[*count];
        strncpy(w->name, name, sizeof w->name - 1);
        strncpy(w->path, prog, sizeof w->path - 1);
        w->argv[0][0] = 0; w->argc = 0;
        strncpy(w->argv[0], prog, sizeof w->argv[0] - 1);
        w->argc = 1;
        w->pid = -1; w->state = STATE_STOPPED;
        w->restart_count = 0; w->max_restarts = mr;
        w->base_delay_ms = dm; w->alive = 0;
        w->last_restart_time = 0; w->policy = policy;

        (*count)++; loaded++;
        log_write(LOG_INFO, "config: '%s' %s restarts=%d delay=%d policy=%s",
                  name, prog, mr, dm, policy_str[policy]);
        ensure_executable(prog);
    }
    fclose(fp);
    log_write(LOG_INFO, "config: loaded %d workers from %s", loaded, path);
    return loaded;
}

/* ==================================================================
 *  CLEAN MODE
 *
 *  Compile with -DCLEAN_MODE
 *
 *  prctl(PR_SET_PDEATHSIG, SIGTERM) in each child guarantees that if
 *  the parent dies (even via SIGKILL), every child immediately receives
 *  SIGTERM from the kernel.  No waitpid loop, no restart logic, no
 *  SIGCHLD handler.  Graceful shutdown uses kill(0, SIGTERM) to signal
 *  the entire process group.  Signal handlers are minimal — atomic
 *  flags only.
 * ================================================================== */
#ifdef CLEAN_MODE

static volatile sig_atomic_t clean_should_stop = 0;

static void clean_sig_handle(int sig)
{
    (void)sig;
    clean_should_stop = 1;
}

static int run_clean_mode(void)
{
    log_write(LOG_INFO, "CLEAN MODE: prctl(PR_SET_PDEATHSIG) enforced");

    Worker workers[MAX_PROCESSES];
    int    nworkers = 0;

    config_load(workers, &nworkers, "config/processes.conf");

    if (nworkers == 0) {
        log_write(LOG_WARN, "no workers configured");
        return 0;
    }

    printf("\n[CLEAN] Starting %d workers (PDEATHSIG active)\n", nworkers);
    printf("[CLEAN] Parent pid=%d\n", getpid());
    printf("[CLEAN] If parent is killed, children die automatically\n\n");

    for (int i = 0; i < nworkers; i++) {
        Worker *w = &workers[i];
        ensure_executable(w->path);

        pid_t pid = fork();
        if (pid < 0) {
            log_write(LOG_ERROR, "fork failed for %s: %s", w->name, strerror(errno));
            continue;
        }
        if (pid == 0) {
            if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0)
                write_all(STDERR_FILENO, "[CHILD] prctl PR_SET_PDEATHSIG failed\n");

            char buf[256];
            int len = snprintf(buf, sizeof buf,
                               "[CHILD %s] pid=%d ppid=%d PDEATHSIG=SIGTERM\n",
                               w->name, getpid(), getppid());
            write_all(STDERR_FILENO, buf);

            char *args[MAX_ARGS + 1];
            for (int j = 0; j < w->argc; j++) args[j] = w->argv[j];
            args[w->argc] = NULL;

            execvp(w->path, args);
            len = snprintf(buf, sizeof buf,
                           "[CHILD %s] execvp FAILED: %s\n", w->name, strerror(errno));
            write_all(STDERR_FILENO, buf);
            _exit(127);
        }

        w->pid = pid;
        w->state = STATE_RUNNING;
        w->alive = 1;
        log_write(LOG_INFO, "forked %s pid=%d", w->name, pid);
        printf("[CLEAN] %-15s pid=%d\n", w->name, pid);
    }

    printf("\n[CLEAN] All workers running.  Press Ctrl+C or kill parent to stop.\n");
    printf("[CLEAN] Try: kill -9 %d  (children die immediately)\n\n", getpid());

    struct sigaction sa;
    memset(&sa, 0, sizeof sa); sigemptyset(&sa.sa_mask);
    sa.sa_handler = clean_sig_handle; sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    while (!clean_should_stop)
        usleep(100000);

    log_write(LOG_INFO, "CLEAN: shutting down process group");
    printf("\n[CLEAN] Shutting down...\n");

    kill(0, SIGTERM);

    for (int i = 0; i < nworkers; i++) {
        if (workers[i].pid > 0)
            waitpid(workers[i].pid, NULL, 0);
    }

    log_write(LOG_INFO, "CLEAN: all workers terminated");
    printf("[CLEAN] Done.\n");
    return 0;
}

/* ==================================================================
 *  SUPERVISOR MODE  (default)
 *
 *  Lightweight systemd-like controller.  SIGCHLD sets a flag; the
 *  main loop calls waitpid(-1, WNOHANG) to reap and then decides
 *  whether to restart based on policy, restart count, and exponential
 *  backoff.  No work is done inside signal handlers.  Zombie-free.
 * ================================================================== */
#else

static volatile sig_atomic_t g_sigchld_flag = 0;
static SupervisorState *g_state = NULL;

static void sup_sigchld(int sig)
{
    (void)sig;
    g_sigchld_flag = 1;
}

static void sup_sigint(int sig)
{
    (void)sig;
    if (g_state && !g_state->shutting_down)
        g_state->running = 0;
}

static void sup_sigterm(int sig)
{
    (void)sig;
    if (g_state && !g_state->shutting_down)
        g_state->running = 0;
}

static void sup_sighup(int sig)
{
    (void)sig;
    if (g_state)
        g_state->restart_all = 1;
}

static int supervisor_setup_signals(SupervisorState *s)
{
    g_state = s;
    g_sigchld_flag = 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa); sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;

    sa.sa_handler = sup_sigchld;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) return -1;

    sa.sa_flags = 0;
    sa.sa_handler = sup_sigint;
    if (sigaction(SIGINT, &sa, NULL) < 0) return -1;

    sa.sa_handler = sup_sigterm;
    if (sigaction(SIGTERM, &sa, NULL) < 0) return -1;

    sa.sa_handler = sup_sighup;
    if (sigaction(SIGHUP, &sa, NULL) < 0) return -1;

    signal(SIGPIPE, SIG_IGN);
    log_write(LOG_INFO, "SUPERVISOR: signal handlers installed (SIGCHLD/SIGINT/SIGTERM/SIGHUP)");
    return 0;
}

static void supervisor_teardown_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa); sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &sa, NULL); sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_DFL);
    g_state = NULL;
}

static int supervisor_start(SupervisorState *s, Worker *w)
{
    if (w->state == STATE_RUNNING) {
        log_write(LOG_WARN, "already running: %s pid=%d", w->name, w->pid);
        return 0;
    }
    if (ensure_executable(w->path) != 0) {
        log_write(LOG_ERROR, "cannot launch %s: executable unavailable", w->name);
        w->state = STATE_STOPPED; return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        log_write(LOG_ERROR, "fork %s: %s", w->name, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        char *args[MAX_ARGS + 1];
        for (int j = 0; j < w->argc; j++) args[j] = w->argv[j];
        args[w->argc] = NULL;
        execvp(w->path, args);
        fprintf(stderr, "[CHILD %s] execvp: %s\n", w->name, strerror(errno));
        _exit(127);
    }

    w->pid = pid; w->state = STATE_RUNNING; w->alive = 1;
    w->restart_count = 0; w->last_restart_time = time(NULL);
    log_write(LOG_INFO, "START: '%s' pid=%d policy=%s", w->name, pid, policy_str[w->policy]);
    return 0;
}

static int supervisor_start_all(SupervisorState *s)
{
    int ok = 0, err = 0;
    for (int i = 0; i < s->count; i++) {
        if (supervisor_start(s, &s->workers[i]) == 0) ok++; else err++;
    }
    log_write(LOG_INFO, "START_ALL: %d ok %d err", ok, err);
    return ok;
}

static int supervisor_stop(SupervisorState *s, Worker *w)
{
    if (w->state != STATE_RUNNING || w->pid <= 0) {
        w->pid = -1; w->state = STATE_STOPPED; return 0;
    }

    pid_t pid = w->pid;
    log_write(LOG_DEBUG, "STOP: SIGTERM %s pid=%d", w->name, pid);
    kill(pid, SIGTERM);

    int status;
    for (int tries = 0; tries < 50; tries++) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            log_exit_reason(w->name, pid, status, LOG_INFO);
            w->pid = -1; w->state = STATE_STOPPED; w->alive = 0;
            return 0;
        }
        if (r < 0 && errno != EINTR) break;
        usleep(10000);
    }

    log_write(LOG_WARN, "STOP: SIGKILL %s pid=%d (no response to SIGTERM)", w->name, pid);
    kill(pid, SIGKILL); waitpid(pid, NULL, 0);
    w->pid = -1; w->state = STATE_STOPPED; w->alive = 0;
    return 0;
}

static int supervisor_stop_all(SupervisorState *s)
{
    int ok = 0, err = 0;
    for (int i = s->count - 1; i >= 0; i--) {
        if (s->workers[i].state == STATE_RUNNING) {
            if (supervisor_stop(s, &s->workers[i]) == 0) ok++; else err++;
        }
    }
    log_write(LOG_INFO, "STOP_ALL: %d ok %d err", ok, err);
    return ok;
}

static int compute_backoff_ms(Worker *w)
{
    unsigned long long d = (w->base_delay_ms > 0 ? (unsigned long long)w->base_delay_ms : 500ULL)
                           * (1ULL << (w->restart_count > 10 ? 10 : w->restart_count));
    return (int)(d > MAX_BACKOFF_MS ? MAX_BACKOFF_MS : d);
}

static int should_restart(Worker *w, int status)
{
    if (w->policy == POLICY_NEVER) return 0;
    if (w->policy == POLICY_ON_FAILURE) {
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
        return 1;
    }
    return 1;
}

static void supervisor_check_children(SupervisorState *s)
{
    int status; pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int found = 0;
        for (int i = 0; i < s->count; i++) {
            Worker *w = &s->workers[i];
            if (w->pid != pid) continue;
            found = 1;

            log_exit_reason(w->name, pid, status, LOG_INFO);
            int was_running = (w->state == STATE_RUNNING);

            w->pid = -1; w->state = STATE_CRASHED; w->alive = 0;

            if (!was_running || s->shutting_down) {
                log_write(LOG_DEBUG, "CHK: '%s' not restarted (was_running=%d shutting_down=%d)",
                          w->name, was_running, s->shutting_down);
                break;
            }

            if (!should_restart(w, status)) {
                log_write(LOG_INFO, "CHK: '%s' policy=%s prevents restart", w->name, policy_str[w->policy]);
                break;
            }

            if (w->restart_count >= w->max_restarts) {
                log_write(LOG_ERROR, "CHK: '%s' max_restarts=%d exhausted, giving up",
                          w->name, w->max_restarts);
                break;
            }

            w->restart_count++;
            int backoff_ms = compute_backoff_ms(w);
            log_write(LOG_WARN,
                      "RESTART: '%s' old_pid=%d attempt=%d/%d backoff=%dms policy=%s",
                      w->name, pid, w->restart_count, w->max_restarts,
                      backoff_ms, policy_str[w->policy]);

            usleep(backoff_ms * 1000);

            pid_t np = fork();
            if (np < 0) {
                log_write(LOG_ERROR, "RESTART: fork %s: %s", w->name, strerror(errno));
                w->state = STATE_CRASHED;
            } else if (np == 0) {
                char *args[MAX_ARGS + 1];
                for (int j = 0; j < w->argc; j++) args[j] = w->argv[j];
                args[w->argc] = NULL;
                execvp(w->path, args);
                fprintf(stderr, "[CHILD %s] execvp: %s\n", w->name, strerror(errno));
                _exit(127);
            } else {
                w->pid = np; w->state = STATE_RUNNING; w->alive = 1;
                w->last_restart_time = time(NULL);
                log_write(LOG_INFO,
                          "RESTART: '%s' old_pid=%d new_pid=%d attempt=%d/%d",
                          w->name, pid, np, w->restart_count, w->max_restarts);
            }
            break;
        }
        if (!found)
            log_write(LOG_WARN, "CHK: unknown child pid=%d reaped", pid);
    }

    if (pid < 0 && errno != ECHILD)
        log_write(LOG_ERROR, "CHK: waitpid error: %s", strerror(errno));
}

static void supervisor_shutdown(SupervisorState *s)
{
    log_write(LOG_INFO, "SHUTDOWN: stopping supervisor");
    s->shutting_down = 1; s->running = 0;

    kill(0, SIGTERM);
    supervisor_stop_all(s);
    log_write(LOG_INFO, "SHUTDOWN: complete");
}

static void print_workers(const char *label, Worker *workers, int count)
{
    printf("\n%s\n", label);
    printf("%-20s %-10s %-8s %-10s %-6s %s\n",
           "NAME", "STATE", "PID", "POLICY", "RST", "ALIVE");
    printf("%-20s %-10s %-8s %-10s %-6s %s\n",
           "----", "-----", "---", "------", "---", "-----");
    for (int i = 0; i < count; i++) {
        Worker *w = &workers[i];
        printf("%-20s %-10s %-8d %-10s %d/%-2d %s\n",
               w->name, state_str[w->state], (int)w->pid,
               policy_str[w->policy],
               w->restart_count, w->max_restarts,
               w->alive ? "yes" : "no");
    }
    printf("\n");
}

static int run_supervisor_mode(void)
{
    log_write(LOG_INFO, "SUPERVISOR MODE: systemd-like controller");

    SupervisorState s;
    memset(&s, 0, sizeof s);
    s.running = 1; s.verbose = g_verbose;

    config_load(s.workers, &s.count, "config/processes.conf");
    if (s.count == 0) {
        log_write(LOG_WARN, "no workers configured");
        return 0;
    }

    printf("\n[SUPERVISOR] PID=%d  workers=%d\n", getpid(), s.count);
    print_workers("=== Configured Workers ===", s.workers, s.count);

    if (supervisor_setup_signals(&s) < 0) {
        log_write(LOG_FATAL, "signal setup failed");
        return 1;
    }

    supervisor_start_all(&s);
    print_workers("=== After Initial Start ===", s.workers, s.count);

    log_write(LOG_INFO, "SUPERVISOR: entering event loop");
    printf("[SUPERVISOR] Event loop running.  Signals:\n");
    printf("  SIGTERM/SIGINT  = graceful shutdown\n");
    printf("  SIGHUP          = restart all workers\n");
    printf("  SIGCHLD         = auto-reap + restart with backoff\n\n");

    while (s.running) {
        if (g_sigchld_flag) {
            g_sigchld_flag = 0;
            supervisor_check_children(&s);
        }
        if (s.restart_all) {
            s.restart_all = 0;
            log_write(LOG_INFO, "SIGHUP: restart-all triggered");
            kill(0, SIGTERM);
            usleep(200000);
            supervisor_stop_all(&s);
            usleep(500000);
            supervisor_start_all(&s);
            print_workers("=== After SIGHUP Restart ===", s.workers, s.count);
        }
        usleep(50000);
    }

    log_write(LOG_INFO, "SUPERVISOR: event loop exited");
    supervisor_shutdown(&s);
    supervisor_teardown_signals();
    print_workers("=== Final State ===", s.workers, s.count);
    return 0;
}

#endif /* CLEAN_MODE */

int main(int argc, char **argv)
{
    int daemon_flag = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0)
            daemon_flag = 1;
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
            g_verbose = 1;
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("  -d, --daemon     Run as daemon\n");
            printf("  -v, --verbose    Enable DEBUG logging\n");
            printf("  -h, --help       This help\n");
            printf("\nBuild modes:\n");
            printf("  default          Supervisor mode (systemd-like controller)\n");
            printf("  -DCLEAN_MODE     Clean mode (prctl PDEATHSIG, no restart)\n");
            return 0;
        }
    }

    if (log_init() != 0) { fprintf(stderr, "log_init failed\n"); return 1; }

    printf("============================================\n");
    printf("  Process Manager\n");
#ifdef CLEAN_MODE
    printf("  Mode: CLEAN  (prctl PDEATHSIG enforced)\n");
#else
    printf("  Mode: SUPERVISOR  (systemd-like controller)\n");
#endif
    printf("============================================\n");
    printf("  PID: %d\n", getpid());
    printf("--------------------------------------------\n");

    if (daemon_flag && daemonize() < 0) {
        log_write(LOG_FATAL, "daemonization failed");
        log_close(); return 1;
    }

    int rc;

#ifdef CLEAN_MODE
    rc = run_clean_mode();
#else
    rc = run_supervisor_mode();
#endif

    log_close();
    return rc;
}
