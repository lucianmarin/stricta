#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct {
    int64_t acc;
    int64_t i;
    int64_t steps;
    bool done;
} Task;

static void task_step(Task* t) {
    if (t->done) return;
    if (t->i < t->steps) {
        t->acc = (t->acc * 1103515245LL + 12345LL + t->i) & 2147483647LL;
        t->i++;
    } else {
        t->done = true;
    }
}

int main(void) {
    const int n = 800;
    const int64_t steps = 2000;
    Task* tasks = (Task*)malloc((size_t)n * sizeof(Task));
    for (int i = 0; i < n; i++) {
        tasks[i].acc = i + 1;
        tasks[i].i = 0;
        tasks[i].steps = steps;
        tasks[i].done = false;
    }
    for (;;) {
        bool all_done = true;
        for (int i = 0; i < n; i++) {
            if (!tasks[i].done) {
                task_step(&tasks[i]);
                if (!tasks[i].done) all_done = false;
            }
        }
        if (all_done) break;
    }
    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        total += tasks[i].acc;
    }
    free(tasks);
    printf("%lld\n", (long long)total);
    return 0;
}
