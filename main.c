#include "gui.h"

#include <stdio.h>
#include <string.h>

static int parse_mode(int argc, char **argv, TerminatorMode *mode)
{
    int i;

    *mode = TERMINATOR_THREAD_MODE;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "useProcess=True") == 0)
        {
            *mode = TERMINATOR_PROCESS_MODE;
        }
        else
        {
            fprintf(stderr,
                    "Usage: %s [useProcess=True]\n"
                    "  no argument       : thread mode (default)\n"
                    "  useProcess=True   : process mode\n",
                    argv[0]);
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    TerminatorMode mode;

    if (parse_mode(argc, argv, &mode) != 0)
        return 2;

    run_gui(mode);
    return 0;
}