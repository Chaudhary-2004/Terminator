# Terminator

**Terminator** is a terminal emulator and shell project written in C for
Linux. It uses **X11** to create its own graphical terminal window and
provides a tab-based interface for running Linux commands.

The project demonstrates Linux process management, command parsing, job
control, synchronization, and GUI event handling.

## Features

-   X11 graphical terminal interface.
-   Multiple tabs, each with its own shell runtime.
-   Thread mode and process mode.
-   Command execution through the project's shell implementation.
-   Persistent command history and reverse search with `Ctrl+R`.
-   Input/output redirection and pipelines.
-   Background command execution and job management.
-   Foreground command interrupt and suspend shortcuts.
-   Dynamic text wrapping when the window is resized.
-   Custom `multiWatch` functionality.

The exact shell syntax and supported built-in commands are determined by
the parser and shell implementation.

## Project Structure

``` text
Terminator/
├── include/
│   ├── gui.h
│   ├── parser.h
│   ├── shell.h
│   └── tab_runtime.h
├── src/
│   ├── gui.c
│   ├── parser.c
│   ├── shell.c
│   └── tab_runtime.c
├── main.c
├── Makefile
└── README.md
```

  -----------------------------------------------------------------------
  File                                Responsibility
  ----------------------------------- -----------------------------------
  `main.c`                            Program entry point and GUI mode
                                      selection

  `include/gui.h`                     GUI interface and mode declarations

  `src/gui.c`                         X11 window, drawing, keyboard
                                      input, tabs, and event loop

  `include/parser.h`                  Command and parser declarations

  `src/parser.c`                      Command tokenization and parsing

  `include/shell.h`                   Shell function declarations

  `src/shell.c`                       Command execution, built-ins,
                                      pipelines, redirection, and jobs

  `include/tab_runtime.h`             Per-tab shell runtime interface

  `src/tab_runtime.c`                 Starts and communicates with each
                                      tab's shell worker

  `Makefile`                          Build instructions
  -----------------------------------------------------------------------

## Requirements

You need a Linux environment with:

-   GCC
-   GNU Make
-   X11 development headers and libraries
-   POSIX threads support

On Ubuntu or Debian:

``` bash
sudo apt update
sudo apt install build-essential libx11-dev
```

## Build

From the project root:

``` bash
make
```

To remove the generated executable:

``` bash
make clean
```

The Makefile builds the `terminator` executable using GCC, X11, and
pthread support.

## Run

``` bash
./terminator
```

The application opens an X11 window. Type a Linux command and press
**Enter** to execute it.

The project supports thread mode and process mode. Use the
mode-selection syntax implemented in `main.c`; the window title
identifies the active mode.

## Keyboard Shortcuts

  -----------------------------------------------------------------------
  Shortcut                            Action
  ----------------------------------- -----------------------------------
  `Enter`                             Execute the current command

  `Backspace`                         Delete the previous character

  `Up Arrow`                          Browse to an older command

  `Down Arrow`                        Browse to a newer command

  `Ctrl + R`                          Start or continue reverse history
                                      search

  `Escape`                            Cancel reverse history search

  `Ctrl + C`                          Interrupt the active command, or
                                      stop `multiWatch` in its owning tab

  `Ctrl + Z`                          Send a suspend signal to the active
                                      foreground command

  `Ctrl + T`                          Open a new tab

  `Ctrl + Tab`                        Switch to the next tab

  `Ctrl + W`                          Close the active tab
  -----------------------------------------------------------------------

## Command History

Terminator maintains a shared command history across its tabs.

-   Commands are kept in memory while the application is running.
-   History is loaded when the application starts.
-   New commands are appended to `~/.terminator_history`.
-   The Up and Down arrows navigate through saved commands.
-   `Ctrl+R` searches previous commands in reverse order.

## Architecture

### GUI layer

`src/gui.c` manages the X11 display and window, draws the terminal
interface, receives keyboard events, manages tabs, and refreshes each
tab's output.

### Parser layer

`src/parser.c` converts the command line into a structured
representation for the shell. It recognizes command arguments and
supported operators.

### Shell layer

`src/shell.c` executes parsed commands and implements shell behavior
such as pipelines, redirection, built-in commands, and job management.

### Tab runtime layer

`src/tab_runtime.c` manages the shell worker associated with each tab.
The GUI submits commands to a worker and polls it for output and
completion, allowing the graphical interface to remain responsive.

### Thread and process modes

-   **Thread mode:** a tab's shell worker uses the thread-based
    implementation.
-   **Process mode:** a tab's shell worker uses the process-based
    implementation.

Both modes are selected through the program's entry-point configuration.

## Systems Programming Concepts

This project brings together concepts including:

-   X11 window creation, drawing, and event handling
-   `fork()`, `execvp()`, and process waiting
-   Pipes and file-descriptor redirection
-   `open()`, `read()`, `write()`, `dup2()`, and `close()`
-   Signals and foreground/background job control
-   POSIX threads and synchronization
-   Shared memory and process-shared synchronization
-   Asynchronous worker communication
-   Command tokenization and parsing
-   Makefiles, compilation, and linking

## Troubleshooting

### `cannot open X display`

Terminator requires an active X11 display. Run it from a graphical Linux
session with a valid display environment.

### X11 header not found

Install the X11 development package:

``` bash
sudo apt install libx11-dev
```

### Build errors

Try a clean rebuild:

``` bash
make clean
make
```

Check that GCC, Make, and the X11 development libraries are installed.

## Development Notes

-   Build from the repository root so the Makefile can find `include/`
    and `src/`.
-   Each tab has its own runtime and output buffer.
-   Command history is shared across tabs and protected by
    synchronization.
-   The GUI periodically polls shell workers and background jobs instead
    of blocking while waiting for commands.

## License

Add the project's chosen license here before distributing or publishing
the repository.