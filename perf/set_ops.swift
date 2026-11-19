func runSetBenchmark(_ n: Int, _ rounds: Int) -> Int {
    var checksum = 0
    for r in 0..<rounds {
        var s1 = Set<Int>()
        var s2 = Set<Int>()
        var s3 = Set<Int>()
        for i in 0..<n {
            s1.insert((i * 2654435761 + r * 97) % (n * 2))
            s2.insert((i * 1597334677 + r * 131) % (n * 2))
            s3.insert((i * 2246822519 + r * 53) % (n * 2))
        }
        let u12 = s1.union(s2)
        let inter12 = s1.intersection(s2)
        let diff12 = s1.subtracting(s2)
        let sym12 = s1.symmetricDifference(s2)

        let uAll = u12.union(s3)
        let interAll = inter12.intersection(s3)
        let diffAll = diff12.subtracting(s3)
        let symAll = sym12.symmetricDifference(s3)

        if inter12.isSubset(of: s1) && u12.isSuperset(of: s2) {
            checksum += 1
        }
        if diff12.isDisjoint(with: inter12) {
            checksum += 1
        }

        var work = s1
        work.formUnion(s2)
        work.subtract(interAll)
        work.formSymmetricDifference(symAll)

        var hits = 0
        for j in 0..<n {
            let probe = (j * 3266489917 + r) % (n * 2)
            if work.remove(probe) != nil {
                hits += 1
            }
        }
        var totalRem = 0
        for val in work {
            totalRem = (totalRem + val) % 1000000007
        }
        checksum = (checksum + uAll.count + interAll.count + diffAll.count + hits + totalRem) % 1000000007
    }
    return checksum
}

print(runSetBenchmark(4000, 25))
