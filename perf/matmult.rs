struct Rng(i64);

impl Rng {
    fn next_f64(&mut self) -> f64 {
        self.0 = (self.0 * 1103515245 + 12345) & 0x7fffffff;
        self.0 as f64 / 2147483648.0
    }
}

fn multiply_matrices(size: usize) {
    let mut rng = Rng(1234567);
    let a: Vec<Vec<f64>> = (0..size)
        .map(|_| (0..size).map(|_| rng.next_f64()).collect())
        .collect();
    let b: Vec<Vec<f64>> = (0..size)
        .map(|_| (0..size).map(|_| rng.next_f64()).collect())
        .collect();
    let mut c: Vec<Vec<f64>> = vec![vec![0.0; size]; size];

    for i in 0..size {
        for j in 0..size {
            let mut s = 0.0;
            for k in 0..size {
                s += a[i][k] * b[k][j];
            }
            c[i][j] = s;
        }
    }
}

fn main() {
    println!("Multiply matrices");
    multiply_matrices(1 << 9);
}
