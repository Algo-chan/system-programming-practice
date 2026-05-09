#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include <sys/types.h>
#include <unistd.h>

#define MAX_PROCESSES    32
#define NAME_MAX         64
#define PATH_MAX         256
#define MAX_ARGS         8
#define RESTART_DELAY_US 500000

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
} ManagedProcess;

typedef struct {
    ManagedProcess procs[MAX_PROCESSES];
    int            count;
    int            running;
    int            restart_all;
    int            sigchld_pending;
} ProcessManager;

int  pm_init(ProcessManager *pm);
int  pm_add(ProcessManager *pm, const char *name, const char *path,
            char *const argv[], int argc);
int  pm_start(ProcessManager *pm, const char *name);
int  pm_start_all(ProcessManager *pm);
int  pm_stop(ProcessManager *pm, const char *name);
int  pm_stop_all(ProcessManager *pm);
int  pm_kill(ProcessManager *pm, const char *name);
int  pm_restart(ProcessManager *pm, const char *name);
int  pm_restart_all(ProcessManager *pm);
void pm_check_children(ProcessManager *pm);
void pm_shutdown(ProcessManager *pm);
int  pm_get_index(ProcessManager *pm, const char *name);
void pm_print_status(ProcessManager *pm);

#endif
