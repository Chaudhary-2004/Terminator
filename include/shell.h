#ifndef SHELL_H
#define SHELL_H

#define SHELL_MAX_JOBS 64

void run_command(
    const char *command,
    char *output,
    int output_size,
    int *exit_status
);

int shell_foreground_running(void);

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

void shell_handle_signal(int signal_number);

int shell_exit_requested(void);

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