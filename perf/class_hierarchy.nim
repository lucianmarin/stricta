type
  Vec2D = object
    x, y: int64
  Vec3D = object
    x, y, z: int64

proc normSq(v: Vec2D): int64 =
  v.x * v.x + v.y * v.y

proc normSq(v: Vec3D): int64 =
  v.x * v.x + v.y * v.y + v.z * v.z

proc `+`(a, b: Vec2D): Vec2D =
  Vec2D(x: a.x + b.x, y: a.y + b.y)

proc `==`(a, b: Vec3D): bool =
  a.x == b.x and a.y == b.y and a.z == b.z

type
  Component = ref object of RootObj
    baseVal: int64
  ScaleMixin = ref object of Component
  OffsetMixin = ref object of Component
  SmartNode = ref object of ScaleMixin
    extra: int64
  LeafNode = ref object of OffsetMixin

var nodeRegistryCount: int64 = 0

proc tagWeight(c: Component): int64 =
  c.baseVal + 7

method compute(c: Component, factor: int64): int64 {.base.} =
  c.baseVal * factor

method compute(n: SmartNode, factor: int64): int64 =
  let base = n.baseVal * factor
  let offset = (base + 97) mod 1000003
  let scaled = (offset * 3 + 11) mod 1000003
  (scaled + n.extra + n.baseVal * 2 - n.baseVal div 2) mod 1000003

method compute(n: LeafNode, factor: int64): int64 =
  let base = n.baseVal * factor
  let offset = (base + 97) mod 1000003
  (offset + n.tagWeight) mod 1000003

var checksum: int64 = 1  # ABC construction is blocked, as in the .str version
for r in 0'i64 ..< 40:
  var nodes: seq[Component] = @[]
  var i: int64 = 0
  while i < 500:
    if i mod 3 == 0:
      nodes.add(SmartNode(baseVal: i + r, extra: (i * 7) mod 50))
      inc nodeRegistryCount
    else:
      nodes.add(LeafNode(baseVal: i + r))
    inc i

  var roundAcc: int64 = 0
  var idx = 0
  while idx < nodes.len:
    let nObj = nodes[idx]
    var val = nObj.compute((int64(idx) mod 13) + 1)
    if nObj of ScaleMixin:
      val = (val * 17 + nObj.tagWeight * 31) mod 1000003
    else:
      val = val + nObj.tagWeight
    roundAcc = (roundAcc + val) mod 1000000007
    inc idx

  var vAcc = Vec2D(x: 0, y: 0)
  var eqHits: int64 = 0
  var k: int64 = 0
  while k < 500:
    let v1 = Vec2D(x: k, y: k * 2)
    let v2 = Vec2D(x: r, y: k)
    vAcc = vAcc + v1 + v2
    let v3 = Vec3D(x: k, y: k * 2, z: r)
    let v4 = Vec3D(x: k, y: k * 2, z: r)
    if v3 == v4:
      eqHits += v3.normSq()
    inc k

  checksum = (checksum + roundAcc + vAcc.normSq() + eqHits +
              nodeRegistryCount) mod 1000000007

echo checksum
