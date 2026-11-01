#ifndef TAB_RUNTIME_H
#define TAB_RUNTIME_H

#include "gui.h"

#include <stddef.h>
#include <sys/types.h>

#define TAB_RUNTIME_BUFFER_SIZE 20000

typedef struct
{
    int fd;
    int mode;
    int active;

    pid_t pid;

    /* Used only in thread mode. */
    void *thread_storage;

    /* Receive buffer for framed worker responses. */
    unsigned char rx[TAB_RUNTIME_BUFFER_SIZE];
    size_t rx_used;
    size_t rx_needed;

} TabRuntime;

/* Start a worker for one tab. */
int tab_runtime_start(
    TabRuntime *runtime,
    TerminatorMode mode
);

/* Submit a command to the worker. */
int tab_runtime_submit(
    TabRuntime *runtime,
    const char *command
);

/*
 * Poll for worker output.
 *
 * Return:
 *   1 = complete response received
 *   0 = no complete response available yet
 *  -1 = worker disconnected or communication error
 */
int tab_runtime_poll(
    TabRuntime *runtime,
    char *output,
    size_t output_size,
    int *exit_status
);

/* Send Ctrl+C or Ctrl+Z notification to this tab's worker. */
void tab_runtime_signal(
    TabRuntime *runtime,
    int signal_number
);

/* Stop and clean up the worker. */
void tab_runtime_stop(
    TabRuntime *runtime
);

#endif