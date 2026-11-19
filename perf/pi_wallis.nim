import std/strutils

proc piWallis(n: int): float =
  var pi: float = 2.0
  for i in 1 ..< n:
    let left = (2.0 * float(i)) / (2.0 * float(i) - 1.0)
    let right = (2.0 * float(i)) / (2.0 * float(i) + 1.0)
    pi = pi * left * right
  pi

echo "Calculate Pi using Wallis product"
echo formatFloat(piWallis((1 shl 21) + (1 shl 20)), ffDecimal, 16)
