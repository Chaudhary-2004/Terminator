#include "shell.h"
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <stdint.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/stat.h>

#define MW_MAX 16
#define MW_INTERVAL 2
#define MW_BUFFER 16384

#define MW_SEPARATOR \
    "----------------------------------------------------\n"

/* =========================================================
   DATA STRUCTURES
   ========================================================= */

typedef struct
{
    int id;
    pid_t pgid;

    pid_t pids[MAX_COMMANDS];

    int alive[MAX_COMMANDS];
    int stopped_processes[MAX_COMMANDS];

    int process_count;

    int output_fd;

    int active;
    int background;
    int stopped;

    char command[MAX_COMMAND];

} Job;

typedef struct
{
    char command[MAX_COMMAND];

    pid_t pid;
    int pipe_fd;

    int active;
    int running;
    int reaped;

    time_t last_start;

    char output[MW_BUFFER];
    size_t output_len;

} MultiWatchCommand;

/* =========================================================
   WORKER-OWNED SHELL STATE

   Each tab worker owns its own copy of these variables.
   GUI code must not call shell functions that manipulate
   this state from the GUI thread.
   ========================================================= */

static _Thread_local Job jobs[SHELL_MAX_JOBS];

static _Thread_local MultiWatchCommand mw[MW_MAX];

static _Thread_local int next_job_id = 1;

static _Thread_local int last_status = 0;

static _Thread_local int exit_requested = 0;

static _Thread_local pid_t foreground_pgid = 0;

static _Thread_local int mw_active = 0;

static _Thread_local ShellOutputCallback output_callback = NULL;
static _Thread_local void *output_callback_context = NULL;
/*
 * The asynchronous signal handler only writes this variable.
 * The worker processes it later in normal execution.
 */
static _Thread_local volatile sig_atomic_t pending_signal = 0;

void shell_set_output_callback(
    ShellOutputCallback callback,
    void *context)
{
    output_callback = callback;
    output_callback_context = context;
}
/* =========================================================
   GENERAL HELPERS
   ========================================================= */

static void append_text(
    char *output,
    int output_size,
    size_t *used,
    const char *text)
{
    size_t room;
    size_t length;

    if (!output || !used || !text || output_size <= 0)
        return;

    if (*used >= (size_t)output_size - 1)
        return;

    room = (size_t)output_size - 1 - *used;

    length = strlen(text);

    if (length > room)
        length = room;

    memcpy(output + *used, text, length);

    *used += length;

    output[*used] = '\0';
}

static void close_pipes(int pipes[][2], int count)
{
    int a;

    for (a = 0; a < count; a++)
    {
        if (pipes[a][0] >= 0)
            close(pipes[a][0]);

        if (pipes[a][1] >= 0)
            close(pipes[a][1]);

        pipes[a][0] = -1;
        pipes[a][1] = -1;
    }
}

static int set_nonblocking(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1)
        return -1;

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int all_processes_done(Job *job)
{
    int a;

    for (a = 0; a < job->process_count; a++)
    {
        if (job->alive[a])
            return 0;
    }

    return 1;
}

static int all_processes_stopped(Job *job)
{
    int a;
    int found = 0;

    for (a = 0; a < job->process_count; a++)
    {
        if (job->alive[a])
        {
            found = 1;

            if (!job->stopped_processes[a])
                return 0;
        }
    }

    return found;
}

/* =========================================================
   SIGNAL HANDLING
   ========================================================= */

/*
 * Called from the asynchronous signal handler.
 *
 * Do not call kill(), waitpid(), close(), malloc(), printf()
 * or other normal shell functions here.
 */
void shell_handle_signal(int signal_number)
{
    pending_signal = signal_number;
}

/*
 * Called by the worker during normal execution.
 *
 * This is where signals are actually forwarded to the
 * foreground process group.
 */
void shell_process_pending_signal(void)
{
    sig_atomic_t signal_number;

    signal_number = pending_signal;

    if (signal_number == 0)
        return;

    pending_signal = 0;

    if (mw_active)
    {
        shell_multiwatch_stop();
        return;
    }

    if (foreground_pgid > 0)
    {
        (void)kill(-foreground_pgid, (int)signal_number);
    }
}

int shell_foreground_running(void)
{
    return foreground_pgid > 0;
}

/* =========================================================
   JOB TABLE
   ========================================================= */

static Job *allocate_job(void)
{
    int a;

    for (a = 0; a < SHELL_MAX_JOBS; a++)
    {
        if (!jobs[a].active)
        {
            memset(&jobs[a], 0, sizeof(jobs[a]));

            jobs[a].id = next_job_id++;

            jobs[a].active = 1;

            jobs[a].output_fd = -1;

            return &jobs[a];
        }
    }

    return NULL;
}

static Job *find_job(int id)
{
    int a;

    for (a = 0; a < SHELL_MAX_JOBS; a++)
    {
        if (jobs[a].active && jobs[a].id == id)
            return &jobs[a];
    }

    return NULL;
}

static void remove_job(Job *job)
{
    if (!job)
        return;

    if (job->output_fd >= 0)
    {
        close(job->output_fd);
        job->output_fd = -1;
    }

    memset(job, 0, sizeof(*job));

    job->output_fd = -1;
}

/* =========================================================
   JOB OUTPUT COLLECTION
   ========================================================= */

static void collect_job_output(
    Job *job,
    char *output,
    int output_size,
    size_t *used)
{
    if (!job || job->output_fd < 0)
        return;

    for (;;)
    {
        char buffer[1024];

        ssize_t n = read(
            job->output_fd,
            buffer,
            sizeof(buffer));

        if (n > 0)
        {
            size_t room;
            size_t take;

            if (*used >= (size_t)output_size - 1)
                continue;

            room = (size_t)output_size - 1 - *used;

            take = (size_t)n < room
                       ? (size_t)n
                       : room;

            memcpy(output + *used, buffer, take);

            *used += take;

            output[*used] = '\0';

            continue;
        }

        if (n == 0)
        {
            close(job->output_fd);

            job->output_fd = -1;

            break;
        }

        if (errno == EINTR)
            continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;

        close(job->output_fd);

        job->output_fd = -1;

        break;
    }
}

/* =========================================================
   WAIT FOR A JOB

   This is the important replacement for your old foreground
   loop.

   We monitor:
     1. Output pipe
     2. Child completion
     3. Child stopping
     4. Pending Ctrl+C / Ctrl+Z signals
   ========================================================= */
static void stream_new_output(
    char *output,
    size_t *used,
    size_t previous_used)
{
    if (!output_callback || *used <= previous_used)
        return;

    output_callback(
        output + previous_used,
        output_callback_context);

    /* Remove the bytes that were already streamed. */
    output[previous_used] = '\0';

    *used = previous_used;
}

/*Foreground JOB Polling*/
static int monitor_job(
    Job *job,
    char *output,
    int output_size,
    int foreground)
{
    size_t used = strlen(output);

    if (!job)
        return 1;

    if (foreground)
    {
        job->background = 0;

        job->stopped = 0;

        foreground_pgid = job->pgid;
    }

    for (;;)
    {
        int a;

        int all_done;
        int all_stopped;

        shell_process_pending_signal();

        /*
         * Read available output without blocking.
         */
        {
            size_t previous_used = used;

            collect_job_output(
                job,
                output,
                output_size,
                &used);

            stream_new_output(
                output,
                &used,
                previous_used);
        }

        /*
         * Check each child.
         */
        for (a = 0; a < job->process_count; a++)
        {
            int status;

            pid_t result;

            if (!job->alive[a])
                continue;

            result = waitpid(
                job->pids[a],
                &status,
                WNOHANG | WUNTRACED | WCONTINUED);

            if (result == job->pids[a])
            {
                if (WIFSTOPPED(status))
                {
                    job->stopped_processes[a] = 1;
                }
                else if (WIFCONTINUED(status))
                {
                    job->stopped_processes[a] = 0;
                }
                else if (WIFEXITED(status) ||
                         WIFSIGNALED(status))
                {
                    job->alive[a] = 0;

                    job->stopped_processes[a] = 0;
                }
            }
            else if (result == -1)
            {
                if (errno == ECHILD)
                {
                    job->alive[a] = 0;

                    job->stopped_processes[a] = 0;
                }
            }
        }

        all_done = all_processes_done(job);

        all_stopped = all_processes_stopped(job);

        /*
         * All processes finished.
         */
        if (all_done)
        {
            /*
             * Drain remaining pipe output.
             */
            {
                size_t previous_used = used;

                collect_job_output(
                    job,
                    output,
                    output_size,
                    &used);

                stream_new_output(
                    output,
                    &used,
                    previous_used);
            }

            if (job->output_fd >= 0)
            {
                close(job->output_fd);
                job->output_fd = -1;
            }

            if (foreground)
                foreground_pgid = 0;

            remove_job(job);

            return 0;
        }

        /*
         * All remaining processes stopped.
         *
         * This is what allows Ctrl+Z to return to the prompt.
         */
        if (all_stopped)
        {
            job->stopped = 1;

            job->background = 1;

            if (foreground)
                foreground_pgid = 0;

            {
                char line[128];

                snprintf(
                    line,
                    sizeof(line),
                    "\n[%d] Stopped\n",
                    job->id);

                append_text(
                    output,
                    output_size,
                    &used,
                    line);
            }

            return 128 + SIGTSTP;
        }

        /*
         * Background jobs are not waited for synchronously.
         */
        if (!foreground)
            return 0;

        /*
         * Small sleep to avoid consuming a CPU core.
         * The loop checks signals and process states every 10ms.
         */
        {
            struct timespec delay;

            delay.tv_sec = 0;

            delay.tv_nsec = 10000000L;

            nanosleep(&delay, NULL);
        }
    }
}

/* =========================================================
   MULTIWATCH PARSER
   ========================================================= */

static int parse_mw(
    const char *input,
    char commands[MW_MAX][MAX_COMMAND])
{
    const char *p;

    int count = 0;

    if (!input)
        return -1;

    p = strchr(input, '[');

    if (!p)
        return -1;

    p++;

    for (;;)
    {
        int len = 0;

        while (*p == ' ' || *p == '\t' ||
               *p == '\n' || *p == ',')
        {
            p++;
        }

        if (*p == ']')
        {
            p++;

            while (*p == ' ' || *p == '\t' || *p == '\n')
                p++;

            return (*p == '\0' && count > 0)
                       ? count
                       : -1;
        }

        if (*p != '"' || count >= MW_MAX)
            return -1;

        p++;

        while (*p != '\0' && *p != '"')
        {
            if (len >= MAX_COMMAND - 1)
                return -1;

            commands[count][len++] = *p++;
        }

        if (*p != '"' || len == 0)
            return -1;

        commands[count][len] = '\0';

        count++;

        p++;

        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;

        if (*p == ']')
            continue;

        if (*p != ',')
            return -1;

        p++;
    }
}

/* =========================================================
   MULTIWATCH START
   ========================================================= */

static int start_mw_command(int index)
{
    int fds[2];

    pid_t pid;

    if (pipe(fds) == -1)
        return -1;

    pid = fork();

    if (pid == -1)
    {
        int saved_errno = errno;

        close(fds[0]);
        close(fds[1]);

        errno = saved_errno;

        return -1;
    }

    if (pid == 0)
    {
        close(fds[0]);

        (void)setpgid(0, 0);

        if (dup2(fds[1], STDOUT_FILENO) == -1 ||
            dup2(fds[1], STDERR_FILENO) == -1)
        {
            _exit(126);
        }

        close(fds[1]);

        execl(
            "/bin/sh",
            "sh",
            "-c",
            mw[index].command,
            (char *)NULL);

        _exit(127);
    }

    close(fds[1]);

    (void)setpgid(pid, pid);

    if (set_nonblocking(fds[0]) == -1)
    {
        int saved_errno = errno;

        (void)kill(-pid, SIGKILL);

        close(fds[0]);

        while (waitpid(pid, NULL, 0) == -1 &&
               errno == EINTR)
        {
            ;
        }

        errno = saved_errno;

        return -1;
    }

    mw[index].pid = pid;

    mw[index].pipe_fd = fds[0];

    mw[index].running = 1;

    mw[index].reaped = 0;

    mw[index].last_start = time(NULL);

    mw[index].output_len = 0;

    mw[index].output[0] = '\0';

    return 0;
}

int shell_multiwatch_start(
    const char *command,
    char *output,
    int output_size)
{
    char parsed[MW_MAX][MAX_COMMAND];

    int count;

    int a;

    if (!output || output_size <= 0)
        return 1;

    output[0] = '\0';

    if (mw_active)
    {
        snprintf(
            output,
            output_size,
            "multiWatch: already running\n");

        return 1;
    }

    count = parse_mw(command, parsed);

    if (count < 1)
    {
        snprintf(
            output,
            output_size,
            "Usage: multiWatch [\"cmd1\", \"cmd2\", ...]\n");

        return 1;
    }

    memset(mw, 0, sizeof(mw));

    for (a = 0; a < MW_MAX; a++)
        mw[a].pipe_fd = -1;

    for (a = 0; a < count; a++)
    {
        snprintf(
            mw[a].command,
            sizeof(mw[a].command),
            "%s",
            parsed[a]);

        mw[a].active = 1;
    }

    mw_active = 1;

    for (a = 0; a < count; a++)
    {
        if (start_mw_command(a) == -1)
        {
            int saved_errno = errno;

            shell_multiwatch_stop();

            snprintf(
                output,
                output_size,
                "multiWatch: %s\n",
                strerror(saved_errno));

            return 1;
        }
    }

    return 0;
}

int shell_multiwatch_active(void)
{
    return mw_active;
}

/* =========================================================
   MULTIWATCH POLL
   ========================================================= */

int shell_multiwatch_poll(char *output, int output_size)
{
    fd_set read_set;

    struct timeval timeout;

    int max_fd = -1;

    int selected;

    int a;

    int produced = 0;

    size_t used = 0;

    time_t now;

    if (!output || output_size <= 0 || !mw_active)
        return 0;

    output[0] = '\0';

    FD_ZERO(&read_set);

    for (a = 0; a < MW_MAX; a++)
    {
        if (mw[a].active &&
            mw[a].running &&
            mw[a].pipe_fd >= 0 &&
            mw[a].pipe_fd < FD_SETSIZE)
        {
            FD_SET(mw[a].pipe_fd, &read_set);

            if (mw[a].pipe_fd > max_fd)
                max_fd = mw[a].pipe_fd;
        }
    }

    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    selected = max_fd >= 0
                   ? select(
                         max_fd + 1,
                         &read_set,
                         NULL,
                         NULL,
                         &timeout)
                   : 0;

    if (selected < 0 && errno != EINTR)
        return 0;

    if (selected > 0)
    {
        for (a = 0; a < MW_MAX; a++)
        {
            MultiWatchCommand *item = &mw[a];

            if (!item->active ||
                !item->running ||
                item->pipe_fd < 0 ||
                !FD_ISSET(item->pipe_fd, &read_set))
            {
                continue;
            }

            for (;;)
            {
                char buffer[1024];

                ssize_t n = read(
                    item->pipe_fd,
                    buffer,
                    sizeof(buffer));

                if (n > 0)
                {
                    size_t room =
                        sizeof(item->output) - 1 -
                        item->output_len;

                    size_t take =
                        (size_t)n < room
                            ? (size_t)n
                            : room;

                    if (take > 0)
                    {
                        memcpy(
                            item->output + item->output_len,
                            buffer,
                            take);

                        item->output_len += take;

                        item->output[item->output_len] = '\0';
                    }

                    continue;
                }

                if (n == 0)
                {
                    char header[MAX_COMMAND + 64];

                    snprintf(
                        header,
                        sizeof(header),
                        "\"%s\", %ld:\n%s",
                        item->command,
                        (long)time(NULL),
                        MW_SEPARATOR);

                    append_text(
                        output,
                        output_size,
                        &used,
                        header);

                    append_text(
                        output,
                        output_size,
                        &used,
                        item->output);

                    if (item->output_len > 0 &&
                        item->output[item->output_len - 1] != '\n')
                    {
                        append_text(
                            output,
                            output_size,
                            &used,
                            "\n");
                    }

                    append_text(
                        output,
                        output_size,
                        &used,
                        MW_SEPARATOR);

                    close(item->pipe_fd);

                    item->pipe_fd = -1;

                    item->running = 0;

                    item->last_start = time(NULL);

                    item->output_len = 0;

                    item->output[0] = '\0';

                    {
                        pid_t r = waitpid(
                            item->pid,
                            NULL,
                            WNOHANG);

                        if (r == item->pid ||
                            (r == -1 && errno == ECHILD))
                        {
                            item->reaped = 1;
                        }
                    }

                    produced = 1;

                    break;
                }

                if (errno == EINTR)
                    continue;

                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;

                close(item->pipe_fd);

                item->pipe_fd = -1;

                item->running = 0;

                item->last_start = time(NULL);

                break;
            }
        }
    }

    now = time(NULL);

    for (a = 0; a < MW_MAX; a++)
    {
        MultiWatchCommand *item = &mw[a];

        if (!item->active || item->running)
            continue;

        if (!item->reaped && item->pid > 0)
        {
            pid_t r = waitpid(
                item->pid,
                NULL,
                WNOHANG);

            if (r == item->pid ||
                (r == -1 && errno == ECHILD))
            {
                item->reaped = 1;
            }
        }

        if (item->reaped &&
            now - item->last_start >= MW_INTERVAL)
        {
            if (start_mw_command(a) == -1)
            {
                char message[MAX_COMMAND + 128];

                snprintf(
                    message,
                    sizeof(message),
                    "multiWatch: restart failed for \"%s\": %s\n",
                    item->command,
                    strerror(errno));

                append_text(
                    output,
                    output_size,
                    &used,
                    message);

                item->last_start = now;

                produced = 1;
            }
        }
    }

    return produced;
}

/* =========================================================
   MULTIWATCH STOP
   ========================================================= */

void shell_multiwatch_stop(void)
{
    int a;

    if (!mw_active)
        return;

    mw_active = 0;

    for (a = 0; a < MW_MAX; a++)
    {
        if (mw[a].active &&
            mw[a].running &&
            mw[a].pid > 0)
        {
            (void)kill(-mw[a].pid, SIGTERM);
        }
    }

    for (a = 0; a < MW_MAX; a++)
    {
        MultiWatchCommand *item = &mw[a];

        if (item->active &&
            item->running &&
            item->pid > 0)
        {
            int status;

            pid_t r = waitpid(
                item->pid,
                &status,
                WNOHANG);

            if (r == 0)
            {
                (void)kill(-item->pid, SIGKILL);

                while (waitpid(
                           item->pid,
                           &status,
                           0) == -1 &&
                       errno == EINTR)
                {
                    ;
                }
            }
        }

        if (item->pipe_fd >= 0)
            close(item->pipe_fd);

        item->pipe_fd = -1;

        item->active = 0;

        item->running = 0;

        item->pid = 0;

        item->reaped = 1;

        item->output_len = 0;

        item->output[0] = '\0';
    }
}

/* =========================================================
   BACKGROUND JOB POLLING
   ========================================================= */

int shell_poll_jobs(char *output, int output_size)
{
    int a;

    size_t used = 0;

    if (!output || output_size <= 0)
        return 0;

    output[0] = '\0';

    for (a = 0; a < SHELL_MAX_JOBS; a++)
    {
        Job *job = &jobs[a];

        int b;

        if (!job->active || !job->background)
            continue;

        collect_job_output(
            job,
            output,
            output_size,
            &used);

        for (b = 0; b < job->process_count; b++)
        {
            int status;

            pid_t r;

            if (!job->alive[b])
                continue;

            r = waitpid(
                job->pids[b],
                &status,
                WNOHANG | WUNTRACED | WCONTINUED);

            if (r == job->pids[b])
            {
                if (WIFSTOPPED(status))
                {
                    job->stopped_processes[b] = 1;

                    job->stopped = 1;
                }
                else if (WIFCONTINUED(status))
                {
                    job->stopped_processes[b] = 0;
                }
                else if (WIFEXITED(status) ||
                         WIFSIGNALED(status))
                {
                    job->alive[b] = 0;

                    job->stopped_processes[b] = 0;
                }
            }
            else if (r == -1 && errno == ECHILD)
            {
                job->alive[b] = 0;

                job->stopped_processes[b] = 0;
            }
        }

        if (all_processes_done(job) &&
            job->output_fd == -1)
        {
            char line[MAX_COMMAND + 64];

            snprintf(
                line,
                sizeof(line),
                "\n[%d] Done: %s\n",
                job->id,
                job->command);

            append_text(
                output,
                output_size,
                &used,
                line);

            remove_job(job);
        }
    }

    return used > 0 ? 1 : 0;
}

/* =========================================================
   JOB LIST
   ========================================================= */

void shell_jobs(char *output, int output_size)
{
    int a;

    size_t used = 0;

    if (!output || output_size <= 0)
        return;

    output[0] = '\0';

    for (a = 0; a < SHELL_MAX_JOBS; a++)
    {
        if (jobs[a].active)
        {
            char line[MAX_COMMAND + 64];

            snprintf(
                line,
                sizeof(line),
                "[%d] %s %s\n",
                jobs[a].id,
                jobs[a].stopped
                    ? "Stopped"
                    : (jobs[a].background
                           ? "Running"
                           : "Foreground"),
                jobs[a].command);

            append_text(
                output,
                output_size,
                &used,
                line);
        }
    }

    if (used == 0)
        snprintf(output, output_size, "No jobs.\n");
}

/* =========================================================
   BG COMMAND
   ========================================================= */

int shell_bg(int job_id, char *output, int output_size)
{
    Job *job;

    if (!output || output_size <= 0)
        return 1;

    output[0] = '\0';

    job = find_job(job_id);

    if (!job)
    {
        snprintf(
            output,
            output_size,
            "bg: job not found\n");

        return 1;
    }

    if (kill(-job->pgid, SIGCONT) == -1)
    {
        snprintf(
            output,
            output_size,
            "bg: %s\n",
            strerror(errno));

        return 1;
    }

    job->background = 1;

    job->stopped = 0;

    {
        int a;

        for (a = 0; a < job->process_count; a++)
            job->stopped_processes[a] = 0;
    }

    snprintf(
        output,
        output_size,
        "[%d] continued in background\n",
        job->id);

    return 0;
}

/* =========================================================
   FG COMMAND
   ========================================================= */

int shell_fg(int job_id, char *output, int output_size)
{
    Job *job;

    int result;

    if (!output || output_size <= 0)
        return 1;

    output[0] = '\0';

    job = find_job(job_id);

    if (!job)
    {
        snprintf(
            output,
            output_size,
            "fg: job not found\n");

        return 1;
    }

    if (kill(-job->pgid, SIGCONT) == -1)
    {
        snprintf(
            output,
            output_size,
            "fg: %s\n",
            strerror(errno));

        return 1;
    }

    job->background = 0;

    job->stopped = 0;

    {
        int a;

        for (a = 0; a < job->process_count; a++)
            job->stopped_processes[a] = 0;
    }

    result = monitor_job(
        job,
        output,
        output_size,
        1);

    return result;
}

/* =========================================================
   BUILT-IN COMMANDS
   ========================================================= */

static int run_builtin(
    Command *cmd,
    char *output,
    int output_size,
    int *handled)
{
    int id;

    *handled = 1;

    output[0] = '\0';

    if (!cmd->args[0])
        return 0;

    if (strcmp(cmd->args[0], "cd") == 0)
    {
        const char *path = cmd->args[1];

        if (!path)
            path = getenv("HOME");

        if (!path)
        {
            snprintf(
                output,
                output_size,
                "cd: HOME is not set\n");

            return 1;
        }

        if (chdir(path) == -1)
        {
            snprintf(
                output,
                output_size,
                "cd: %s\n",
                strerror(errno));

            return 1;
        }

        return 0;
    }

    if (strcmp(cmd->args[0], "pwd") == 0)
    {
        char cwd[PATH_MAX];

        if (!getcwd(cwd, sizeof(cwd)))
        {
            snprintf(
                output,
                output_size,
                "pwd: %s\n",
                strerror(errno));

            return 1;
        }

        snprintf(
            output,
            output_size,
            "%s\n",
            cwd);

        return 0;
    }

    if (strcmp(cmd->args[0], "jobs") == 0)
    {
        shell_jobs(output, output_size);

        return 0;
    }

    if (strcmp(cmd->args[0], "bg") == 0)
    {
        id = cmd->args[1]
                 ? atoi(cmd->args[1])
                 : 0;

        return shell_bg(id, output, output_size);
    }

    if (strcmp(cmd->args[0], "fg") == 0)
    {
        id = cmd->args[1]
                 ? atoi(cmd->args[1])
                 : 0;

        return shell_fg(id, output, output_size);
    }

    if (strcmp(cmd->args[0], "multiWatch") == 0)
    {
        return shell_multiwatch_start(
            "",
            output,
            output_size);
    }

    if (strcmp(cmd->args[0], "exit") == 0)
    {
        if (mw_active)
            shell_multiwatch_stop();

        exit_requested = 1;

        return 0;
    }

    *handled = 0;

    return 0;
}

/* =========================================================
   PIPELINE EXECUTION
   ========================================================= */

static int execute_pipeline(
    Command cmds[],
    int count,
    char *output,
    int output_size)
{
    int pipes[MAX_COMMANDS - 1][2];

    int capture[2] = {-1, -1};

    pid_t pids[MAX_COMMANDS];

    int pipe_count;

    int a;
    int b;

    int fork_failed = 0;

    int background;

    Job *job;

    output[0] = '\0';

    if (count <= 0 || count > MAX_COMMANDS)
        return 1;

    pipe_count = count - 1;

    background = cmds[0].background;

    for (a = 0; a < pipe_count; a++)
    {
        pipes[a][0] = -1;
        pipes[a][1] = -1;
    }

    job = allocate_job();

    if (!job)
    {
        snprintf(
            output,
            output_size,
            "shell: job table full\n");

        return 1;
    }

    /*
     * Construct readable command string for jobs.
     */
    for (a = 0; a < count; a++)
    {
        if (a > 0)
        {
            strncat(
                job->command,
                " | ",
                sizeof(job->command) -
                    strlen(job->command) - 1);
        }

        for (b = 0; cmds[a].args[b]; b++)
        {
            if (b > 0)
            {
                strncat(
                    job->command,
                    " ",
                    sizeof(job->command) -
                        strlen(job->command) - 1);
            }

            strncat(
                job->command,
                cmds[a].args[b],
                sizeof(job->command) -
                    strlen(job->command) - 1);
        }
    }

    for (a = 0; a < pipe_count; a++)
    {
        if (pipe(pipes[a]) == -1)
        {
            snprintf(
                output,
                output_size,
                "pipe: %s\n",
                strerror(errno));

            close_pipes(pipes, pipe_count);

            remove_job(job);

            return 1;
        }
    }

    if (pipe(capture) == -1)
    {
        snprintf(
            output,
            output_size,
            "pipe: %s\n",
            strerror(errno));

        close_pipes(pipes, pipe_count);

        remove_job(job);

        return 1;
    }

    /*
     * Fork all pipeline processes.
     */
    for (a = 0; a < count; a++)
    {
        pid_t pid = fork();

        if (pid == -1)
        {
            fork_failed = 1;

            break;
        }

        if (pid == 0)
        {
            /*
             * Child process group.
             */
            if (a == 0)
                (void)setpgid(0, 0);
            else
                (void)setpgid(0, pids[0]);

            close(capture[0]);

            /*
             * Connect previous pipeline output to stdin.
             */
            if (a > 0)
            {
                if (dup2(
                        pipes[a - 1][0],
                        STDIN_FILENO) == -1)
                {
                    _exit(126);
                }
            }

            /*
             * Connect stdout to next pipeline process.
             */
            if (a < count - 1)
            {
                if (dup2(
                        pipes[a][1],
                        STDOUT_FILENO) == -1)
                {
                    _exit(126);
                }
            }
            else if (!cmds[a].output_file)
            {
                if (dup2(
                        capture[1],
                        STDOUT_FILENO) == -1)
                {
                    _exit(126);
                }
            }

            /*
             * Capture stderr.
             */
            if (dup2(
                    capture[1],
                    STDERR_FILENO) == -1)
            {
                _exit(126);
            }

            close(capture[1]);

            close_pipes(pipes, pipe_count);

            /*
             * Input redirection.
             */
            if (cmds[a].input_file)
            {
                int fd = open(
                    cmds[a].input_file,
                    O_RDONLY);

                if (fd == -1 ||
                    dup2(fd, STDIN_FILENO) == -1)
                {
                    dprintf(
                        STDERR_FILENO,
                        "%s: %s\n",
                        cmds[a].input_file,
                        strerror(errno));

                    _exit(126);
                }

                close(fd);
            }

            /*
             * Output redirection.
             */
            if (cmds[a].output_file)
            {
                int flags = O_WRONLY | O_CREAT;

                if (cmds[a].append_output)
                    flags |= O_APPEND;
                else
                    flags |= O_TRUNC;

                int fd = open(
                    cmds[a].output_file,
                    flags,
                    0666);

                if (fd == -1 ||
                    dup2(fd, STDOUT_FILENO) == -1)
                {
                    dprintf(
                        STDERR_FILENO,
                        "%s: %s\n",
                        cmds[a].output_file,
                        strerror(errno));

                    _exit(126);
                }

                close(fd);
            }

          
            execvp(
                cmds[a].args[0],
                cmds[a].args);

            dprintf(
                STDERR_FILENO,
                "%s: %s\n",
                cmds[a].args[0],
                strerror(errno));

            _exit(errno == ENOENT ? 127 : 126);
        }

        pids[a] = pid;

        if (a == 0)
            job->pgid = pid;

        (void)setpgid(pid, job->pgid);
    }

    close_pipes(pipes, pipe_count);

    close(capture[1]);

    if (fork_failed)
    {
        int saved_errno = errno;

        for (b = 0; b < a; b++)
            (void)kill(pids[b], SIGTERM);

        for (b = 0; b < a; b++)
        {
            while (waitpid(pids[b], NULL, 0) == -1 &&
                   errno == EINTR)
            {
                ;
            }
        }

        close(capture[0]);

        remove_job(job);

        snprintf(
            output,
            output_size,
            "fork: %s\n",
            strerror(saved_errno));

        return 1;
    }

    /*
     * Save process information.
     */
    job->process_count = count;

    job->output_fd = capture[0];

    job->background = background;

    job->stopped = 0;

    for (a = 0; a < count; a++)
    {
        job->pids[a] = pids[a];

        job->alive[a] = 1;

        job->stopped_processes[a] = 0;
    }

    /*
     * The capture pipe must be nonblocking so that the
     * monitoring loop can inspect child process states.
     */
    if (set_nonblocking(capture[0]) == -1)
    {
        /*
         * We can still monitor the children, but a blocking
         * output read would be unsafe. Fail and terminate.
         */
        (void)kill(-job->pgid, SIGTERM);

        for (a = 0; a < count; a++)
        {
            while (waitpid(pids[a], NULL, 0) == -1 &&
                   errno == EINTR)
            {
                ;
            }
        }

        remove_job(job);

        return 1;
    }

    if (background)
    {
        char line[128];

        snprintf(
            line,
            sizeof(line),
            "[%d] %ld\n",
            job->id,
            (long)job->pgid);

        snprintf(output, output_size, "%s", line);

        return 0;
    }

    /*
     * Foreground process.
     *
     * monitor_job() returns when:
     *   - all processes finish
     *   - all remaining processes stop
     */
    return monitor_job(
        job,
        output,
        output_size,
        1);
}

/* =========================================================
   RUN COMMAND
   ========================================================= */

void run_command(
    const char *command,
    char *output,
    int output_size,
    int *exit_status)
{
    char storage[MAX_COMMAND];

    Command cmds[MAX_COMMANDS];

    int count;

    int handled = 0;

    int result = 0;

    if (!output || output_size <= 0)
        return;

    output[0] = '\0';

    if (exit_status)
        *exit_status = 0;

    if (!command)
        return;

    shell_process_pending_signal();

    if (strlen(command) >= sizeof(storage))
    {
        snprintf(
            output,
            output_size,
            "shell: command too long\n");

        if (exit_status)
            *exit_status = 1;

        last_status = 1;

        return;
    }

    snprintf(
        storage,
        sizeof(storage),
        "%s",
        command);

    /*
     * Special handling for multiWatch because it requires
     * the complete original command, including quotes.
     */
    if (strncmp(command, "multiWatch", 10) == 0 &&
        (command[10] == '\0' ||
         command[10] == ' ' ||
         command[10] == '\t' ||
         command[10] == '['))
    {
        result = shell_multiwatch_start(
            command,
            output,
            output_size);

        last_status = result;

        if (exit_status)
            *exit_status = result;

        return;
    }

    count = parse_pipeline(
        storage,
        cmds,
        MAX_COMMANDS);

    if (count <= 0)
    {
        last_status = 0;

        return;
    }

    /*
     * Single built-in command without redirection/background.
     */
    if (count == 1 &&
        cmds[0].input_file == NULL &&
        cmds[0].output_file == NULL &&
        !cmds[0].background)
    {
        result = run_builtin(
            &cmds[0],
            output,
            output_size,
            &handled);

        if (handled)
        {
            last_status = result;

            if (exit_status)
                *exit_status = result;

            return;
        }
    }

    result = execute_pipeline(
        cmds,
        count,
        output,
        output_size);

    last_status = result;

    if (exit_status)
        *exit_status = result;
}

/* =========================================================
   EXIT STATE
   ========================================================= */

int shell_exit_requested(void)
{
    return exit_requested;
}