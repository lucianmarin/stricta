use std::collections::HashSet;

fn run_set_benchmark(n: i64, rounds: i64) -> i64 {
    let mut checksum: i64 = 0;
    for r in 0..rounds {
        let mut s1: HashSet<i64> = HashSet::new();
        let mut s2: HashSet<i64> = HashSet::new();
        let mut s3: HashSet<i64> = HashSet::new();

        for i in 0..n {
            let v1 = (i * 2654435761 + r * 97) % (n * 2);
            let v2 = (i * 1597334677 + r * 131) % (n * 2);
            let v3 = (i * 2246822519 + r * 53) % (n * 2);
            s1.insert(v1);
            s2.insert(v2);
            s3.insert(v3);
        }

        let u12: HashSet<i64> = &s1 | &s2;
        let inter12: HashSet<i64> = &s1 & &s2;
        let diff12: HashSet<i64> = &s1 - &s2;
        let sym12: HashSet<i64> = &s1 ^ &s2;

        let u_all: HashSet<i64> = &u12 | &s3;
        let inter_all: HashSet<i64> = &inter12 & &s3;
        let diff_all: HashSet<i64> = &diff12 - &s3;
        let sym_all: HashSet<i64> = &sym12 ^ &s3;

        if inter12.is_subset(&s1) && s2.is_subset(&u12) {
            checksum += 1;
        }
        if diff12.is_disjoint(&inter12) {
            checksum += 1;
        }

        let mut work = s1.clone();
        work.extend(&s2);
        for x in &inter_all {
            work.remove(x);
        }
        for &x in &sym_all {
            if !work.remove(&x) {
                work.insert(x);
            }
        }

        let mut hits: i64 = 0;
        for j in 0..n {
            let probe = (j * 3266489917 + r) % (n * 2);
            if work.remove(&probe) {
                hits += 1;
            }
        }

        let mut total_rem: i64 = 0;
        for &val in &work {
            total_rem = (total_rem + val) % 1000000007;
        }

        checksum = (checksum + u_all.len() as i64 + inter_all.len() as i64 + diff_all.len() as i64 + hits + total_rem) % 1000000007;
    }
    checksum
}

fn main() {
    let res = run_set_benchmark(4000, 25);
    println!("{}", res);
}
