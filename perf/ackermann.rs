fn ackermann(m: i64, n: i64) -> i64 {
    if m == 0 {
        return n + 1;
    }
    if n == 0 {
        return ackermann(m - 1, 1);
    }
    ackermann(m - 1, ackermann(m, n - 1))
}

fn main() {
    println!("Ackermann function");
    let mut total: i64 = 0;
    for _ in 0..400 {
        total += ackermann(3, 6);
    }
    println!("{}", total);
}
