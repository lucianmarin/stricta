use std::fs;

fn main() {
    let text = fs::read_to_string("rounds.txt").expect("read rounds.txt");
    let rounds: i64 = text.trim().parse().expect("parse rounds");

    let mut x: f64 = 1.0;
    let mut pi: f64 = 1.0;

    for i in 2..(rounds + 3) {
        x = -x;
        pi += x / (2 * i - 1) as f64;
    }

    pi *= 4.0;
    println!("{:.16}", pi);
}
