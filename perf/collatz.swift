func collatz(_ limit: Int) -> Int {
    var totalSteps = 0
    for i in 1..<limit {
        var n = i
        var steps = 0
        while n > 1 {
            if n % 2 == 0 {
                n /= 2
            } else {
                n = 3 * n + 1
            }
            steps += 1
        }
        totalSteps += steps
    }
    return totalSteps
}

print("Collatz sequence")
print(collatz(1000000))
