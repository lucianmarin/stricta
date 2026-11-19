fn fibonacci(k: i64) -> i64 {
    if k <= 1 {
        1
    } else {
        fibonacci(k - 1) + fibonacci(k - 2)
    }
}

fn fibonacci_recursive(n: i64) -> i64 {
    let mut total: i64 = 0;
    for i in 1..n {
        total += fibonacci(i);
    }
    total
}

fn main() {
    println!("Calculate Fibonacci numbers recursively");
    println!("{}", fibonacci_recursive((1 << 5) + (1 << 2) + 2 + 1));
}
