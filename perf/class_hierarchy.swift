struct Vec2D: Equatable {
    var x: Int = 0
    var y: Int = 0
    func normSq() -> Int { x * x + y * y }
    static func + (lhs: Vec2D, rhs: Vec2D) -> Vec2D {
        Vec2D(x: lhs.x + rhs.x, y: lhs.y + rhs.y)
    }
}

struct Vec3D: Equatable {
    var x: Int = 0
    var y: Int = 0
    var z: Int = 0
    func normSq() -> Int { x * x + y * y + z * z }
}

class Component {
    var baseVal: Int
    init(_ baseVal: Int) { self.baseVal = baseVal }
    func compute(_ factor: Int) -> Int { baseVal * factor }
    var tagWeight: Int { baseVal + 7 }
}

class OffsetMixin: Component {
    override func compute(_ factor: Int) -> Int {
        let prev = super.compute(factor)
        return (prev + 97) % 1000003
    }
    func penalty() -> Int { baseVal / 2 }
}

class ScaleMixin: OffsetMixin {
    override func compute(_ factor: Int) -> Int {
        let prev = super.compute(factor)
        return (prev * 3 + 11) % 1000003
    }
    func bonus() -> Int { baseVal * 2 }
}

class SmartNode: ScaleMixin {
    static var nodeRegistryCount = 0
    var extra: Int
    init(_ baseVal: Int, _ extra: Int) {
        self.extra = extra
        super.init(baseVal)
    }
    override func compute(_ factor: Int) -> Int {
        let res = super.compute(factor)
        return (res + extra + bonus() - penalty()) % 1000003
    }
    static func combine(_ a: Int, _ b: Int) -> Int { (a * 17 + b * 31) % 1000003 }
    @discardableResult
    static func bumpRegistry() -> Int {
        nodeRegistryCount += 1
        return nodeRegistryCount
    }
}

class LeafNode: OffsetMixin {
    override init(_ baseVal: Int) { super.init(baseVal) }
    override func compute(_ factor: Int) -> Int {
        (super.compute(factor) + tagWeight) % 1000003
    }
}

var checksum = 1
for r in 0..<40 {
    var nodes: [Component] = []
    nodes.reserveCapacity(500)
    for i in 0..<500 {
        if i % 3 == 0 {
            nodes.append(SmartNode(i + r, (i * 7) % 50))
            SmartNode.bumpRegistry()
        } else {
            nodes.append(LeafNode(i + r))
        }
    }
    var roundAcc = 0
    for idx in 0..<nodes.count {
        let nObj = nodes[idx]
        var val = nObj.compute((idx % 13) + 1)
        if nObj is ScaleMixin {
            val = SmartNode.combine(val, nObj.tagWeight)
        } else {
            val += nObj.tagWeight
        }
        roundAcc = (roundAcc + val) % 1000000007
    }
    var vAcc = Vec2D(x: 0, y: 0)
    var eqHits = 0
    for k in 0..<500 {
        let v1 = Vec2D(x: k, y: k * 2)
        let v2 = Vec2D(x: r, y: k)
        vAcc = vAcc + v1 + v2
        let v3 = Vec3D(x: k, y: k * 2, z: r)
        let v4 = Vec3D(x: k, y: k * 2, z: r)
        if v3 == v4 {
            eqHits += v3.normSq()
        }
    }
    checksum = (checksum + roundAcc + vAcc.normSq() + eqHits + SmartNode.nodeRegistryCount) % 1000000007
}
print(checksum)
