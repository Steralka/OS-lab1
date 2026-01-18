#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void usage(const char *p) {
    fprintf(stderr,
            "Использование:\n"
            "  %s --count N [--exec /path [args...]]\n"
            "  %s --seconds S [--exec /path [args...]]\n"
            "  %s --count N --cmd true|false\n"
            "\n"
            "Режимы:\n"
            "  --count N     создать N процессов\n"
            "  --seconds S   грузить S секунд (удобно для мониторинга ~60)\n"
            "\n"
            "Нагрузка:\n"
            "  --cmd true    fork+wait (без exec) (очень быстрый)\n"
            "  --exec ...    fork+exec+wait (реалистичнее, но медленнее)\n"
            "\n"
            "Примеры:\n"
            "  %s --seconds 60 --cmd true\n"
            "  %s --seconds 60 --exec /bin/true\n",
            p, p, p, p, p);
}

static int is_option(const char *s) {
    return s && strncmp(s, "--", 2) == 0;
}

static void sleep_us(int us) {
    struct timespec ts;
    ts.tv_sec = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000;
    nanosleep(&ts, NULL);
}


int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    long long count = -1;     
    double seconds = -1.0;   
    int mode_cmd = 0;         
    int cmd_exit = 0;         
    int mode_exec = 0;        
    char **exec_argv = NULL;  

    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }

    // парсинг аргументов
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = atof(argv[++i]);
        } else if (strcmp(argv[i], "--cmd") == 0 && i + 1 < argc) {
            mode_cmd = 1;
            const char *v = argv[++i];
            if (strcmp(v, "true") == 0) cmd_exit = 0;
            else if (strcmp(v, "false") == 0) cmd_exit = 1;
            else {
                fprintf(stderr, "Ошибка: --cmd должен быть true или false\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--exec") == 0 && i + 1 < argc) {
            mode_exec = 1;
            exec_argv = &argv[i + 1];
            break;
        } else {
            fprintf(stderr, "Неизвестный аргумент: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if ((count < 0 && seconds < 0) || (count >= 0 && seconds >= 0)) {
        fprintf(stderr, "Ошибка: укажи либо --count, либо --seconds\n");
        return 2;
    }
    if (!mode_cmd && !mode_exec) {
        mode_cmd = 1;
        cmd_exit = 0;
    }
    if (mode_cmd && mode_exec) {
        fprintf(stderr, "Ошибка: нельзя одновременно --cmd и --exec\n");
        return 2;
    }

    const double t0 = now_sec();
    double t_deadline = (seconds > 0) ? (t0 + seconds) : 0.0;

    long long created = 0;
    long long reaped = 0;
    double child_time_sum = 0.0;

    int backoff_us = 1000;      
    const int backoff_us_max = 200000;

    while (!g_stop) {
        if (count >= 0 && created >= count) break;
        if (seconds > 0 && now_sec() >= t_deadline) break;

        double c_start = now_sec();
        pid_t pid = fork();

        if (pid < 0) {
            if (errno == EAGAIN) {
                sleep_us(backoff_us);
                if (backoff_us < backoff_us_max) backoff_us *= 2;
                continue;
            }
            perror("fork");
            break;
        }

        if (pid == 0) {
            if (mode_cmd) {
                _exit(cmd_exit);
            } else {
                execvp(exec_argv[0], exec_argv);
                fprintf(stderr, "execvp: не удалось запустить %s\n", exec_argv[0]);
                _exit(127);
            }
        }

        created++;
        backoff_us = 1000;

        int st = 0;
        if (waitpid(pid, &st, 0) < 0) {
            perror("waitpid");
            break;
        }
        reaped++;

        double c_end = now_sec();
        child_time_sum += (c_end - c_start);

        if (seconds > 0 && (created % 5000 == 0)) {
            double dt = now_sec() - t0;
            double rate = (dt > 0) ? (reaped / dt) : 0.0;
            fprintf(stderr,
                    "proc-fork: создано=%lld завершено=%lld время=%.3f c скорость=%.2f proc/s\n",
                    created, reaped, dt, rate);
        }
    }

    const double t1 = now_sec();
    double dt = t1 - t0;
    double rate = (dt > 0) ? (reaped / dt) : 0.0;
    double avg_child = (reaped > 0) ? (child_time_sum / reaped) : 0.0;

    printf("proc-fork: создано=%lld завершено=%lld время=%.6f c скорость=%.2f proc/s ср_на_ребёнка=%.6f c\n",
           created, reaped, dt, rate, avg_child);

    return 0;
}
