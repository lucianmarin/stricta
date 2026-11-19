func sieve(_ n: Int) -> Int {
    var flags = [Int](repeating: 1, count: n)
    flags[0] = 0
    flags[1] = 0
    for i in 2..<n {
        if flags[i] == 1 {
            var j = i * i
            while j < n {
                flags[j] = 0
                j += i
            }
        }
    }
    var count = 0
    for k in 0..<n {
        count += flags[k]
    }
    return count
}

print("Sieve of Eratosthenes")
print(sieve(2000000))
