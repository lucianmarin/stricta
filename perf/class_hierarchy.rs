use std::ops::Add;

#[derive(Clone, Copy, PartialEq, Eq)]
struct Vec2D {
    x: i64,
    y: i64,
}

impl Vec2D {
    fn norm_sq(self) -> i64 {
        self.x * self.x + self.y * self.y
    }
}

impl Add for Vec2D {
    type Output = Vec2D;
    fn add(self, other: Vec2D) -> Vec2D {
        Vec2D { x: self.x + other.x, y: self.y + other.y }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
struct Vec3D {
    x: i64,
    y: i64,
    z: i64,
}

impl Vec3D {
    fn norm_sq(self) -> i64 {
        self.x * self.x + self.y * self.y + self.z * self.z
    }
}

trait Component {
    fn base_val(&self) -> i64;
    fn compute(&self, factor: i64) -> i64;
    fn is_scale_mixin(&self) -> bool { false }
    fn tag_weight(&self) -> i64 {
        self.base_val() + 7
    }
}

struct SmartNode {
    base_val: i64,
    extra: i64,
}

impl SmartNode {
    fn bonus(&self) -> i64 {
        self.base_val * 2
    }

    fn penalty(&self) -> i64 {
        self.base_val / 2
    }

    fn combine(a: i64, b: i64) -> i64 {
        (a * 17 + b * 31) % 1000003
    }
}

impl Component for SmartNode {
    fn base_val(&self) -> i64 { self.base_val }
    fn is_scale_mixin(&self) -> bool { true }
    fn compute(&self, factor: i64) -> i64 {
        let prev = self.base_val * factor;
        let offset = (prev + 97) % 1000003;
        let scaled = (offset * 3 + 11) % 1000003;
        (scaled + self.extra + self.bonus() - self.penalty()) % 1000003
    }
}

struct LeafNode {
    base_val: i64,
}

impl Component for LeafNode {
    fn base_val(&self) -> i64 { self.base_val }
    fn compute(&self, factor: i64) -> i64 {
        let prev = self.base_val * factor;
        let offset = (prev + 97) % 1000003;
        (offset + self.tag_weight()) % 1000003
    }
}

fn main() {
    let mut checksum: i64 = 1; // abc_blocked = 1
    let mut node_registry_count: i64 = 0;

    for r in 0..40_i64 {
        let mut nodes: Vec<Box<dyn Component>> = Vec::with_capacity(500);
        for i in 0..500_i64 {
            if i % 3 == 0 {
                nodes.push(Box::new(SmartNode { base_val: i + r, extra: (i * 7) % 50 }));
                node_registry_count += 1;
            } else {
                nodes.push(Box::new(LeafNode { base_val: i + r }));
            }
        }

        let mut round_acc: i64 = 0;
        for (idx, n_obj) in nodes.iter().enumerate() {
            let mut val = n_obj.compute((idx as i64 % 13) + 1);
            if n_obj.is_scale_mixin() {
                val = SmartNode::combine(val, n_obj.tag_weight());
            } else {
                val += n_obj.tag_weight();
            }
            round_acc = (round_acc + val) % 1000000007;
        }

        let mut v_acc = Vec2D { x: 0, y: 0 };
        let mut eq_hits: i64 = 0;
        for k in 0..500_i64 {
            let v1 = Vec2D { x: k, y: k * 2 };
            let v2 = Vec2D { x: r, y: k };
            v_acc = v_acc + v1 + v2;
            let v3 = Vec3D { x: k, y: k * 2, z: r };
            let v4 = Vec3D { x: k, y: k * 2, z: r };
            if v3 == v4 {
                eq_hits += v3.norm_sq();
            }
        }

        checksum = (checksum + round_acc + v_acc.norm_sq() + eq_hits + node_registry_count) % 1000000007;
    }

    println!("{}", checksum);
}
