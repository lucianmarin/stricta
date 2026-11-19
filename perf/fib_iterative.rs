fn fibonacci_iterative(n: i64) -> i64 {
    let mut first: i64 = 0;
    let mut second: i64 = 1;
    for _ in 2..n {
        let next = first.wrapping_add(second);
        first = second;
        second = next;
    }
    second
}

fn main() {
    println!("Calculate Fibonacci numbers iteratively");
    println!("{}", fibonacci_iterative((1 << 19) + (1 << 18)) & 0xFFFF_FFFF);
}
