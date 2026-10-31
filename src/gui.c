#include "gui.h"
#include "shell.h"
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
#include <sys/time.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <semaphore.h>
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

    /* Asynchronous foreground command state for this tab. */
    pid_t command_pgid;
    int command_fd;
    int command_running;
    TabRuntime runtime;
} Tab;

static Tab tabs[MAX_TABS];
static int tab_count = 1;
static int active_tab = 0;

static int multiwatch_tab_index = -1;
/* Most recently launched foreground worker; Ctrl+C works after tab switching. */
static TerminatorMode active_mode = TERMINATOR_THREAD_MODE;
static int gui_exit_requested = 0;


/* Command history is shared by all tabs. */
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
    shared_history = mmap(NULL, sizeof(*shared_history),
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
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
    if (shared_history)
    {
        sem_destroy(&shared_history->lock);
        munmap(shared_history, sizeof(*shared_history));
        shared_history = NULL;
        history = NULL;
    }
}

static Tab *current_tab(void)
{
    return &tabs[active_tab];
}

/* ---------- Tab bar ---------- */

static void draw_tab_bar(void)
{
    int a;
    int x = 10;
    char label[64];

    /* Row 1: keyboard shortcuts */
    XDrawString(d, w, g, 10, 17,
                "Ctrl+T: New | Ctrl+Tab: Next | Ctrl+W: Close",
                46);

    /* Row 2: tab names */
    for (a = 0; a < tab_count; a++)
    {
        snprintf(label, sizeof(label), "%sTab %d",
                 a == active_tab ? "[*] " : "[ ] ",
                 a + 1);

        XDrawString(d, w, g, x, 42,
                    label, (int)strlen(label));

        x += (int)strlen(label) * 8 + 12;
    }

    /* Separator below the tab bar */
    XDrawLine(d, w, g, 0, 52,
              window_width, 52);
}

/* ---------- Text wrapping helpers ---------- */

/* Return the pixel width of a piece of text using the GC's current font. */
static int measure_text(const char *text, int length)
{
    if (length <= 0)
        return 0;

    if (font_info)
        return XTextWidth(font_info, text, length);

    /* Fallback for the unlikely case that XQueryFont() failed. */
    return length * 8;
}

/* Find the largest number of characters that fit in one visual line. */
static int wrapped_chunk_length(const char *text, int length, int max_width)
{
    int a;
    int fit = 0;

    for (a = 1; a <= length; a++)
    {
        if (measure_text(text, a) > max_width)
            break;
        fit = a;
    }

    /* Always consume at least one character to avoid an infinite loop. */
    return fit > 0 ? fit : 1;
}

/* Count visual lines in output, including blank lines and wrapped lines. */
static int count_output_lines(const char *text, int max_width)
{
    int a = 0;
    int total = 0;
    int text_length = (int)strlen(text);

    while (a < text_length)
    {
        int b = a;
        int length;

        while (b < text_length && text[b] != '\n' && text[b] != '\r')
            b++;

        length = b - a;
        if (length == 0)
            total++;
        else
        {
            while (length > 0)
            {
                int chunk = wrapped_chunk_length(text + a, length, max_width);
                total++;
                a += chunk;
                length -= chunk;
            }
        }

        a = b;
        if (text[a] == '\r')
            a++;
        if (text[a] == '\n')
            a++;
    }

    /* A trailing newline means the next prompt starts on a fresh line. */
    if (text_length > 0 &&
        (text[text_length - 1] == '\n' || text[text_length - 1] == '\r'))
        total++;

    return total;
}

/* Draw the visible tail of a tab's output and return the next baseline. */
static int draw_output(int skip_lines)
{
    Tab *t = current_tab();
    int a = 0;
    int y = TAB_BAR_HEIGHT + LINE_HEIGHT;
    int max_width = window_width - 20;
    int text_length = (int)strlen(t->output);
    int line_index = 0;

    while (a < text_length && y < window_height - 10)
    {
        int b = a;
        int length;

        while (b < text_length && t->output[b] != '\n' && t->output[b] != '\r')
            b++;

        length = b - a;
        if (length == 0)
        {
            if (line_index >= skip_lines)
                y += LINE_HEIGHT;
            line_index++;
        }
        else
        {
            while (length > 0)
            {
                int chunk = wrapped_chunk_length(t->output + a, length, max_width);

                if (line_index >= skip_lines && y < window_height - 10)
                {
                    XDrawString(d, w, g, 10, y,
                                t->output + a, chunk);
                    y += LINE_HEIGHT;
                }

                line_index++;
                a += chunk;
                length -= chunk;
            }
        }

        a = b;
        if (t->output[a] == '\r')
            a++;
        if (t->output[a] == '\n')
            a++;
    }

    if (text_length > 0 &&
        (t->output[text_length - 1] == '\n' ||
         t->output[text_length - 1] == '\r'))
    {
        if (line_index >= skip_lines)
            y += LINE_HEIGHT;
    }

    return y;
}

/* ---------- History search ---------- */

static int find_history_match(const char *query, int start)
{
    int a;

    if (query == NULL || query[0] == '\0')
        return -1;

    if (start >= history_count)
        start = history_count - 1;

    for (a = start; a >= 0; a--)
    {
        if (strstr(history[a], query) != NULL)
            return a;
    }

    return -1;
}

/* ---------- Redraw ---------- */

static void redraw(void)
{
    Tab *t = current_tab();
    char prompt[GUI_MAX_INPUT + 128];
    int prompt_length;
    int prompt_lines = 0;
    int offset = 0;
    int max_width = window_width - 20;
    int available_lines =
        (window_height - TAB_BAR_HEIGHT - 20) / LINE_HEIGHT;
    int output_lines;
    int skip_lines;
    int y;

    XClearWindow(d, w);
    draw_tab_bar();

    if (t->search_mode)
    {
        const char *match;

        if (t->search_index >= 0)
            match = history[t->search_index];
        else
            match = "(no match)";

        snprintf(prompt, sizeof(prompt),
                 "(reverse-i-search)`%.100s': %.1000s",
                 t->search_query, match);
    }
    else
    {
        snprintf(prompt, sizeof(prompt), "%s%s", PROMPT, t->input);
    }

    prompt_length = (int)strlen(prompt);
    while (offset < prompt_length)
    {
        int chunk = wrapped_chunk_length(prompt + offset,
                                         prompt_length - offset,
                                         max_width);
        offset += chunk;
        prompt_lines++;
    }
    if (prompt_lines == 0)
        prompt_lines = 1;

    output_lines = count_output_lines(t->output, max_width);
    skip_lines = output_lines + prompt_lines > available_lines
                     ? output_lines + prompt_lines - available_lines
                     : 0;

    y = draw_output(skip_lines);
    offset = 0;

    /* Draw the editable prompt immediately after the previous output. */
    while (offset < prompt_length && y < window_height - 5)
    {
        int chunk = wrapped_chunk_length(prompt + offset,
                                         prompt_length - offset,
                                         max_width);
        XDrawString(d, w, g, 10, y, prompt + offset, chunk);
        offset += chunk;
        y += LINE_HEIGHT;
    }

    XFlush(d);
}

/* ---------- Save and load history ---------- */

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
        memmove(history, history + 1,
                sizeof(history[0]) * (HISTORY_SIZE - 1));
        history_count--;
    }

    snprintf(history[history_count], sizeof(history[history_count]),
             "%s", command);
    history_count++;
    current_tab()->history_position = history_count;

    if (home && snprintf(path, sizeof(path), "%s/%s",
                         home, HISTORY_FILE) < (int)sizeof(path))
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
    if (snprintf(path, sizeof(path), "%s/%s",
                 home, HISTORY_FILE) >= (int)sizeof(path))
        return;

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
            memmove(history, history + 1,
                    sizeof(history[0]) * (HISTORY_SIZE - 1));
            history_count--;
        }

        snprintf(history[history_count], sizeof(history[history_count]),
                 "%s", line);
        history_count++;
    }
    for (a = 0; a < MAX_TABS; a++)
        tabs[a].history_position = history_count;
    sem_post(&shared_history->lock);
    fclose(fp);
}

/* ---------- Output buffer ---------- */

static void append_output_to(Tab *t, const char *text)
{
    size_t used, available, length;
    if (!t || !text) return;
    used = strlen(t->output);
    available = sizeof(t->output) - used - 1;
    length = strlen(text);
    if (length > available)
    {
        size_t keep = sizeof(t->output) / 2;
        size_t old_length = strlen(t->output);
        if (old_length > keep)
        {
            memmove(t->output, t->output + old_length - keep, keep);
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

static void start_foreground_worker(Tab *t, const char *command)
{
    if (t->command_running)
    {
        append_output_to(t, "A command is already running in this tab.\n");
        return;
    }
    if (!t->runtime.active)
    {
        append_output_to(t, "Tab shell worker is not available.\n");
        return;
    }
    if (tab_runtime_submit(&t->runtime, command) != 0)
    {
        append_output_to(t, "Could not send command to this tab's shell worker.\n");
        return;
    }
    t->command_running = 1;
    t->command_pgid = t->runtime.pid;
}

static void poll_foreground_workers(void)
{
    int a, changed = 0;
    char output[MAX_OUTPUT];
    for (a = 0; a < tab_count; a++)
    {
        Tab *t = &tabs[a];
        int status = 0;
        int result;
        if (!t->command_running) continue;
        result = tab_runtime_poll(&t->runtime, output, sizeof(output), &status);
        if (result == 1)
        {
            if (output[0]) append_output_to(t, output);
            if (status == -999)
                gui_exit_requested = 1;
            else
                t->exit_status = status;
            t->command_running = 0;
            t->command_pgid = 0;
            changed = 1;
        }
        else if (result < 0)
        {
            append_output_to(t, "Shell worker disconnected.\n");
            t->command_running = 0;
            t->command_pgid = 0;
            changed = 1;
        }
    }
    if (changed) redraw();
}

static void execute_current_command(void)
{
    Tab *t = current_tab();
    char command[GUI_MAX_INPUT];
    char line[GUI_MAX_INPUT + 64];

    if (t->input_len > 0)
    {
        snprintf(command, sizeof(command), "%s", t->input);
        save_history(command);
        snprintf(line, sizeof(line), "%s%s\n", PROMPT, command);
        append_output(line);

        if (strncmp(command, "multiWatch", 10) == 0 &&
            (command[10] == '\0' || command[10] == ' ' ||
             command[10] == '\t' || command[10] == '['))
        {
            if (shell_multiwatch_active())
                append_output_to(t, "multiWatch is already running. Press Ctrl+C to stop it.\n");
            else
            {
                char result[MAX_OUTPUT];
                result[0] = '\0';
                if (shell_multiwatch_start(command, result, sizeof(result)) == 0)
                    multiwatch_tab_index = active_tab;
                if (result[0]) append_output_to(t, result);
            }
        }
        else
            start_foreground_worker(t, command);
    }

    t->input[0] = '\0';
    t->input_len = 0;
    t->history_position = history_count;
}

/* ---------- Tab management ---------- */

static void open_tab(void)
{
    int new_index;

    if (tab_count >= MAX_TABS)
    {
        append_output("Maximum number of tabs reached.\n");
        return;
    }

    new_index = tab_count;

    memset(&tabs[new_index], 0, sizeof(tabs[new_index]));
    tabs[new_index].command_fd = -1;
    if (tab_runtime_start(&tabs[new_index].runtime, active_mode) != 0)
    {
        append_output("Could not start shell worker for new tab.\n");
        return;
    }

    tabs[new_index].history_position = history_count;
    tabs[new_index].search_index = -1;

    if (getcwd(tabs[new_index].cwd,
               sizeof(tabs[new_index].cwd)) == NULL)
    {
        tabs[new_index].cwd[0] = '\0';
    }

    snprintf(tabs[new_index].output,
             sizeof(tabs[new_index].output),
             "Welcome to Terminator - Tab %d\n",
             new_index + 1);

    tab_count++;
    active_tab = new_index;
}

static void close_active_tab(void)
{
    int a;

    if (tab_count == 1)
    {
        snprintf(current_tab()->output,
                 sizeof(current_tab()->output),
                 "This is the last tab. Close the window to exit.\n");
        return;
    }

    tab_runtime_stop(&current_tab()->runtime);

    for (a = active_tab; a < tab_count - 1; a++)
        tabs[a] = tabs[a + 1];

    memset(&tabs[tab_count - 1], 0, sizeof(tabs[tab_count - 1]));

    tab_count--;

    if (active_tab >= tab_count)
        active_tab = tab_count - 1;
}

static void next_tab(void)
{
    if (tab_count > 1)
        active_tab = (active_tab + 1) % tab_count;
}


/* ---------- Main X11 event loop ---------- */

void run_gui(TerminatorMode mode)
{
    XEvent e;
    KeySym k;
    char key[32];
    int n;
    int s;

    memset(tabs, 0, sizeof(tabs));
    {
        int a;
        for (a = 0; a < MAX_TABS; a++)
            tabs[a].command_fd = -1;
    }

    active_mode = mode;
    gui_exit_requested = 0;
    multiwatch_tab_index = -1;

    tab_count = 1;
    active_tab = 0;

    if (getcwd(tabs[0].cwd, sizeof(tabs[0].cwd)) == NULL)
        tabs[0].cwd[0] = '\0';

    d = XOpenDisplay(NULL);

    if (d == NULL)
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
        100, 100,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        1,
        BlackPixel(d, s),
        BlackPixel(d, s));

    XStoreName(d, w,
               mode == TERMINATOR_PROCESS_MODE
                   ? "Terminator - Process Mode"
                   : "Terminator - Thread Mode");

    XSelectInput(d, w,
                 ExposureMask |
                     KeyPressMask |
                     StructureNotifyMask);

    g = XCreateGC(d, w, 0, NULL);
    font_info = XQueryFont(d, XGContextFromGC(g));

    XSetForeground(d, g, WhitePixel(d, s));

    XMapWindow(d, w);

    snprintf(tabs[0].output, sizeof(tabs[0].output),
             "Welcome to Terminator!\n"
             "Type a Linux command and press Enter.\n");

    load_history();

    while (1)
    {
        Tab *t;

        /*
         * Wait for X11 input with a short timeout. This avoids busy-waiting
         * while still allowing background shell jobs to be polled regularly.
         */
        if (XPending(d) == 0)
        {
            fd_set read_fds;
            struct timeval timeout;
            int x_fd = ConnectionNumber(d);

            FD_ZERO(&read_fds);
            FD_SET(x_fd, &read_fds);

            timeout.tv_sec = 0;
            timeout.tv_usec = 100000;

            (void)select(x_fd + 1, &read_fds, NULL, NULL, &timeout);
        }

        /*
         * Collect output or completion notifications from background jobs.
         */
        {
            char job_output[MAX_OUTPUT];

            if (shell_poll_jobs(job_output, sizeof(job_output)))
            {
                append_output(job_output);
                redraw();
            }
        }

        poll_foreground_workers();

        /* Route multiWatch output to the tab that launched it. */
        {
            char watch_output[MAX_OUTPUT];

            if (shell_multiwatch_active() &&
                shell_multiwatch_poll(watch_output, sizeof(watch_output)))
            {
                if (multiwatch_tab_index >= 0 &&
                    multiwatch_tab_index < tab_count)
                {
                    append_output_to(
                        &tabs[multiwatch_tab_index],
                        watch_output);

                    redraw();
                }
            }
        }

        if (XPending(d) == 0)
            continue;

        XNextEvent(d, &e);

        if (e.type == Expose)
        {
            redraw();
        }
        else if (e.type == ConfigureNotify)
        {
            /* Keep wrapping dimensions in sync when the window is resized. */
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
                NULL);

            t = current_tab();

            /* Ctrl+C interrupts the active tab's shell worker. */
            if ((e.xkey.state & ControlMask) &&
                (k == XK_c || k == XK_C))
            {
                if (shell_multiwatch_active() &&
                    multiwatch_tab_index == active_tab)
                {
                    shell_multiwatch_stop();
                    append_output_to(t, "^C\n");
                    multiwatch_tab_index = -1;
                }
                else if (t->command_running)
                {
                    tab_runtime_signal(&t->runtime, SIGINT);
                    append_output_to(t, "^C\n");
                }

                redraw();
                continue;
            }

            /* Ctrl+Z suspends a command in this tab's shell worker. */
            if ((e.xkey.state & ControlMask) &&
                (k == XK_z || k == XK_Z))
            {
                if (t->command_running)
                    tab_runtime_signal(&t->runtime, SIGTSTP);

                append_output_to(t, "^Z\n");
                redraw();
                continue;
            }

            /* Ctrl+T: open a tab. */
            if ((e.xkey.state & ControlMask) &&
                (k == XK_t || k == XK_T))
            {
                open_tab();
                redraw();
            }

            /* Ctrl+Tab: switch tabs. */
            else if ((e.xkey.state & ControlMask) &&
                     k == XK_Tab)
            {
                next_tab();
                redraw();
            }

            /* Ctrl+W: close the active tab. */
            else if ((e.xkey.state & ControlMask) &&
                     (k == XK_w || k == XK_W))
            {
                close_active_tab();
                redraw();
            }

            /* Ctrl+R: reverse history search. */
            else if ((e.xkey.state & ControlMask) &&
                     (k == XK_r || k == XK_R))
            {
                if (!t->search_mode)
                {
                    snprintf(
                        t->search_saved_input,
                        sizeof(t->search_saved_input),
                        "%s",
                        t->input);

                    t->search_query[0] = '\0';
                    t->search_index = -1;
                    t->search_mode = 1;
                }
                else if (t->search_index > 0)
                {
                    t->search_index =
                        find_history_match(
                            t->search_query,
                            t->search_index - 1);
                }
                else if (t->search_index == -1)
                {
                    t->search_index =
                        find_history_match(
                            t->search_query,
                            history_count - 1);
                }

                redraw();
            }

            else if (t->search_mode)
            {
                if (k == XK_Escape)
                {
                    t->search_mode = 0;

                    snprintf(
                        t->input,
                        sizeof(t->input),
                        "%s",
                        t->search_saved_input);

                    t->input_len = (int)strlen(t->input);
                }
                else if (k == XK_BackSpace || k == XK_Delete)
                {
                    size_t len = strlen(t->search_query);

                    if (len > 0)
                        t->search_query[len - 1] = '\0';

                    t->search_index =
                        find_history_match(
                            t->search_query,
                            history_count - 1);
                }
                else if (k == XK_Return || k == XK_KP_Enter)
                {
                    if (t->search_index >= 0)
                    {
                        snprintf(
                            t->input,
                            sizeof(t->input),
                            "%s",
                            history[t->search_index]);

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

                    t->search_index =
                        find_history_match(
                            t->search_query,
                            history_count - 1);
                }

                redraw();
            }

            /* Up arrow: older command. */
            else if (k == XK_Up)
            {
                if (history_count > 0 &&
                    t->history_position > 0)
                {
                    t->history_position--;

                    snprintf(
                        t->input,
                        sizeof(t->input),
                        "%s",
                        history[t->history_position]);

                    t->input_len = (int)strlen(t->input);
                }

                redraw();
            }

            /* Down arrow: newer command. */
            else if (k == XK_Down)
            {
                if (t->history_position < history_count - 1)
                {
                    t->history_position++;

                    snprintf(
                        t->input,
                        sizeof(t->input),
                        "%s",
                        history[t->history_position]);

                    t->input_len = (int)strlen(t->input);
                }
                else
                {
                    t->history_position = history_count;
                    t->input[0] = '\0';
                    t->input_len = 0;
                }

                redraw();
            }

            /* Enter: execute command. */
            else if (k == XK_Return || k == XK_KP_Enter)
            {
                execute_current_command();
                redraw();

                if (gui_exit_requested)
                    break;
            }

            /* Backspace: delete one character. */
            else if (k == XK_BackSpace || k == XK_Delete)
            {
                if (t->input_len > 0)
                    t->input[--t->input_len] = '\0';

                redraw();
            }

            /* Normal typed character. */
            else if (n > 0 &&
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

    if (shell_multiwatch_active())
        shell_multiwatch_stop();

    {
        int a;

        for (a = 0; a < tab_count; a++)
            tab_runtime_stop(&tabs[a].runtime);
    }

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