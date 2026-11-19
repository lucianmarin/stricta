import Foundation

let text = try String(contentsOfFile: "rounds.txt", encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines)
let rounds = Int(text)!

var x = 1.0
var pi = 1.0
for i in 2..<(rounds + 3) {
    x = -x
    pi += x / Double(2 * i - 1)
}
pi *= 4.0
print(String(format: "%.16f", pi))
