fn sieve(n: usize) -> i64 {
    let mut flags = vec![1_i64; n];
    flags[0] = 0;
    flags[1] = 0;
    for i in 2..n {
        if flags[i] == 1 {
            let mut j = i * i;
            while j < n {
                flags[j] = 0;
                j += i;
            }
        }
    }
    flags.iter().sum()
}

fn main() {
    println!("Sieve of Eratosthenes");
    let res = sieve(2_000_000);
    println!("{}", res);
}
