struct BSTNode {
    key: i64,
    val: i64,
    left: Option<Box<BSTNode>>,
    right: Option<Box<BSTNode>>,
    size: i64,
    subtree_sum: i64,
}

impl BSTNode {
    fn new(key: i64, val: i64) -> Self {
        BSTNode {
            key,
            val,
            left: None,
            right: None,
            size: 1,
            subtree_sum: val,
        }
    }
}

fn bst_insert_node(node: &mut BSTNode, key: i64, val: i64) -> (i64, i64) {
    if key == node.key {
        let diff = val - node.val;
        node.val = val;
        node.subtree_sum += diff;
        (0, diff)
    } else if key < node.key {
        if let Some(ref mut left) = node.left {
            let (ds, dsum) = bst_insert_node(left, key, val);
            node.size += ds;
            node.subtree_sum += dsum;
            (ds, dsum)
        } else {
            node.left = Some(Box::new(BSTNode::new(key, val)));
            node.size += 1;
            node.subtree_sum += val;
            (1, val)
        }
    } else if let Some(ref mut right) = node.right {
        let (ds, dsum) = bst_insert_node(right, key, val);
        node.size += ds;
        node.subtree_sum += dsum;
        (ds, dsum)
    } else {
        node.right = Some(Box::new(BSTNode::new(key, val)));
        node.size += 1;
        node.subtree_sum += val;
        (1, val)
    }
}

fn bst_insert(root: Option<Box<BSTNode>>, key: i64, val: i64) -> Box<BSTNode> {
    match root {
        None => Box::new(BSTNode::new(key, val)),
        Some(mut r) => {
            bst_insert_node(&mut r, key, val);
            r
        }
    }
}

fn bst_range_sum(node: &Option<Box<BSTNode>>, low: i64, high: i64) -> i64 {
    let n = match node {
        None => return 0,
        Some(n) => n,
    };
    let mut total: i64 = 0;
    if low <= n.key && n.key <= high {
        total += n.val;
    }
    if low < n.key && n.left.is_some() {
        total += bst_range_sum(&n.left, low, high);
    }
    if n.key < high && n.right.is_some() {
        total += bst_range_sum(&n.right, low, high);
    }
    total
}

fn bst_mirror_weighted(node: &Option<Box<BSTNode>>, depth: i64) -> i64 {
    let n = match node {
        None => return 0,
        Some(n) => n,
    };
    let l_val = bst_mirror_weighted(&n.left, depth + 1);
    let r_val = bst_mirror_weighted(&n.right, depth + 1);
    (n.key * depth + n.val + l_val + r_val) % 1000000007
}

struct RoseNode {
    id_val: i64,
    weight: i64,
    children: Vec<RoseNode>,
}

fn build_rose_tree(branch_factor: i64, depth: i64, seed: i64) -> RoseNode {
    let mut root = RoseNode {
        id_val: seed,
        weight: (seed * 31 + depth) % 997,
        children: Vec::new(),
    };
    if depth <= 0 {
        return root;
    }
    for i in 0..branch_factor {
        let child_seed = (seed * branch_factor + i + 1) % 1000003;
        root.children.push(build_rose_tree(branch_factor, depth - 1, child_seed));
    }
    root
}

fn eval_rose_tree(node: &RoseNode, mult: i64) -> i64 {
    let mut acc = (node.id_val * mult + node.weight) % 1000000007;
    for (c_i, child) in node.children.iter().enumerate() {
        acc = (acc + eval_rose_tree(child, (mult + c_i as i64 + 1) % 257)) % 1000000007;
    }
    acc
}

fn main() {
    let mut checksum: i64 = 0;
    for r in 0..20_i64 {
        let mut root: Option<Box<BSTNode>> = None;
        for i in 0..2000_i64 {
            let k = (i * 2654435761 + r * 1013904223) % 10000;
            let v = (i * 41 + r * 19) % 5000;
            root = Some(bst_insert(root, k, v));
        }

        let mut q_sum: i64 = 0;
        for q in 0..200_i64 {
            let low = (q * 47 + r * 13) % 8000;
            let high = low + 1500;
            q_sum = (q_sum + bst_range_sum(&root, low, high)) % 1000000007;
        }

        let m1 = bst_mirror_weighted(&root, 1);
        let m2 = bst_mirror_weighted(&root, 1);

        let rose = build_rose_tree(4, 5, r + 1);
        let r_eval = eval_rose_tree(&rose, 3);

        if let Some(ref rt) = root {
            checksum = (checksum + rt.size + rt.subtree_sum + q_sum + m1 + m2 + r_eval) % 1000000007;
        }
    }
    println!("{}", checksum);
}
