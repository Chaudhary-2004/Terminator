#include "tab_runtime.h"
#include "shell.h"

#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>

#define RUNTIME_MAX_COMMAND 4096
#define RUNTIME_EVENT_INTERVAL_USEC 50000

/* Reserved status for unsolicited background output. */
#define RUNTIME_ASYNC_STATUS (-998)
#define RUNTIME_EXIT_STATUS  (-999)

typedef struct
{
    uint32_t length;
} RequestHeader;

typedef struct
{
    uint32_t length;
    int32_t status;
} ResponseHeader;

static pthread_mutex_t thread_cwd_mutex = PTHREAD_MUTEX_INITIALIZER;

static void runtime_signal_handler(int signal_number)
{
    /*
     * This function must only record the signal.
     * shell_process_pending_signal() performs the actual work.
     */
    shell_handle_signal(signal_number);
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));

    action.sa_handler = runtime_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    if (sigaction(SIGINT, &action, NULL) == -1)
        return -1;

    if (sigaction(SIGTSTP, &action, NULL) == -1)
        return -1;

    /*
     * Avoid terminating the worker if the GUI closes its socket.
     */
    signal(SIGPIPE, SIG_IGN);

    return 0;
}

static int read_all(int fd, void *buffer, size_t length)
{
    size_t used = 0;

    while (used < length)
    {
        ssize_t n = read(
            fd,
            (char *)buffer + used,
            length - used
        );

        if (n > 0)
        {
            used += (size_t)n;
        }
        else if (n == 0)
        {
            return -1;
        }
        else if (errno == EINTR)
        {
            shell_process_pending_signal();
        }
        else
        {
            return -1;
        }
    }

    return 0;
}

static int write_all(int fd, const void *buffer, size_t length)
{
    size_t used = 0;

    while (used < length)
    {
        ssize_t n = send(
            fd,
            (const char *)buffer + used,
            length - used,
            MSG_NOSIGNAL
        );

        if (n > 0)
        {
            used += (size_t)n;
        }
        else if (n < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            return -1;
        }
    }

    return 0;
}

static int send_response(
    int fd,
    const char *output,
    int status)
{
    ResponseHeader response;
    size_t length = output ? strlen(output) : 0;

    response.length = (uint32_t)length;
    response.status = status;

    if (write_all(fd, &response, sizeof(response)) != 0)
        return -1;

    if (length > 0 &&
        write_all(fd, output, length) != 0)
    {
        return -1;
    }

    return 0;
}

/*
 * Called only by the worker thread/process.
 *
 * Background jobs and MultiWatch are polled here, not in gui.c.
 */
static int poll_shell_events(int fd)
{
    char output[TAB_RUNTIME_BUFFER_SIZE];
    int produced = 0;

    output[0] = '\0';

    if (shell_poll_jobs(output, sizeof(output)))
    {
        if (send_response(fd, output, RUNTIME_ASYNC_STATUS) != 0)
            return -1;

        produced = 1;
    }

    output[0] = '\0';

    if (shell_multiwatch_active() &&
        shell_multiwatch_poll(output, sizeof(output)))
    {
        if (send_response(fd, output, RUNTIME_ASYNC_STATUS) != 0)
            return -1;

        produced = 1;
    }

    (void)produced;
    return 0;
}

/*
 * Read and execute one request.
 * Returns:
 *   0 = request handled
 *  -1 = socket closed/error
 */
static int handle_one_request(int fd, int thread_mode, char *thread_cwd)
{
    RequestHeader request;
    char command[RUNTIME_MAX_COMMAND];
    char output[TAB_RUNTIME_BUFFER_SIZE];
    int status = 0;

    if (read_all(fd, &request, sizeof(request)) != 0)
        return -1;

    if (request.length == 0 ||
        request.length >= sizeof(command))
    {
        return -1;
    }

    if (read_all(fd, command, request.length) != 0)
        return -1;

    command[request.length] = '\0';
    output[0] = '\0';

    shell_process_pending_signal();

    if (thread_mode)
    {
        char original_cwd[4096];

        pthread_mutex_lock(&thread_cwd_mutex);

        if (getcwd(original_cwd, sizeof(original_cwd)) == NULL)
            original_cwd[0] = '\0';

        if (thread_cwd[0] != '\0')
            (void)chdir(thread_cwd);

        run_command(
            command,
            output,
            sizeof(output),
            &status
        );

        if (getcwd(thread_cwd, 4096) == NULL)
            thread_cwd[0] = '\0';

        if (original_cwd[0] != '\0')
            (void)chdir(original_cwd);

        pthread_mutex_unlock(&thread_cwd_mutex);
    }
    else
    {
        run_command(
            command,
            output,
            sizeof(output),
            &status
        );
    }

    shell_process_pending_signal();

    if (shell_exit_requested())
        status = RUNTIME_EXIT_STATUS;

    if (send_response(fd, output, status) != 0)
        return -1;

    return shell_exit_requested() ? -1 : 0;
}

/*
 * The worker owns all shell state.
 *
 * It waits for commands while periodically polling background
 * jobs and MultiWatch.
 */
static void worker_loop(int fd, int thread_mode)
{
    char thread_cwd[4096];

    if (getcwd(thread_cwd, sizeof(thread_cwd)) == NULL)
        thread_cwd[0] = '\0';

    if (install_signal_handlers() == -1)
    {
        close(fd);
        return;
    }

    for (;;)
    {
        fd_set read_set;
        struct timeval timeout;
        int result;

        shell_process_pending_signal();

        /*
         * Poll background jobs and MultiWatch before waiting
         * for another command.
         */
        if (poll_shell_events(fd) != 0)
            break;

        FD_ZERO(&read_set);
        FD_SET(fd, &read_set);

        timeout.tv_sec = 0;
        timeout.tv_usec = RUNTIME_EVENT_INTERVAL_USEC;

        result = select(fd + 1, &read_set, NULL, NULL, &timeout);

        if (result < 0)
        {
            if (errno == EINTR)
            {
                shell_process_pending_signal();
                continue;
            }

            break;
        }

        if (result == 0)
            continue;

        if (FD_ISSET(fd, &read_set))
        {
            if (handle_one_request(fd, thread_mode, thread_cwd) != 0)
                break;
        }
    }

    if (shell_multiwatch_active())
        shell_multiwatch_stop();

    close(fd);
}

static void *thread_worker(void *arg)
{
    int fd = *(int *)arg;

    free(arg);

    worker_loop(fd, 1);

    return NULL;
}

int tab_runtime_start(TabRuntime *runtime, TerminatorMode mode)
{
    int pair[2];

    if (!runtime)
        return -1;

    memset(runtime, 0, sizeof(*runtime));

    runtime->fd = -1;
    runtime->mode = (int)mode;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return -1;

    if (mode == TERMINATOR_THREAD_MODE)
    {
        pthread_t thread;
        int *thread_fd = malloc(sizeof(*thread_fd));

        if (!thread_fd)
        {
            close(pair[0]);
            close(pair[1]);
            return -1;
        }

        *thread_fd = pair[1];

        if (pthread_create(
                &thread,
                NULL,
                thread_worker,
                thread_fd) != 0)
        {
            free(thread_fd);
            close(pair[0]);
            close(pair[1]);
            return -1;
        }

        runtime->thread_storage = malloc(sizeof(thread));

        if (!runtime->thread_storage)
        {
            shutdown(pair[0], SHUT_RDWR);
            close(pair[0]);
            pthread_join(thread, NULL);
            return -1;
        }

        memcpy(runtime->thread_storage, &thread, sizeof(thread));
    }
    else
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            close(pair[0]);
            close(pair[1]);
            return -1;
        }

        if (pid == 0)
        {
            close(pair[0]);
            worker_loop(pair[1], 0);
            _exit(0);
        }

        close(pair[1]);
        runtime->pid = pid;
    }

    runtime->fd = pair[0];

    {
        int flags = fcntl(runtime->fd, F_GETFL, 0);

        if (flags < 0 ||
            fcntl(runtime->fd, F_SETFL, flags | O_NONBLOCK) < 0)
        {
            tab_runtime_stop(runtime);
            return -1;
        }
    }

    runtime->active = 1;
    return 0;
}

int tab_runtime_submit(TabRuntime *runtime, const char *command)
{
    RequestHeader request;
    size_t length;

    if (!runtime || !runtime->active || !command)
        return -1;

    length = strlen(command);

    if (length == 0 || length >= RUNTIME_MAX_COMMAND)
        return -1;

    request.length = (uint32_t)length;

    /*
     * Requests are small. The GUI socket is nonblocking, so
     * EAGAIN is reported to the caller instead of blocking X11.
     */
    if (write_all(runtime->fd, &request, sizeof(request)) != 0)
        return -1;

    if (write_all(runtime->fd, command, length) != 0)
        return -1;

    return 0;
}

/*
 * Returns:
 *   1 = one complete message
 *   0 = no complete message available
 *  -1 = worker disconnected/error
 *
 * exit_status == -998 means asynchronous job/MultiWatch output.
 */
int tab_runtime_poll(
    TabRuntime *runtime,
    char *output,
    size_t output_size,
    int *exit_status)
{
    ResponseHeader header;
    ssize_t n;
    size_t target;

    if (!runtime || !runtime->active ||
        !output || output_size == 0)
    {
        return -1;
    }

    output[0] = '\0';

    for (;;)
    {
        if (runtime->rx_used >= sizeof(ResponseHeader))
        {
            memcpy(&header, runtime->rx, sizeof(header));

            if (header.length >
                sizeof(runtime->rx) - sizeof(header))
            {
                return -1;
            }

            target = sizeof(header) + header.length;
            runtime->rx_needed = target;

            if (runtime->rx_used >= target)
            {
                size_t copy = header.length;

                if (copy >= output_size)
                    copy = output_size - 1;

                memcpy(
                    output,
                    runtime->rx + sizeof(header),
                    copy
                );

                output[copy] = '\0';

                if (exit_status)
                    *exit_status = header.status;

                memmove(
                    runtime->rx,
                    runtime->rx + target,
                    runtime->rx_used - target
                );

                runtime->rx_used -= target;
                runtime->rx_needed = 0;

                return 1;
            }
        }

        if (runtime->rx_used == sizeof(runtime->rx))
            return -1;

        n = read(
            runtime->fd,
            runtime->rx + runtime->rx_used,
            sizeof(runtime->rx) - runtime->rx_used
        );

        if (n > 0)
        {
            runtime->rx_used += (size_t)n;
        }
        else if (n == 0)
        {
            return -1;
        }
        else if (errno == EINTR)
        {
            continue;
        }
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return 0;
        }
        else
        {
            return -1;
        }
    }
}

void tab_runtime_signal(TabRuntime *runtime, int signal_number)
{
    if (!runtime || !runtime->active)
        return;

    if (runtime->mode == TERMINATOR_THREAD_MODE &&
        runtime->thread_storage)
    {
        pthread_t thread;

        memcpy(&thread, runtime->thread_storage, sizeof(thread));

        (void)pthread_kill(thread, signal_number);
    }
    else if (runtime->pid > 0)
    {
        (void)kill(runtime->pid, signal_number);
    }
}

void tab_runtime_stop(TabRuntime *runtime)
{
    if (!runtime)
        return;

    if (runtime->fd >= 0)
    {
        shutdown(runtime->fd, SHUT_RDWR);
        close(runtime->fd);
        runtime->fd = -1;
    }

    if (runtime->mode == TERMINATOR_THREAD_MODE &&
        runtime->thread_storage)
    {
        pthread_t thread;

        memcpy(&thread, runtime->thread_storage, sizeof(thread));

        /*
         * Closing the socket wakes the worker if it is idle.
         * If it is running a foreground command, this join waits
         * for that command to finish.
         */
        pthread_join(thread, NULL);

        free(runtime->thread_storage);
        runtime->thread_storage = NULL;
    }
    else if (runtime->mode == TERMINATOR_PROCESS_MODE &&
             runtime->pid > 0)
    {
        int status;
        pid_t waited = waitpid(runtime->pid, &status, WNOHANG);

        if (waited == 0)
        {
            (void)kill(runtime->pid, SIGTERM);

            while (waitpid(runtime->pid, &status, 0) < 0 &&
                   errno == EINTR)
            {
                ;
            }
        }

        runtime->pid = 0;
    }

    runtime->active = 0;
}