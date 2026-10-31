#ifndef TAB_RUNTIME_H
#define TAB_RUNTIME_H

#include "gui.h"
#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int fd;
    int mode;
    int active;
    pid_t pid;
    void *thread_storage;
    unsigned char rx[20000];
    size_t rx_used;
    size_t rx_needed;
} TabRuntime;

int tab_runtime_start(TabRuntime *runtime, TerminatorMode mode);

int tab_runtime_submit(
    TabRuntime *runtime,
    const char *command
);

int tab_runtime_poll(
    TabRuntime *runtime,
    char *output,
    size_t output_size,
    int *exit_status
);

void tab_runtime_signal(
    TabRuntime *runtime,
    int signal_number
);

void tab_runtime_stop(TabRuntime *runtime);

#endif