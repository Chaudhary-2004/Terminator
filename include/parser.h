#ifndef PARSER_H
#define PARSER_H

#define MAX_ARGS 64
#define MAX_COMMAND 4096
#define MAX_COMMANDS 32

typedef struct {
    char *args[MAX_ARGS];
    char *input_file;
    char *output_file;
    int append_output;
    int background;
} Command;

int parse_pipeline(char *storage,
                   Command commands[],
                   int max_commands);

#endif