func runDictBenchmark(_ n: Int, _ rounds: Int) -> Int {
    var checksum = 0
    for r in 0..<rounds {
        var d: [Int: Int] = [:]
        for i in 0..<n {
            let k = (i * 1103515245 + r * 12345) % (n * 2)
            let v = (i * 37 + r) % 10000
            d[k, default: 0] += v
        }
        var d2: [Int: Int] = [:]
        for j in 0..<(n / 2) {
            let k2 = (j * 2654435761 + r * 17) % (n * 2)
            d2[k2] = j * 3 + r
        }
        var merged = d
        for (k, v) in d2 { merged[k] = v }

        var evicted = 0
        for m in 0..<n {
            let probe = (m * 1664525 + r) % (n * 2)
            let cur: Int
            if let existing = merged[probe] {
                cur = existing
            } else {
                merged[probe] = m
                cur = m
            }
            if cur % 3 == 0 {
                if let popped = merged.removeValue(forKey: probe) {
                    evicted = (evicted + popped) % 1000000007
                }
            } else if cur % 5 == 0 {
                merged.removeValue(forKey: probe)
                evicted += 1
            }
        }
        var filtered: [Int: Int] = [:]
        for (k, v) in merged where v % 2 == 1 {
            filtered[k] = v * 2
        }
        for (k, v) in filtered { d[k] = v }

        var valSum = 0
        for v in d.values {
            valSum = (valSum + v) % 1000000007
        }
        checksum = (checksum + d.count + filtered.count + evicted + valSum) % 1000000007
    }
    return checksum
}

print(runDictBenchmark(4000, 25))
