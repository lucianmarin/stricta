var six {.volatile.}: int = 6

proc ackermann(m, n: int): int =
  if m == 0:
    return n + 1
  if n == 0:
    return ackermann(m - 1, 1)
  ackermann(m - 1, ackermann(m, n - 1))

echo "Ackermann function"
var total: int = 0
for _ in 0 ..< 400:
  total += ackermann(3, six)
echo total
