proc fibonacciIterative(n: int): int =
  var first: int = 0
  var second: int = 1
  for _ in 2 ..< n:
    let next = first +% second
    first = second
    second = next
  second

echo "Calculate Fibonacci numbers iteratively"
echo(fibonacciIterative((1 shl 19) + (1 shl 18)) and 0xFFFFFFFF)
