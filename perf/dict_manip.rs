use std::collections::HashMap;

fn run_dict_benchmark(n: i64, rounds: i64) -> i64 {
    let mut checksum: i64 = 0;
    for r in 0..rounds {
        let mut d: HashMap<i64, i64> = HashMap::new();
        for i in 0..n {
            let k = (i * 1103515245 + r * 12345) % (n * 2);
            let v = (i * 37 + r) % 10000;
            let cur = d.get(&k).copied().unwrap_or(0);
            d.insert(k, cur + v);
        }

        let mut d2: HashMap<i64, i64> = HashMap::new();
        for j in 0..(n / 2) {
            let k2 = (j * 2654435761 + r * 17) % (n * 2);
            d2.insert(k2, j * 3 + r);
        }

        let mut merged = d.clone();
        for (&k, &v) in &d2 {
            merged.insert(k, v);
        }

        let mut evicted: i64 = 0;
        for m in 0..n {
            let probe = (m * 1664525 + r) % (n * 2);
            let cur = *merged.entry(probe).or_insert(m);
            if cur % 3 == 0 {
                let popped = merged.remove(&probe).unwrap();
                evicted = (evicted + popped) % 1000000007;
            } else if cur % 5 == 0 {
                merged.remove(&probe);
                evicted += 1;
            }
        }

        let filtered: HashMap<i64, i64> = merged
            .iter()
            .filter(|(_, &v)| v % 2 == 1)
            .map(|(&k, &v)| (k, v * 2))
            .collect();

        for (&k, &v) in &filtered {
            d.insert(k, v);
        }

        let mut val_sum: i64 = 0;
        for &v in d.values() {
            val_sum = (val_sum + v) % 1000000007;
        }

        checksum = (checksum + d.len() as i64 + filtered.len() as i64 + evicted + val_sum) % 1000000007;
    }
    checksum
}

fn main() {
    let res = run_dict_benchmark(4000, 25);
    println!("{}", res);
}
