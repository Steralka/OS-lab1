#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void bubble_sort(int *a, int n) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n - 1; j++)
            if (a[j] > a[j + 1]) {
                int t = a[j]; a[j] = a[j + 1]; a[j + 1] = t;
            }
}

void quick_sort(int *a, int l, int r) {
    if (l >= r) return;
    int x = a[(l + r) / 2];
    int i = l, j = r;
    while (i <= j) {
        while (a[i] < x) i++;
        while (a[j] > x) j--;
        if (i <= j) {
            int t = a[i]; a[i] = a[j]; a[j] = t;
            i++; j--;
        }
    }
    quick_sort(a, l, j);
    quick_sort(a, i, r);
}


int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Использовать: %s <N> <fast|slow> <repeats>\n", argv[0]);
        return 1;
    }

    int n = atoi(argv[1]);
    int repeats = atoi(argv[3]);
    const char *mode = argv[2];

    int *arr = malloc(sizeof(int) * n);
    if (!arr) return 1;

    for (int r = 0; r < repeats; r++) {
        for (int i = 0; i < n; i++) arr[i] = rand();
        if (strcmp(mode, "fast") == 0) quick_sort(arr, 0, n - 1);
        else bubble_sort(arr, n);
    }

    free(arr);
    return 0;
}
