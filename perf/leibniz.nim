import std/[strutils, strformat]

let rounds = parseInt(readFile("rounds.txt").strip())

var x = 1.0
var pi = 1.0

for i in 2 .. (rounds + 2):
  x = -x
  pi += x / float(2 * i - 1)

pi *= 4.0
echo formatFloat(pi, ffDecimal, 16)
