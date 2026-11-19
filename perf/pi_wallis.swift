import Foundation

func piWallis(_ n: Int) -> Double {
    var pi = 2.0
    for i in 1..<n {
        let di = Double(i)
        let left = (2.0 * di) / (2.0 * di - 1.0)
        let right = (2.0 * di) / (2.0 * di + 1.0)
        pi = pi * left * right
    }
    return pi
}

print("Calculate Pi using Wallis product")
print(String(format: "%.16f", piWallis((1 << 21) + (1 << 20))))
