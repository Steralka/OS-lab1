#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(const char *prog) {
    fprintf(stderr,
        "Использование:\n"
        "  %s gen <файл> <размер_в_байтах> [seed]\n"
        "  %s replace <файл> <старое_значение> <новое_значение>\n",
        prog, prog);
}

static int parse_long(const char *s, long *out) {
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        return -1;
    *out = v;
    return 0;
}

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Генерация файла */
static int cmd_gen(const char *path, long size_bytes, uint32_t seed) {
    if (size_bytes <= 0) {
        fprintf(stderr, "Ошибка: размер файла должен быть больше 0\n");
        return 2;
    }
    if (size_bytes % (long)sizeof(int32_t) != 0) {
        fprintf(stderr,
            "Ошибка: размер файла должен быть кратен %zu байтам\n",
            sizeof(int32_t));
        return 2;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        perror("Ошибка открытия файла для записи");
        return 1;
    }

    long count = size_bytes / (long)sizeof(int32_t);
    uint32_t rng = seed ? seed : (uint32_t)time(NULL);

    for (long i = 0; i < count; i++) {
        int32_t v = (int32_t)xorshift32(&rng);
        if (fwrite(&v, sizeof(v), 1, f) != 1) {
            perror("Ошибка записи в файл");
            fclose(f);
            return 1;
        }
    }

    fclose(f);

    printf("Файл '%s' создан, размер: %ld байт (%ld чисел int32)\n",
           path, size_bytes, count);
    return 0;
}

/* Поиск и замена */
static int cmd_replace(const char *path, int32_t oldv, int32_t newv) {
    FILE *f = fopen(path, "r+b");
    if (!f) {
        perror("Ошибка открытия файла для чтения/записи");
        return 1;
    }

    int64_t replaced = 0;
    int32_t cur;

    while (fread(&cur, sizeof(cur), 1, f) == 1) {
        if (cur == oldv) {
            if (fseek(f, -(long)sizeof(cur), SEEK_CUR) != 0) {
                perror("Ошибка перемещения в файле");
                fclose(f);
                return 1;
            }
            if (fwrite(&newv, sizeof(newv), 1, f) != 1) {
                perror("Ошибка записи нового значения");
                fclose(f);
                return 1;
            }
            replaced++;
        }
    }

    if (ferror(f)) {
        perror("Ошибка чтения файла");
        fclose(f);
        return 1;
    }

    fclose(f);

    printf("Заменено вхождений: %" PRId64 "\n", replaced);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "gen") == 0) {
        if (argc != 4 && argc != 5) {
            usage(argv[0]);
            return 2;
        }

        const char *file = argv[2];
        long size_bytes = 0;

        if (parse_long(argv[3], &size_bytes) != 0) {
            fprintf(stderr, "Ошибка: неверный размер файла: %s\n", argv[3]);
            return 2;
        }

        uint32_t seed = 0;
        if (argc == 5) {
            long s = 0;
            if (parse_long(argv[4], &s) != 0 || s < 0) {
                fprintf(stderr, "Ошибка: неверный seed: %s\n", argv[4]);
                return 2;
            }
            seed = (uint32_t)s;
        }

        return cmd_gen(file, size_bytes, seed);
    }

    if (strcmp(argv[1], "replace") == 0) {
        if (argc != 5) {
            usage(argv[0]);
            return 2;
        }

        const char *file = argv[2];
        long oldl = 0, newl = 0;

        if (parse_long(argv[3], &oldl) != 0 ||
            oldl < INT32_MIN || oldl > INT32_MAX) {
            fprintf(stderr, "Ошибка: неверное старое значение: %s\n", argv[3]);
            return 2;
        }

        if (parse_long(argv[4], &newl) != 0 ||
            newl < INT32_MIN || newl > INT32_MAX) {
            fprintf(stderr, "Ошибка: неверное новое значение: %s\n", argv[4]);
            return 2;
        }

        return cmd_replace(file, (int32_t)oldl, (int32_t)newl);
    }

    usage(argv[0]);
    return 2;
}