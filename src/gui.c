#include "gui.h"
#include "tab_runtime.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <signal.h>
#include <errno.h>

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 500
#define GUI_MAX_INPUT 1024
#define MAX_OUTPUT 16000
#define LINE_HEIGHT 20
#define PROMPT "user@terminator> "
#define HISTORY_FILE ".terminator_history"
#define HISTORY_SIZE 1000
#define MAX_TABS 8
#define TAB_BAR_HEIGHT 58

#define RUNTIME_ASYNC_STATUS (-998)
#define RUNTIME_EXIT_STATUS  (-999)

static Display *d;
static Window w;
static GC g;
static XFontStruct *font_info;

static int window_width = WINDOW_WIDTH;
static int window_height = WINDOW_HEIGHT;

typedef struct
{
    char input[GUI_MAX_INPUT];
    int input_len;

    char output[MAX_OUTPUT];
    int exit_status;

    int history_position;

    int search_mode;
    char search_query[GUI_MAX_INPUT];
    char search_saved_input[GUI_MAX_INPUT];
    int search_index;

    char cwd[PATH_MAX];

    int command_running;
    int multiwatch_running;

    TabRuntime runtime;

} Tab;

static Tab tabs[MAX_TABS];

static int tab_count = 1;
static int active_tab = 0;

static TerminatorMode active_mode = TERMINATOR_THREAD_MODE;
static int gui_exit_requested = 0;


/* =========================================================
   SHARED HISTORY
   ========================================================= */

typedef struct
{
    sem_t lock;
    int count;
    char entries[HISTORY_SIZE][GUI_MAX_INPUT];

} SharedHistory;

static SharedHistory *shared_history = NULL;

static char (*history)[GUI_MAX_INPUT] = NULL;

#define history_count (shared_history->count)


static int initialize_shared_history(void)
{
    shared_history = mmap(
        NULL,
        sizeof(*shared_history),
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS,
        -1,
        0
    );

    if (shared_history == MAP_FAILED)
    {
        shared_history = NULL;
        return -1;
    }

    memset(shared_history, 0, sizeof(*shared_history));

    if (sem_init(&shared_history->lock, 1, 1) != 0)
    {
        munmap(shared_history, sizeof(*shared_history));
        shared_history = NULL;
        return -1;
    }

    history = shared_history->entries;

    return 0;
}


static void destroy_shared_history(void)
{
    if (!shared_history)
        return;

    sem_destroy(&shared_history->lock);

    munmap(shared_history, sizeof(*shared_history));

    shared_history = NULL;
    history = NULL;
}


/* =========================================================
   TAB HELPERS
   ========================================================= */

static Tab *current_tab(void)
{
    return &tabs[active_tab];
}


static void append_output_to(Tab *t, const char *text)
{
    size_t used;
    size_t available;
    size_t length;

    if (!t || !text)
        return;

    used = strlen(t->output);
    length = strlen(text);

    available = sizeof(t->output) - used - 1;

    if (length > available)
    {
        size_t keep = sizeof(t->output) / 2;
        size_t old_length = strlen(t->output);

        if (old_length > keep)
        {
            memmove(
                t->output,
                t->output + old_length - keep,
                keep
            );

            t->output[keep] = '\0';
        }

        used = strlen(t->output);

        available = sizeof(t->output) - used - 1;

        length = strlen(text);

        if (length > available)
        {
            text += length - available;
            length = available;
        }
    }

    memcpy(t->output + used, text, length);

    t->output[used + length] = '\0';
}


static void append_output(const char *text)
{
    append_output_to(current_tab(), text);
}


/* =========================================================
   HISTORY
   ========================================================= */

static void save_history(const char *command)
{
    char path[PATH_MAX];

    const char *home = getenv("HOME");

    FILE *fp = NULL;

    if (!command || !command[0] || !shared_history)
        return;

    sem_wait(&shared_history->lock);

    if (history_count > 0 &&
        strcmp(history[history_count - 1], command) == 0)
    {
        current_tab()->history_position = history_count;

        sem_post(&shared_history->lock);

        return;
    }

    if (history_count == HISTORY_SIZE)
    {
        memmove(
            history,
            history + 1,
            sizeof(history[0]) * (HISTORY_SIZE - 1)
        );

        history_count--;
    }

    snprintf(
        history[history_count],
        sizeof(history[history_count]),
        "%s",
        command
    );

    history_count++;

    current_tab()->history_position = history_count;

    if (home &&
        snprintf(
            path,
            sizeof(path),
            "%s/%s",
            home,
            HISTORY_FILE
        ) < (int)sizeof(path))
    {
        fp = fopen(path, "a");

        if (fp)
        {
            fprintf(fp, "%s\n", command);
            fclose(fp);
        }
    }

    sem_post(&shared_history->lock);
}


static void load_history(void)
{
    char path[PATH_MAX];
    char line[GUI_MAX_INPUT];

    const char *home = getenv("HOME");

    FILE *fp;

    int a;

    if (!home || !shared_history)
        return;

    if (snprintf(
            path,
            sizeof(path),
            "%s/%s",
            home,
            HISTORY_FILE) >= (int)sizeof(path))
    {
        return;
    }

    fp = fopen(path, "r");

    if (!fp)
        return;

    sem_wait(&shared_history->lock);

    while (fgets(line, sizeof(line), fp))
    {
        size_t length = strlen(line);

        if (length && line[length - 1] == '\n')
            line[--length] = '\0';

        if (!length)
            continue;

        if (history_count == HISTORY_SIZE)
        {
            memmove(
                history,
                history + 1,
                sizeof(history[0]) * (HISTORY_SIZE - 1)
            );

            history_count--;
        }

        snprintf(
            history[history_count],
            sizeof(history[history_count]),
            "%s",
            line
        );

        history_count++;
    }

    for (a = 0; a < MAX_TABS; a++)
        tabs[a].history_position = history_count;

    sem_post(&shared_history->lock);

    fclose(fp);
}


static int find_history_match(const char *query, int start)
{
    int a;

    if (!query || !query[0])
        return -1;

    if (start >= history_count)
        start = history_count - 1;

    for (a = start; a >= 0; a--)
    {
        if (strstr(history[a], query))
            return a;
    }

    return -1;
}


/* =========================================================
   TEXT DRAWING
   ========================================================= */

static int measure_text(const char *text, int length)
{
    if (length <= 0)
        return 0;

    if (font_info)
        return XTextWidth(font_info, text, length);

    return length * 8;
}


static int wrapped_chunk_length(
    const char *text,
    int length,
    int max_width)
{
    int a;
    int fit = 0;

    for (a = 1; a <= length; a++)
    {
        if (measure_text(text, a) > max_width)
            break;

        fit = a;
    }

    return fit > 0 ? fit : 1;
}


static void draw_tab_bar(void)
{
    int a;
    int x = 10;

    char label[64];

    XDrawString(
        d,
        w,
        g,
        10,
        17,
        "Ctrl+T: New | Ctrl+Tab: Next | Ctrl+W: Close",
        46
    );

    for (a = 0; a < tab_count; a++)
    {
        snprintf(
            label,
            sizeof(label),
            "%sTab %d",
            a == active_tab ? "[*] " : "[ ] ",
            a + 1
        );

        XDrawString(
            d,
            w,
            g,
            x,
            42,
            label,
            (int)strlen(label)
        );

        x += (int)strlen(label) * 8 + 12;
    }

    XDrawLine(
        d,
        w,
        g,
        0,
        52,
        window_width,
        52
    );
}



static int draw_output(int max_lines, int start_y)
{
    Tab *t = current_tab();

    /* Keep the positions of visually wrapped output lines. */
    int line_start[MAX_OUTPUT];
    int line_length[MAX_OUTPUT];
    int total_lines = 0;
    int a = 0;
    int max_width = window_width - 20;
    int length = (int)strlen(t->output);

    if (max_lines < 0)
        max_lines = 0;

    while (a < length && total_lines < MAX_OUTPUT)
    {
        int b = a;

        while (b < length &&
               t->output[b] != '\n' &&
               t->output[b] != '\r')
            b++;

        int remaining = b - a;

        if (remaining == 0)
        {
            line_start[total_lines] = a;
            line_length[total_lines] = 0;
            total_lines++;
        }
        else
        {
            while (remaining > 0 && total_lines < MAX_OUTPUT)
            {
                int chunk = wrapped_chunk_length(
                    t->output + a,
                    remaining,
                    max_width
                );

                line_start[total_lines] = a;
                line_length[total_lines] = chunk;
                total_lines++;

                a += chunk;
                remaining -= chunk;
            }
        }

        a = b;

        if (a < length && t->output[a] == '\r')
            a++;
        if (a < length && t->output[a] == '\n')
            a++;
    }

    /* If output is longer than the window, show its latest lines. */
    int first_line = total_lines - max_lines;
    if (first_line < 0)
        first_line = 0;

    int y = start_y;

    for (a = first_line; a < total_lines; a++)
    {
        if (line_length[a] > 0)
        {
            XDrawString(
                d, w, g,
                10, y,
                t->output + line_start[a],
                line_length[a]
            );
        }

        y += LINE_HEIGHT;
    }

    /* Return the next free baseline, so the prompt follows output. */
    return y;
}


static void redraw(void)
{
    Tab *t = current_tab();
    char prompt[GUI_MAX_INPUT + 128];
    int length;
    int offset = 0;
    int max_width = window_width - 20;
    int output_start_y = TAB_BAR_HEIGHT + LINE_HEIGHT;

    XClearWindow(d, w);
    draw_tab_bar();

    /* Build the editable prompt/search line. */
    if (t->search_mode)
    {
        const char *match = (t->search_index >= 0)
            ? history[t->search_index]
            : "(no match)";

        snprintf(
            prompt, sizeof(prompt),
            "(reverse-i-search)`%.100s': %.1000s",
            t->search_query, match
        );
    }
    else
    {
        snprintf(prompt, sizeof(prompt), "%s%s", PROMPT, t->input);
    }

    /*
     * Normal terminal flow: output begins below the tab bar and
     * the prompt is drawn immediately after the visible output.
     * While a command runs, reserve no prompt row at all.
     */
    int available_lines = (window_height - 10 - output_start_y) / LINE_HEIGHT;
    if (available_lines < 0)
        available_lines = 0;

    int prompt_lines = 0;
    if (!t->command_running)
    {
        length = (int)strlen(prompt);
        int remaining = length;

        while (remaining > 0)
        {
            int chunk = wrapped_chunk_length(
                prompt + offset, remaining, max_width
            );
            offset += chunk;
            remaining -= chunk;
            prompt_lines++;
        }

        if (prompt_lines == 0)
            prompt_lines = 1;
    }

    int output_lines = available_lines - prompt_lines;
    if (t->command_running)
        output_lines = available_lines;
    if (output_lines < 0)
        output_lines = 0;

    int prompt_y = draw_output(output_lines, output_start_y);

    /* Do not show an empty/editable prompt while a command is running. */
    if (!t->command_running)
    {
        offset = 0;
        int remaining = (int)strlen(prompt);

        while (remaining > 0)
        {
            int chunk = wrapped_chunk_length(
                prompt + offset, remaining, max_width
            );

            XDrawString(
                d, w, g,
                10, prompt_y,
                prompt + offset,
                chunk
            );

            offset += chunk;
            remaining -= chunk;
            prompt_y += LINE_HEIGHT;
        }
    }

    XFlush(d);
}


/* =========================================================
   WORKER COMMUNICATION
   ========================================================= */

/*
 * Send every command through the worker.
 *
 * Do not call shell functions directly from the GUI thread.
 */
static void start_foreground_worker(Tab *t, const char *command)
{
    if (t->command_running)
    {
        append_output_to(
            t,
            "A command is already running in this tab.\n"
        );

        return;
    }

    if (!t->runtime.active)
    {
        append_output_to(
            t,
            "Tab shell worker is not available.\n"
        );

        return;
    }

    if (tab_runtime_submit(&t->runtime, command) != 0)
    {
        append_output_to(
            t,
            "Could not send command to this tab's shell worker.\n"
        );

        return;
    }

    t->command_running = 1;

    /*
     * MultiWatch is also started by the worker.
     * Remember this in the GUI only to route Ctrl+C.
     */
    t->multiwatch_running =
        strncmp(command, "multiWatch", 10) == 0 &&
        (command[10] == '\0' ||
         command[10] == ' ' ||
         command[10] == '\t' ||
         command[10] == '[');
}


/*
 * Poll all tab workers, not only tabs with a foreground command.
 *
 * This is necessary because background jobs and MultiWatch can
 * continue producing output after their original command returns.
 */
static void poll_foreground_workers(void)
{
    int a;
    int changed = 0;

    char output[MAX_OUTPUT];

    for (a = 0; a < tab_count; a++)
    {
        Tab *t = &tabs[a];

        int status = 0;
        int result;

        if (!t->runtime.active)
            continue;

        /*
         * Drain available messages. A worker may have more than
         * one asynchronous message waiting.
         */
        for (;;)
        {
            result = tab_runtime_poll(
                &t->runtime,
                output,
                sizeof(output),
                &status
            );

            if (result == 0)
                break;

            if (result < 0)
            {
                if (t->command_running)
                {
                    append_output_to(
                        t,
                        "Shell worker disconnected.\n"
                    );
                }

                t->command_running = 0;
                t->multiwatch_running = 0;

                changed = 1;

                break;
            }

            if (output[0])
            {
                append_output_to(t, output);
                changed = 1;
            }

            if (status == RUNTIME_ASYNC_STATUS)
            {
                /*
                 * Background-job or MultiWatch output.
                 * The original command has already returned.
                 */
                continue;
            }

            /*
             * This is a normal command response.
             */
            if (status == RUNTIME_EXIT_STATUS)
            {
                gui_exit_requested = 1;
            }
            else
            {
                t->exit_status = status;
            }

            t->command_running = 0;

            /*
             * MultiWatch continues after its start command returns.
             * Ctrl+C is still routed to its worker.
             */
            if (t->multiwatch_running &&
                status != RUNTIME_EXIT_STATUS)
            {
                /* Keep this flag until Ctrl+C is pressed. */
            }

            changed = 1;
        }
    }

    if (changed)
        redraw();
}


/* =========================================================
   COMMAND EXECUTION
   ========================================================= */

static void execute_current_command(void)
{
    Tab *t = current_tab();

    char command[GUI_MAX_INPUT];

    char line[GUI_MAX_INPUT + 64];

    if (t->input_len > 0)
    {
        snprintf(
            command,
            sizeof(command),
            "%s",
            t->input
        );

        save_history(command);

        snprintf(
            line,
            sizeof(line),
            "%s%s\n",
            PROMPT,
            command
        );

        append_output(line);

        /*
         * All commands, including multiWatch, fg, bg and jobs,
         * go through the tab's worker.
         */
        start_foreground_worker(t, command);
    }

    t->input[0] = '\0';
    t->input_len = 0;

    t->history_position = history_count;
}


/* =========================================================
   TAB MANAGEMENT
   ========================================================= */

static void open_tab(void)
{
    int new_index = tab_count;

    if (tab_count >= MAX_TABS)
    {
        append_output("Maximum number of tabs reached.\n");
        return;
    }

    memset(&tabs[new_index], 0, sizeof(tabs[new_index]));

    if (tab_runtime_start(
            &tabs[new_index].runtime,
            active_mode) != 0)
    {
        append_output("Could not start shell worker for new tab.\n");
        return;
    }

    tabs[new_index].history_position = history_count;
    tabs[new_index].search_index = -1;

    if (getcwd(
            tabs[new_index].cwd,
            sizeof(tabs[new_index].cwd)) == NULL)
    {
        tabs[new_index].cwd[0] = '\0';
    }

    snprintf(
        tabs[new_index].output,
        sizeof(tabs[new_index].output),
        "Welcome to Terminator - Tab %d\n",
        new_index + 1
    );

    tab_count++;

    active_tab = new_index;
}


static void close_active_tab(void)
{
    int a;

    if (tab_count == 1)
    {
        snprintf(
            current_tab()->output,
            sizeof(current_tab()->output),
            "This is the last tab. Close the window to exit.\n"
        );

        return;
    }

    tab_runtime_stop(&current_tab()->runtime);

    for (a = active_tab; a < tab_count - 1; a++)
        tabs[a] = tabs[a + 1];

    memset(
        &tabs[tab_count - 1],
        0,
        sizeof(tabs[tab_count - 1])
    );

    tab_count--;

    if (active_tab >= tab_count)
        active_tab = tab_count - 1;
}


static void next_tab(void)
{
    if (tab_count > 1)
        active_tab = (active_tab + 1) % tab_count;
}


/* =========================================================
   KEYBOARD SIGNALS
   ========================================================= */

static void handle_ctrl_c(Tab *t)
{
    if (t->command_running || t->multiwatch_running)
    {
        tab_runtime_signal(&t->runtime, SIGINT);

        append_output_to(t, "^C\n");

        /*
         * The worker will stop MultiWatch or interrupt the
         * foreground process group. The GUI does not call
         * shell_multiwatch_stop() itself.
         */
        t->multiwatch_running = 0;
    }
}


static void handle_ctrl_z(Tab *t)
{
    if (t->command_running)
    {
        tab_runtime_signal(&t->runtime, SIGTSTP);

        append_output_to(t, "^Z\n");
    }
}


/* =========================================================
   MAIN GUI
   ========================================================= */

void run_gui(TerminatorMode mode)
{
    XEvent e;

    KeySym k;

    char key[32];

    int n;
    int s;

    int a;

    memset(tabs, 0, sizeof(tabs));

    active_mode = mode;

    gui_exit_requested = 0;

    tab_count = 1;
    active_tab = 0;

    for (a = 0; a < MAX_TABS; a++)
        tabs[a].runtime.fd = -1;

    if (getcwd(tabs[0].cwd, sizeof(tabs[0].cwd)) == NULL)
        tabs[0].cwd[0] = '\0';

    d = XOpenDisplay(NULL);

    if (!d)
    {
        fprintf(stderr, "terminator: cannot open X display\n");
        return;
    }

    if (initialize_shared_history() != 0)
    {
        fprintf(stderr, "terminator: cannot initialize shared history\n");
        XCloseDisplay(d);
        return;
    }

    if (tab_runtime_start(&tabs[0].runtime, active_mode) != 0)
    {
        destroy_shared_history();

        fprintf(stderr, "terminator: cannot start tab shell worker\n");

        XCloseDisplay(d);

        return;
    }

    s = DefaultScreen(d);

    w = XCreateSimpleWindow(
        d,
        RootWindow(d, s),
        100,
        100,
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        1,
        BlackPixel(d, s),
        BlackPixel(d, s)
    );

    XStoreName(
        d,
        w,
        mode == TERMINATOR_PROCESS_MODE
            ? "Terminator - Process Mode"
            : "Terminator - Thread Mode"
    );

    XSelectInput(
        d,
        w,
        ExposureMask |
        KeyPressMask |
        StructureNotifyMask
    );

    g = XCreateGC(d, w, 0, NULL);

    font_info = XQueryFont(d, XGContextFromGC(g));

    XSetForeground(
        d,
        g,
        WhitePixel(d, s)
    );

    XMapWindow(d, w);

    snprintf(
        tabs[0].output,
        sizeof(tabs[0].output),
        "Welcome to Terminator!\n"
        "Type a Linux command and press Enter.\n"
    );

    load_history();

    while (!gui_exit_requested)
    {
        Tab *t;

        /*
         * Wait briefly for X11 events.
         * The timeout also lets us poll worker sockets.
         */
        if (XPending(d) == 0)
        {
            fd_set read_fds;

            struct timeval timeout;

            int x_fd = ConnectionNumber(d);

            FD_ZERO(&read_fds);
            FD_SET(x_fd, &read_fds);

            timeout.tv_sec = 0;
            timeout.tv_usec = 50000;

            (void)select(
                x_fd + 1,
                &read_fds,
                NULL,
                NULL,
                &timeout
            );
        }

        /*
         * Poll every worker.
         * Do not call shell_poll_jobs() or MultiWatch functions
         * directly from the GUI thread.
         */
        poll_foreground_workers();

        if (gui_exit_requested)
            break;

        if (XPending(d) == 0)
            continue;

        XNextEvent(d, &e);

        if (e.type == Expose)
        {
            redraw();
        }
        else if (e.type == ConfigureNotify)
        {
            window_width = e.xconfigure.width;
            window_height = e.xconfigure.height;

            redraw();
        }
        else if (e.type == KeyPress)
        {
            n = XLookupString(
                &e.xkey,
                key,
                sizeof(key),
                &k,
                NULL
            );

            t = current_tab();

            /* Ctrl+C and Ctrl+P: interrupt the current command. */
            if ((e.xkey.state & ControlMask) &&
                (k == XK_c || k == XK_C ||
                 k == XK_p || k == XK_P))
            {
                handle_ctrl_c(t);

                redraw();

                continue;
            }

            /* Ctrl+Z */
            if ((e.xkey.state & ControlMask) &&
                (k == XK_z || k == XK_Z))
            {
                handle_ctrl_z(t);

                redraw();

                continue;
            }

            /* Ctrl+T: new tab */
            if ((e.xkey.state & ControlMask) &&
                (k == XK_t || k == XK_T))
            {
                open_tab();

                redraw();

                continue;
            }

            /* Ctrl+Tab: next tab */
            if ((e.xkey.state & ControlMask) &&
                k == XK_Tab)
            {
                next_tab();

                redraw();

                continue;
            }

            /* Ctrl+W: close tab */
            if ((e.xkey.state & ControlMask) &&
                (k == XK_w || k == XK_W))
            {
                close_active_tab();

                redraw();

                continue;
            }

            /*
             * A normal terminal does not accept another command while
             * the foreground command is running. Signal and tab shortcuts
             * above remain available; ignore other keys until it finishes.
             */
            if (t->command_running)
                continue;

            /* Ctrl+R: reverse history search */
            if ((e.xkey.state & ControlMask) &&
                (k == XK_r || k == XK_R))
            {
                if (!t->search_mode)
                {
                    snprintf(
                        t->search_saved_input,
                        sizeof(t->search_saved_input),
                        "%s",
                        t->input
                    );

                    t->search_query[0] = '\0';
                    t->search_index = -1;
                    t->search_mode = 1;
                }
                else if (t->search_index > 0)
                {
                    t->search_index = find_history_match(
                        t->search_query,
                        t->search_index - 1
                    );
                }
                else if (t->search_index == -1)
                {
                    t->search_index = find_history_match(
                        t->search_query,
                        history_count - 1
                    );
                }

                redraw();

                continue;
            }

            /* Reverse history search mode */
            if (t->search_mode)
            {
                if (k == XK_Escape)
                {
                    t->search_mode = 0;

                    snprintf(
                        t->input,
                        sizeof(t->input),
                        "%s",
                        t->search_saved_input
                    );

                    t->input_len = (int)strlen(t->input);
                }
                else if (k == XK_BackSpace || k == XK_Delete)
                {
                    size_t len = strlen(t->search_query);

                    if (len > 0)
                        t->search_query[len - 1] = '\0';

                    t->search_index = find_history_match(
                        t->search_query,
                        history_count - 1
                    );
                }
                else if (k == XK_Return || k == XK_KP_Enter)
                {
                    if (t->search_index >= 0)
                    {
                        snprintf(
                            t->input,
                            sizeof(t->input),
                            "%s",
                            history[t->search_index]
                        );

                        t->input_len = (int)strlen(t->input);
                    }

                    t->search_mode = 0;
                    t->history_position = history_count;
                }
                else if (n > 0 &&
                         key[0] >= 32 &&
                         key[0] != 127 &&
                         strlen(t->search_query) < GUI_MAX_INPUT - 1)
                {
                    size_t len = strlen(t->search_query);

                    t->search_query[len] = key[0];
                    t->search_query[len + 1] = '\0';

                    t->search_index = find_history_match(
                        t->search_query,
                        history_count - 1
                    );
                }

                redraw();

                continue;
            }

            /* Up: older history */
            if (k == XK_Up)
            {
                if (history_count > 0 &&
                    t->history_position > 0)
                {
                    t->history_position--;

                    snprintf(
                        t->input,
                        sizeof(t->input),
                        "%s",
                        history[t->history_position]
                    );

                    t->input_len = (int)strlen(t->input);
                }

                redraw();

                continue;
            }

            /* Down: newer history */
            if (k == XK_Down)
            {
                if (t->history_position < history_count - 1)
                {
                    t->history_position++;

                    snprintf(
                        t->input,
                        sizeof(t->input),
                        "%s",
                        history[t->history_position]
                    );

                    t->input_len = (int)strlen(t->input);
                }
                else
                {
                    t->history_position = history_count;

                    t->input[0] = '\0';
                    t->input_len = 0;
                }

                redraw();

                continue;
            }

            /* Enter */
            if (k == XK_Return || k == XK_KP_Enter)
            {
                execute_current_command();

                redraw();

                continue;
            }

            /* Backspace */
            if (k == XK_BackSpace || k == XK_Delete)
            {
                if (t->input_len > 0)
                    t->input[--t->input_len] = '\0';

                redraw();

                continue;
            }

            /* Normal typed character */
            if (n > 0 &&
                key[0] >= 32 &&
                key[0] != 127 &&
                t->input_len < GUI_MAX_INPUT - 1)
            {
                t->input[t->input_len++] = key[0];

                t->input[t->input_len] = '\0';

                redraw();
            }
        }
    }

    /*
     * Stop every worker.
     */
    for (a = 0; a < tab_count; a++)
        tab_runtime_stop(&tabs[a].runtime);

    destroy_shared_history();

    if (font_info)
    {
        XFreeFontInfo(NULL, font_info, 1);
        font_info = NULL;
    }

    XFreeGC(d, g);

    XDestroyWindow(d, w);

    XCloseDisplay(d);
}