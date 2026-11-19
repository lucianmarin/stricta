var seed: int64 = 1234567

proc random(): float =
  seed = (seed * 1103515245 + 12345) and 0x7fffffff'i64
  float(seed) / 2147483648.0

proc multiplyMatrices(size: int): float =
  var a = newSeq[seq[float]](size)
  var b = newSeq[seq[float]](size)
  var c = newSeq[seq[float]](size)

  for i in 0 ..< size:
    a[i] = newSeq[float](size)
    for j in 0 ..< size:
      a[i][j] = random()

  for i in 0 ..< size:
    b[i] = newSeq[float](size)
    for j in 0 ..< size:
      b[i][j] = random()

  for i in 0 ..< size:
    c[i] = newSeq[float](size)

  for i in 0 ..< size:
    for j in 0 ..< size:
      var s = 0.0
      for k in 0 ..< size:
        s += a[i][k] * b[k][j]
      c[i][j] = s

  var checksum = 0.0
  for i in 0 ..< size:
    for j in 0 ..< size:
      checksum += c[i][j]
  checksum

echo "Multiply matrices"
echo multiplyMatrices(1 shl 9)
