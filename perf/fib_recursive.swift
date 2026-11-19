func fibonacci(_ k: Int) -> Int {
    if k <= 1 { return 1 }
    return fibonacci(k - 1) + fibonacci(k - 2)
}

func fibonacciRecursive(_ n: Int) -> Int {
    var total = 0
    for i in 1..<n {
        total += fibonacci(i)
    }
    return total
}

print("Calculate Fibonacci numbers recursively")
print(fibonacciRecursive(39))
