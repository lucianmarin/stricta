import std/sets

proc runSetBenchmark(n, rounds: int64): int64 =
  var checksum: int64 = 0
  for r in 0'i64 ..< rounds:
    var s1 = initHashSet[int64]()
    var s2 = initHashSet[int64]()
    var s3 = initHashSet[int64]()

    var i: int64 = 0
    while i < n:
      let v1 = (i * 2654435761 + r * 97) mod (n * 2)
      let v2 = (i * 1597334677 + r * 131) mod (n * 2)
      let v3 = (i * 2246822519 + r * 53) mod (n * 2)
      s1.incl(v1)
      s2.incl(v2)
      s3.incl(v3)
      inc i

    let u12 = s1 + s2
    let inter12 = s1 * s2
    let diff12 = s1 - s2
    let sym12 = s1 -+- s2

    let uAll = u12 + s3
    let interAll = inter12 * s3
    let diffAll = diff12 - s3
    let symAll = sym12 -+- s3

    if inter12 <= s1 and s2 <= u12:
      inc checksum
    if diff12.disjoint(inter12):
      inc checksum

    var work = s1 + s2
    work = work - interAll
    work = work -+- symAll

    var j: int64 = 0
    var hits: int64 = 0
    while j < n:
      let probe = (j * 3266489917 + r) mod (n * 2)
      if work.contains(probe):
        inc hits
        work.excl(probe)
      inc j

    var totalRem: int64 = 0
    for val in work:
      totalRem = (totalRem + val) mod 1000000007

    checksum = (checksum + int64(uAll.len) + int64(interAll.len) +
                int64(diffAll.len) + hits + totalRem) mod 1000000007

  checksum

echo runSetBenchmark(4000, 25)
