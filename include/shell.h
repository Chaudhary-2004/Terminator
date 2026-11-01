#ifndef SHELL_H
#define SHELL_H

#define SHELL_MAX_JOBS 64

/* Execute one command in this worker's shell context. */
void run_command(
    const char *command,
    char *output,
    int output_size,
    int *exit_status
);

/* Foreground process information. */
int shell_foreground_running(void);

/* Background job management. */
int shell_poll_jobs(
    char *output,
    int output_size
);

void shell_jobs(
    char *output,
    int output_size
);

int shell_fg(
    int job_id,
    char *output,
    int output_size
);

int shell_bg(
    int job_id,
    char *output,
    int output_size
);

/*
 * Signal notification.
 * The signal handler must only record the signal.
 * Actual process-group signalling is performed by the worker.
 */
void shell_handle_signal(int signal_number);

/* Called regularly by the worker outside signal handlers. */
void shell_process_pending_signal(void);

/* Exit state. */
int shell_exit_requested(void);

/* MultiWatch management. */
int shell_multiwatch_start(
    const char *command,
    char *output,
    int output_size
);

int shell_multiwatch_active(void);

int shell_multiwatch_poll(
    char *output,
    int output_size
);

void shell_multiwatch_stop(void);

#endif