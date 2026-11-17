fn pi_wallis(n: i64) {
    let mut pi: f64 = 2.0;
    for i in 1..n {
        let f = i as f64;
        let left = (2.0 * f) / (2.0 * f - 1.0);
        let right = (2.0 * f) / (2.0 * f + 1.0);
        pi = pi * left * right;
    }
}

fn main() {
    println!("Calculate Pi using Wallis product");
    pi_wallis((1 << 21) + (1 << 20));
}
