#include <stdio.h>
#include <stdint.h>

uint64_t fibonacci_iterative(int n) {
    uint64_t first = 0, second = 1;
    for (int i = 2; i < n; i++) {
        uint64_t next = first + second;
        first = second;
        second = next;
    }
    return second;
}

int main(void) {
    printf("Calculate Fibonacci numbers iteratively\n");
    printf("%u\n", (unsigned)(fibonacci_iterative((1 << 19) + (1 << 18)) & 0xFFFFFFFFu));
    return 0;
}
