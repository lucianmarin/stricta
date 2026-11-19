type
  BSTNode = ref object
    key, val, size, subtreeSum: int64
    left, right: BSTNode
  RoseNode = ref object
    idVal, weight: int64
    children: seq[RoseNode]

proc newNode(key, val: int64): BSTNode =
  BSTNode(key: key, val: val, left: nil, right: nil, size: 1, subtreeSum: val)

proc bstInsertNode(node: BSTNode, key, val: int64): tuple[ds, dsum: int64] =
  if key == node.key:
    let diff = val - node.val
    node.val = val
    node.subtreeSum += diff
    return (0'i64, diff)
  elif key < node.key:
    if node.left.isNil:
      node.left = newNode(key, val)
      inc node.size
      node.subtreeSum += val
      return (1'i64, val)
    let (ds, dsum) = bstInsertNode(node.left, key, val)
    node.size += ds
    node.subtreeSum += dsum
    return (ds, dsum)
  else:
    if node.right.isNil:
      node.right = newNode(key, val)
      inc node.size
      node.subtreeSum += val
      return (1'i64, val)
    let (ds, dsum) = bstInsertNode(node.right, key, val)
    node.size += ds
    node.subtreeSum += dsum
    return (ds, dsum)

proc bstInsert(root: BSTNode, key, val: int64): BSTNode =
  if root.isNil:
    return newNode(key, val)
  discard bstInsertNode(root, key, val)
  root

proc bstRangeSum(node: BSTNode, low, high: int64): int64 =
  if node.isNil:
    return 0
  var total: int64 = 0
  if low <= node.key and node.key <= high:
    total += node.val
  if low < node.key and not node.left.isNil:
    total += bstRangeSum(node.left, low, high)
  if node.key < high and not node.right.isNil:
    total += bstRangeSum(node.right, low, high)
  total

proc bstMirrorWeighted(node: BSTNode, depth: int64): int64 =
  if node.isNil:
    return 0
  let lVal = bstMirrorWeighted(node.left, depth + 1)
  let rVal = bstMirrorWeighted(node.right, depth + 1)
  (node.key * depth + node.val + lVal + rVal) mod 1000000007

proc buildRoseTree(branchFactor, depth, seed: int64): RoseNode =
  result = RoseNode(idVal: seed, weight: (seed * 31 + depth) mod 997)
  if depth <= 0:
    return
  var i: int64 = 0
  while i < branchFactor:
    let childSeed = (seed * branchFactor + i + 1) mod 1000003
    result.children.add(buildRoseTree(branchFactor, depth - 1, childSeed))
    inc i

proc evalRoseTree(node: RoseNode, mult: int64): int64 =
  result = (node.idVal * mult + node.weight) mod 1000000007
  for cIdx, child in node.children:
    result = (result + evalRoseTree(child, (mult + int64(cIdx) + 1) mod 257)) mod
      1000000007

var checksum: int64 = 0
for r in 0'i64 ..< 20:
  var root: BSTNode = nil
  var i: int64 = 0
  while i < 2000:
    let k = (i * 2654435761 + r * 1013904223) mod 10000
    let v = (i * 41 + r * 19) mod 5000
    root = bstInsert(root, k, v)
    inc i

  var qSum: int64 = 0
  var q: int64 = 0
  while q < 200:
    let low = (q * 47 + r * 13) mod 8000
    let high = low + 1500
    qSum = (qSum + bstRangeSum(root, low, high)) mod 1000000007
    inc q

  let m1 = bstMirrorWeighted(root, 1)
  let m2 = bstMirrorWeighted(root, 1)

  let rose = buildRoseTree(4, 5, r + 1)
  let rEval = evalRoseTree(rose, 3)

  if not root.isNil:
    checksum = (checksum + root.size + root.subtreeSum + qSum + m1 + m2 +
                rEval) mod 1000000007

echo checksum
