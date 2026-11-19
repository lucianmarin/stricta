struct Task {
    acc: i64,
    i: i64,
    steps: i64,
    done: bool,
}

impl Task {
    fn new(tid: i64, steps: i64) -> Self {
        Self { acc: tid, i: 0, steps, done: false }
    }

    fn step(&mut self) {
        if self.done {
            return;
        }
        if self.i < self.steps {
            self.acc = (self.acc.wrapping_mul(1103515245).wrapping_add(12345 + self.i)) & 2147483647;
            self.i += 1;
        } else {
            self.done = true;
        }
    }
}

fn main() {
    let n: i64 = 800;
    let steps: i64 = 2000;
    let mut tasks: Vec<Task> = (0..n).map(|i| Task::new(i + 1, steps)).collect();
    loop {
        let mut all_done = true;
        for t in tasks.iter_mut() {
            if !t.done {
                t.step();
                if !t.done {
                    all_done = false;
                }
            }
        }
        if all_done {
            break;
        }
    }
    let total: i64 = tasks.iter().map(|t| t.acc).sum();
    println!("{}", total);
}
