#define _POSIX_C_SOURCE 200809L
#include "vtsh.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINE 1024
#define MAX_ARGS 64

#define MAX_ALIASES 64
#define MAX_ALIAS_NAME 64
#define MAX_ALIAS_VALUE 256
#define MAX_ALIAS_EXPAND_DEPTH 16

typedef struct {
    char name[MAX_ALIAS_NAME];
    char value[MAX_ALIAS_VALUE];
} Alias;

static Alias aliases[MAX_ALIASES];
static int alias_count = 0;

static double diff_sec(struct timespec a, struct timespec b) {
    long sec = b.tv_sec - a.tv_sec;
    long nsec = b.tv_nsec - a.tv_nsec;
    if (nsec < 0) {
        sec--;
        nsec += 1000000000L;
    }
    return (double)sec + (double)nsec / 1e9;
}

static void alias_print_all(void) {
    // Формат, который можно вставить обратно в shell
    for (int i = 0; i < alias_count; i++) {
        printf("alias %s='%s'\n", aliases[i].name, aliases[i].value);
    }
}

static void alias_set(const char *name, const char *value) {
    if (!name || !*name) {
        printf("alias: пустое имя\n");
        return;
    }
    if (!value) value = "";

    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            // гарантируем нуль-терминатор
            snprintf(aliases[i].value, MAX_ALIAS_VALUE, "%s", value);
            return;
        }
    }

    if (alias_count >= MAX_ALIASES) {
        printf("alias: список алиасов переполнен\n");
        return;
    }

    snprintf(aliases[alias_count].name, MAX_ALIAS_NAME, "%s", name);
    snprintf(aliases[alias_count].value, MAX_ALIAS_VALUE, "%s", value);
    alias_count++;
}

static void alias_remove(const char *name) {
    if (!name || !*name) {
        printf("unalias: нужно имя\n");
        return;
    }

    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            aliases[i] = aliases[alias_count - 1];
            alias_count--;
            return;
        }
    }
    printf("unalias: нет такого алиаса: %s\n", name);
}

static const char *alias_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, name) == 0)
            return aliases[i].value;
    }
    return NULL;
}

static void sigchld_handler(int sig) {
    (void)sig;

    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int code = 1;
        if (WIFEXITED(status)) {
            code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            code = 128 + WTERMSIG(status);
        }
        fprintf(stderr, "[background pid %d finished, exit=%d]\n", (int)pid, code);
        fflush(stderr);
    }
}

static int vtsh_run(char **argv) {
    if (!argv || !argv[0]) return 0;

    // поддержка "&" только отдельным токеном (упрощение)
    int background = 0;
    for (int i = 0; argv[i]; i++) {
        if (strcmp(argv[i], "&") == 0) {
            background = 1;
            argv[i] = NULL;
            break;
        }
    }

    struct timespec start, end;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        perror("clock_gettime(start)");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        execvp(argv[0], argv);
        printf("Command not found\n");
        fflush(stdout);
        _exit(127);
    }

    if (background) {
        fprintf(stderr, "[started background pid %d]\n", (int)pid);
        return 0;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        perror("clock_gettime(end)");
        return 1;
    }

    int code = 1;
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        code = 128 + WTERMSIG(status);
    }

    fprintf(stderr, "[exit=%d, time=%.3f s]\n", code, diff_sec(start, end));
    return code;
}

static int vtsh_exec_line_depth(char *line, int depth);

static int vtsh_exec_line(char *line) {
    return vtsh_exec_line_depth(line, 0);
}

static int vtsh_exec_line_depth(char *line, int depth) {
    if (!line) return 0;
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') return 0;

    if (depth > MAX_ALIAS_EXPAND_DEPTH) {
        fprintf(stderr, "alias: слишком глубокая подстановка (возможен цикл)\n");
        return 1;
    }

    char copy[MAX_LINE];
    snprintf(copy, sizeof(copy), "%s", line);

    char *first = strtok(copy, " \t");
    if (!first) return 0;

    const char *as = alias_find(first);
    if (as) {
        char newcmd[MAX_LINE];
        const char *rest = line + strlen(first);
        while (*rest == ' ' || *rest == '\t') rest++;

        if (*rest) {
            snprintf(newcmd, sizeof(newcmd), "%s %s", as, rest);
        } else {
            snprintf(newcmd, sizeof(newcmd), "%s", as);
        }

        return vtsh_exec_line_depth(newcmd, depth + 1);
    }

    char *argv[MAX_ARGS];
    int argc = 0;

    char *token = strtok(line, " \t");
    while (token && argc < MAX_ARGS - 1) {
        argv[argc++] = token;
        token = strtok(NULL, " \t");
    }
    argv[argc] = NULL;

    if (argc == 0) return 0;
    return vtsh_run(argv);
}

static void vtsh_eval(char *line) {
    char *saveptr = NULL;
    char *segment = strtok_r(line, ";", &saveptr);

    while (segment) {
        while (*segment == ' ' || *segment == '\t') segment++;

        if (*segment) {
            char *and_pos = strstr(segment, "&&");
            char *or_pos = strstr(segment, "||");

            if (and_pos && (!or_pos || and_pos < or_pos)) {
                *and_pos = '\0';
                char *next = and_pos + 2;
                int rc = vtsh_exec_line(segment);
                if (rc == 0) vtsh_eval(next);
            } else if (or_pos) {
                *or_pos = '\0';
                char *next = or_pos + 2;
                int rc = vtsh_exec_line(segment);
                if (rc != 0) vtsh_eval(next);
            } else {
                vtsh_exec_line(segment);
            }
        }

        segment = strtok_r(NULL, ";", &saveptr);
    }
}

void vtsh_loop(void) {
    char line[MAX_LINE];
    signal(SIGCHLD, sigchld_handler);
    setvbuf(stdin, NULL, _IONBF, 0);
    
    while (1) {
        printf("vtsh> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin))
            break;

        line[strcspn(line, "\n")] = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') continue;

        if (strcmp(p, "exit") == 0) break;

        // ---------- alias / alias -p / unalias ----------
        if (strncmp(p, "alias", 5) == 0 && (p[5] == '\0' || p[5] == ' ' || p[5] == '\t')) {
            char *rest = p + 5;
            while (*rest == ' ' || *rest == '\t') rest++;

            if (*rest == '\0' || (strcmp(rest, "-p") == 0)) {
                alias_print_all();
                continue;
            }

            char *eq = strchr(rest, '=');
            if (!eq) {
                printf("Использование: alias имя=\"значение\"  (или: alias -p)\n");
                continue;
            }

            *eq = '\0';
            char *name = rest;
            char *value = eq + 1;

            size_t nlen = strlen(name);
            while (nlen > 0 && (name[nlen - 1] == ' ' || name[nlen - 1] == '\t')) {
                name[nlen - 1] = '\0';
                nlen--;
            }

            size_t vlen = strlen(value);
            if (vlen >= 2) {
                if ((value[0] == '"' && value[vlen - 1] == '"') ||
                    (value[0] == '\'' && value[vlen - 1] == '\'')) {
                    value[vlen - 1] = '\0';
                    value++;
                }
            }

            alias_set(name, value);
            continue;
        }

        if (strncmp(p, "unalias", 7) == 0 && (p[7] == '\0' || p[7] == ' ' || p[7] == '\t')) {
            char *name = p + 7;
            while (*name == ' ' || *name == '\t') name++;

            if (*name == '\0') {
                printf("Использование: unalias имя\n");
            } else {
                alias_remove(name);
            }
            continue;
        }

        vtsh_eval(p);
    }
}
