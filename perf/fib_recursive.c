#include <stdio.h>

static long long fib(long long n) {
    if (n <= 1) return 1;
    return fib(n - 1) + fib(n - 2);
}

long long fibonacci_recursive(int n) {
    long long total = 0;
    for (int i = 1; i < n; i++) {
        total += fib(i);
    }
    return total;
}

int main(void) {
    printf("Calculate Fibonacci numbers recursively\n");
    printf("%lld\n", fibonacci_recursive((1 << 5) + (1 << 2) + 2 + 1));
    return 0;
}
