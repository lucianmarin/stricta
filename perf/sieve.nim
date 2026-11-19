proc sieve(n: int): int =
  var flags = newSeq[bool](n)
  for i in 0 ..< n:
    flags[i] = true
  flags[0] = false
  flags[1] = false

  var i = 2
  while i < n:
    if flags[i]:
      var j = i * i
      while j < n:
        flags[j] = false
        j += i
    inc i

  var count = 0
  for k in 0 ..< n:
    if flags[k]:
      inc count
  count

echo "Sieve of Eratosthenes"
echo sieve(2_000_000)
