import Foundation

func worker(tid: Int64, steps: Int64) async -> Int64 {
    var acc = tid
    for i in 0..<steps {
        acc = (acc &* 1103515245 &+ 12345 &+ i) & 2147483647
        await Task.yield()
    }
    return acc
}

let sem = DispatchSemaphore(value: 0)
Task {
    let n: Int64 = 800
    let steps: Int64 = 2000
    let total = await withTaskGroup(of: Int64.self) { group -> Int64 in
        for i in 0..<n {
            group.addTask {
                await worker(tid: i + 1, steps: steps)
            }
        }
        var sum: Int64 = 0
        for await val in group {
            sum += val
        }
        return sum
    }
    print(total)
    sem.signal()
}
sem.wait()
