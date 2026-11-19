import std/tables

proc runDictBenchmark(n, rounds: int64): int64 =
  var checksum: int64 = 0
  for r in 0'i64 ..< rounds:
    var d = initTable[int64, int64]()
    var i: int64 = 0
    while i < n:
      let k = (i * 1103515245 + r * 12345) mod (n * 2)
      let v = (i * 37 + r) mod 10000
      d[k] = d.getOrDefault(k, 0) + v
      inc i

    var d2 = initTable[int64, int64]()
    var j: int64 = 0
    while j < n div 2:
      let k2 = (j * 2654435761 + r * 17) mod (n * 2)
      d2[k2] = j * 3 + r
      inc j

    var merged = initTable[int64, int64]()
    for k, v in d:
      merged[k] = v
    for k, v in d2:
      merged[k] = v

    var evicted: int64 = 0
    var m: int64 = 0
    while m < n:
      let probe = (m * 1664525 + r) mod (n * 2)
      let cur = merged.mgetOrPut(probe, m)
      if cur mod 3 == 0:
        var popped: int64
        discard merged.pop(probe, popped)
        evicted = (evicted + popped) mod 1000000007
      elif cur mod 5 == 0:
        merged.del(probe)
        inc evicted
      inc m

    var filtered = initTable[int64, int64]()
    for k, v in merged:
      if v mod 2 == 1:
        filtered[k] = v * 2

    for k, v in filtered:
      d[k] = v

    var valSum: int64 = 0
    for v in d.values:
      valSum = (valSum + v) mod 1000000007

    checksum = (checksum + int64(d.len) + int64(filtered.len) + evicted +
                valSum) mod 1000000007

  checksum

echo runDictBenchmark(4000, 25)
