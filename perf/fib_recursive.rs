fn fibonacci(k: i64) -> i64 {
    if k <= 1 {
        1
    } else {
        fibonacci(k - 1) + fibonacci(k - 2)
    }
}

fn fibonacci_recursive(n: i64) {
    for i in 1..n {
        fibonacci(i);
    }
}

fn main() {
    println!("Calculate Fibonacci numbers recursively");
    fibonacci_recursive((1 << 5) + (1 << 2) + 2 + 1);
}
