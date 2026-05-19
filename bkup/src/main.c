#include "logger.h"
#include "process_manager.h"
#include "signal_handler.h"
#include "config.h"
#include "daemon.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

static ProcessManager pm;

int main(int argc, char **argv)
{
    int run_as_daemon = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0)
            run_as_daemon = 1;
    }

    if (log_init() != 0) {
        fprintf(stderr, "Failed to initialise logger\n");
        return 1;
    }

    if (run_as_daemon) {
        log_write(LOG_INFO, "Daemonising ...");
        if (daemonize() < 0) {
            log_write(LOG_FATAL, "Daemonisation failed");
            log_close();
            return 1;
        }
    }

    pm_init(&pm);

    config_load(&pm, "config/processes.conf");

    if (signal_setup(&pm) < 0) {
        log_write(LOG_FATAL, "Signal setup failed");
        pm_shutdown(&pm);
        log_close();
        return 1;
    }

    log_write(LOG_INFO, "Starting all processes");
    pm_start_all(&pm);
    pm_print_status(&pm);

    while (pm.running) {
        if (g_sigchld_flag) {
            g_sigchld_flag = 0;
            pm_check_children(&pm);
        }
        if (pm.restart_all) {
            pm.restart_all = 0;
            pm_restart_all(&pm);
        }
        usleep(50000);
    }

    pm_shutdown(&pm);
    signal_teardown();
    log_close();
    return 0;
}
