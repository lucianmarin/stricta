func fibonacciIterative(_ n: Int) -> Int64 {
    var first: Int64 = 0
    var second: Int64 = 1
    for _ in 2..<n {
        let next = first &+ second
        first = second
        second = next
    }
    return second
}

print("Calculate Fibonacci numbers iteratively")
print(fibonacciIterative((1 << 19) + (1 << 18)) & 0xFFFFFFFF)
