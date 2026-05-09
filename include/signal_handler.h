#ifndef SIGNAL_HANDLER_H
#define SIGNAL_HANDLER_H

#include "process_manager.h"

int  signal_setup(ProcessManager *pm);
void signal_teardown(void);

extern volatile sig_atomic_t g_sigchld_flag;

#endif
