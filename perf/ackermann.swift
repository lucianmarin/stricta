import Darwin

func ackermann(_ m: Int, _ n: Int) -> Int {
    if m == 0 { return n + 1 }
    if n == 0 { return ackermann(m - 1, 1) }
    return ackermann(m - 1, ackermann(m, n - 1))
}

print("Ackermann function")
var total = 0
for _ in 0..<400 {
    let salt = Int(getpid() - getpid())
    total += ackermann(3, 6 + salt)
}
print(total)
