final class BSTNode {
    var key: Int
    var val: Int
    var left: BSTNode?
    var right: BSTNode?
    var size: Int
    var subtreeSum: Int

    init(_ key: Int, _ val: Int) {
        self.key = key
        self.val = val
        self.size = 1
        self.subtreeSum = val
    }
}

func bstInsertNode(_ node: BSTNode, _ key: Int, _ val: Int) -> (Int, Int) {
    if key == node.key {
        let diff = val - node.val
        node.val = val
        node.subtreeSum += diff
        return (0, diff)
    } else if key < node.key {
        if let left = node.left {
            let (ds, dsum) = bstInsertNode(left, key, val)
            node.size += ds
            node.subtreeSum += dsum
            return (ds, dsum)
        } else {
            node.left = BSTNode(key, val)
            node.size += 1
            node.subtreeSum += val
            return (1, val)
        }
    } else {
        if let right = node.right {
            let (ds, dsum) = bstInsertNode(right, key, val)
            node.size += ds
            node.subtreeSum += dsum
            return (ds, dsum)
        } else {
            node.right = BSTNode(key, val)
            node.size += 1
            node.subtreeSum += val
            return (1, val)
        }
    }
}

func bstInsert(_ root: BSTNode?, _ key: Int, _ val: Int) -> BSTNode {
    guard let root = root else { return BSTNode(key, val) }
    _ = bstInsertNode(root, key, val)
    return root
}

func bstRangeSum(_ node: BSTNode?, _ low: Int, _ high: Int) -> Int {
    guard let node = node else { return 0 }
    var total = 0
    if low <= node.key && node.key <= high {
        total += node.val
    }
    if low < node.key, let left = node.left {
        total += bstRangeSum(left, low, high)
    }
    if node.key < high, let right = node.right {
        total += bstRangeSum(right, low, high)
    }
    return total
}

func bstMirrorWeighted(_ node: BSTNode?, _ depth: Int) -> Int {
    guard let node = node else { return 0 }
    let lVal = bstMirrorWeighted(node.left, depth + 1)
    let rVal = bstMirrorWeighted(node.right, depth + 1)
    return (node.key * depth + node.val + lVal + rVal) % 1000000007
}

final class RoseNode {
    var idVal: Int
    var weight: Int
    var children: [RoseNode] = []

    init(_ idVal: Int, _ weight: Int) {
        self.idVal = idVal
        self.weight = weight
    }
}

func buildRoseTree(_ branchFactor: Int, _ depth: Int, _ seed: Int) -> RoseNode {
    let root = RoseNode(seed, (seed * 31 + depth) % 997)
    if depth <= 0 { return root }
    root.children.reserveCapacity(branchFactor)
    for i in 0..<branchFactor {
        let childSeed = (seed * branchFactor + i + 1) % 1000003
        root.children.append(buildRoseTree(branchFactor, depth - 1, childSeed))
    }
    return root
}

func evalRoseTree(_ node: RoseNode, _ mult: Int) -> Int {
    var acc = (node.idVal * mult + node.weight) % 1000000007
    for cI in 0..<node.children.count {
        acc = (acc + evalRoseTree(node.children[cI], (mult + cI + 1) % 257)) % 1000000007
    }
    return acc
}

var checksum = 0
for r in 0..<20 {
    var root: BSTNode? = nil
    for i in 0..<2000 {
        let k = (i * 2654435761 + r * 1013904223) % 10000
        let v = (i * 41 + r * 19) % 5000
        root = bstInsert(root, k, v)
    }
    var qSum = 0
    for q in 0..<200 {
        let low = (q * 47 + r * 13) % 8000
        let high = low + 1500
        qSum = (qSum + bstRangeSum(root, low, high)) % 1000000007
    }
    let m1 = bstMirrorWeighted(root, 1)
    let m2 = bstMirrorWeighted(root, 1)
    let rose = buildRoseTree(4, 5, r + 1)
    let rEval = evalRoseTree(rose, 3)
    if let root = root {
        checksum = (checksum + root.size + root.subtreeSum + qSum + m1 + m2 + rEval) % 1000000007
    }
}
print(checksum)
