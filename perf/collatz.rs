fn collatz(limit: i64) -> i64 {
    let mut total_steps: i64 = 0;
    for i in 1..limit {
        let mut n: i64 = i;
        let mut steps: i64 = 0;
        while n > 1 {
            if n % 2 == 0 {
                n /= 2;
            } else {
                n = 3 * n + 1;
            }
            steps += 1;
        }
        total_steps += steps;
    }
    total_steps
}

fn main() {
    println!("Collatz sequence");
    let res = collatz(1_000_000);
    println!("{}", res);
}
