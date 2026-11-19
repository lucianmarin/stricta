proc collatz(limit: int): int =
  var totalSteps = 0
  for i in 1 ..< limit:
    var n = i
    var steps = 0
    while n > 1:
      if n mod 2 == 0:
        n = n div 2
      else:
        n = 3 * n + 1
      inc steps
    totalSteps += steps
  totalSteps

echo "Collatz sequence"
echo collatz(1_000_000)
