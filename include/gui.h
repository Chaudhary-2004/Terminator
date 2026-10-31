#ifndef GUI_H
#define GUI_H

typedef enum {
    TERMINATOR_THREAD_MODE = 0,
    TERMINATOR_PROCESS_MODE = 1
} TerminatorMode;

void run_gui(TerminatorMode mode);

#endif