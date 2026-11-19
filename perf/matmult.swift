import Foundation

var seed: Int64 = 1234567
func nextRandom() -> Double {
    seed = (seed &* 1103515245 &+ 12345) & 0x7fffffff
    return Double(seed) / 2147483648.0
}

func multiplyMatrices(_ size: Int) -> Double {
    var A = [[Double]](repeating: [Double](repeating: 0.0, count: size), count: size)
    var B = [[Double]](repeating: [Double](repeating: 0.0, count: size), count: size)
    var C = [[Double]](repeating: [Double](repeating: 0.0, count: size), count: size)
    for i in 0..<size {
        for j in 0..<size { A[i][j] = nextRandom() }
    }
    for i in 0..<size {
        for j in 0..<size { B[i][j] = nextRandom() }
    }
    for i in 0..<size {
        for j in 0..<size {
            var s = 0.0
            for k in 0..<size {
                s += A[i][k] * B[k][j]
            }
            C[i][j] = s
        }
    }
    var checksum = 0.0
    for i in 0..<size {
        for j in 0..<size { checksum += C[i][j] }
    }
    return checksum
}

print("Multiply matrices")
print(String(format: "%.17g", multiplyMatrices(512)))
