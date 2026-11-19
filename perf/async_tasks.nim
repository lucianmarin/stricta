proc makeWorker(tid: int64, steps: int64): iterator(): int64 {.closure.} =
  return iterator(): int64 {.closure.} =
    var acc: int64 = tid
    for i in 0 ..< steps:
      acc = (acc * 1103515245'i64 + 12345'i64 + i) and 2147483647'i64
      yield 0'i64
    yield acc

proc main() =
  const n = 800
  const steps = 2000'i64
  var tasks = newSeq[iterator(): int64 {.closure.}](n)
  var results = newSeq[int64](n)
  for i in 0 ..< n:
    tasks[i] = makeWorker(int64(i + 1), steps)
  while true:
    var allDone = true
    for i in 0 ..< n:
      if not finished(tasks[i]):
        let v = tasks[i]()
        if finished(tasks[i]):
          discard
        else:
          results[i] = v
          allDone = false
    if allDone:
      break
  var total: int64 = 0
  for i in 0 ..< n:
    total += results[i]
  echo total

main()
