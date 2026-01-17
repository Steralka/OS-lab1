#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

#define MAX_LEN 1024
#define MAX_TOKENS 64

static char *skip_spaces(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static int run(char *cmd) {
    cmd = skip_spaces(cmd);
    if (*cmd == '\0') return 0;

    char *tokens[MAX_TOKENS];
    int i = 0;

    char *tok = strtok(cmd, " \t");
    while (tok && i < MAX_TOKENS - 1) {
        tokens[i++] = tok;
        tok = strtok(NULL, " \t");
    }
    tokens[i] = NULL;

    if (!tokens[0]) return 0;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        execvp(tokens[0], tokens);
        perror("execvp");
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

int main(void) {
    char line[MAX_LEN];

    while (1) {
        printf("shell> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\n")] = '\0';

        if (strcmp(line, "exit") == 0) break;

        char *or_pos = strstr(line, "||");
        if (or_pos) {
            *or_pos = '\0';
            char *cmd1 = line;
            char *cmd2 = or_pos + 2;

            int rc = run(cmd1);
            if (rc != 0) run(cmd2);
        } else {
            run(line);
        }
    }

    return 0;
}
