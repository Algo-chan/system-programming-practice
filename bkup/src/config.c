#include "config.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int config_load(ProcessManager *pm, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        log_write(LOG_ERROR, "config_load: cannot open '%s': %s",
                  path, strerror(errno));
        return -1;
    }

    char line[512];
    int  loaded = 0;

    while (fgets(line, sizeof(line), fp)) {
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
            log_write(LOG_WARN, "config: malformed line, skipping: %s", p);
            continue;
        }

        char *argv[] = { prog_path, NULL };
        if (pm_add(pm, name, prog_path, argv, 1) == 0) {
            int idx = pm->count - 1;
            pm->procs[idx].max_restarts     = max_restarts;
            pm->procs[idx].restart_delay_ms = delay_ms;
            loaded++;
        }
    }

    fclose(fp);
    log_write(LOG_INFO, "config_load: loaded %d processes from '%s'",
              loaded, path);
    return loaded;
}
