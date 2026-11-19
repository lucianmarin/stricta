proc fibonacci(k: int): int =
  if k <= 1:
    1
  else:
    fibonacci(k - 1) + fibonacci(k - 2)

proc fibonacciRecursive(n: int): int =
  var total: int = 0
  for i in 1 ..< n:
    total += fibonacci(i)
  total

echo "Calculate Fibonacci numbers recursively"
echo fibonacciRecursive(32 + 4 + 2 + 1)
