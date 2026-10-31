#include "parser.h"

#include <ctype.h>
#include <string.h>

#define MAX_TOKENS 2048

static int is_operator(char a)
{
    return a == '|' || a == '<' || a == '>' || a == '&';
}

int parse_pipeline(char *storage,
                   Command commands[],
                   int max_commands)
{
    char source[MAX_COMMAND];
    char *tokens[MAX_TOKENS];
    char *read;
    char *write;
    int token_count = 0;
    int command_count = 1;
    int arg_count = 0;
    int a;

    if (storage == NULL || commands == NULL ||
        max_commands < 1 || max_commands > MAX_COMMANDS)
        return -1;

    if (strlen(storage) >= MAX_COMMAND)
        return -1;

    memset(commands, 0, sizeof(Command) * max_commands);
    strcpy(source, storage);

    read = source;
    write = storage;

    /* Phase 1: tokenize words, quotes and operators. */
    while (*read != '\0')
    {
        char quote = '\0';

        while (isspace((unsigned char)*read))
            read++;

        if (*read == '\0')
            break;

        if (token_count >= MAX_TOKENS - 1)
            return -1;

        if (is_operator(*read))
        {
            char op = *read++;

            if (op == '>' && *read == '>')
            {
                read++;
                tokens[token_count++] = ">>";
            }
            else if (op == '|')
                tokens[token_count++] = "|";
            else if (op == '<')
                tokens[token_count++] = "<";
            else if (op == '>')
                tokens[token_count++] = ">";
            else
                tokens[token_count++] = "&";

            continue;
        }

        tokens[token_count++] = write;

        while (*read != '\0')
        {
            char ch = *read++;

            if (quote != '\0')
            {
                if (ch == quote)
                {
                    quote = '\0';
                }
                else if (ch == '\\' && quote == '"' &&
                         *read != '\0')
                {
                    if (*read == '"' || *read == '\\' ||
                        *read == '$' || *read == '`')
                    {
                        *write++ = *read++;
                    }
                    else
                    {
                        *write++ = ch;
                        *write++ = *read++;
                    }
                }
                else
                {
                    *write++ = ch;
                }
            }
            else if (ch == '\'' || ch == '"')
            {
                quote = ch;
            }
            else if (isspace((unsigned char)ch) ||
                     is_operator(ch))
            {
                if (is_operator(ch))
                    read--;

                break;
            }
            else if (ch == '\\' && *read != '\0')
            {
                *write++ = *read++;
            }
            else
            {
                *write++ = ch;
            }
        }

        if (quote != '\0')
            return -1;

        *write++ = '\0';
    }

    tokens[token_count] = NULL;

    if (token_count == 0)
        return 0;

    /* Phase 2: convert tokens into pipeline commands. */
    for (a = 0; a < token_count; a++)
    {
        char *token = tokens[a];
        Command *current = &commands[command_count - 1];

        if (strcmp(token, "&") == 0)
        {
            /* '&' is supported only at the end of a pipeline. */
            if (a != token_count - 1 || arg_count == 0)
                return -1;

            commands[0].background = 1;
            continue;
        }

        if (strcmp(token, "|") == 0)
        {
            if (arg_count == 0 ||
                command_count >= max_commands)
                return -1;

            current->args[arg_count] = NULL;
            command_count++;
            arg_count = 0;
            continue;
        }

        if (strcmp(token, "<") == 0 ||
            strcmp(token, ">") == 0 ||
            strcmp(token, ">>") == 0)
        {
            char **target;

            if (++a >= token_count)
                return -1;

            if (strcmp(tokens[a], "|") == 0 ||
                strcmp(tokens[a], "<") == 0 ||
                strcmp(tokens[a], ">") == 0 ||
                strcmp(tokens[a], ">>") == 0 ||
                strcmp(tokens[a], "&") == 0 ||
                tokens[a][0] == '\0')
                return -1;

            if (strcmp(token, "<") == 0)
            {
                target = &current->input_file;
            }
            else
            {
                target = &current->output_file;
                current->append_output = (strcmp(token, ">>") == 0);
            }

            if (*target != NULL)
                return -1;

            *target = tokens[a];
            continue;
        }

        if (arg_count >= MAX_ARGS - 1)
            return -1;

        current->args[arg_count++] = token;
    }

    if (arg_count == 0)
        return -1;

    commands[command_count - 1].args[arg_count] = NULL;

    /* Every pipeline stage must have a program name. */
    for (a = 0; a < command_count; a++)
    {
        if (commands[a].args[0] == NULL)
            return -1;
    }

    return command_count;
}