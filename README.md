# 🏎️ Turbo: Strictly Typed Pythonic Native Compiler

Turbo is a strictly statically typed ahead-of-time programming language (`.turbo` extension) with Pythonic syntax that compiles directly to native machine code via a single-file C11 compiler (`turbo.c`), LLVM IR (`.ll`), and `clang -O2`—with **zero dynamic `TurboVal` NaN-boxing fallback**.

---

## ⚡ Philosophy & Architecture

* **Strict Static Typing**: Every parameter, field, and container type is statically resolved at compile time (`int`/`i64`, `i8`–`i64`, `u8`–`u64`, `float`/`f64`, `f32`, `bool`, `str`, `bytes`, `bigint`, `list[T]`, `set[T]`, `dict[K, V]`, `tuple[...]`, `T | None`, `Callable`, `@dataclass`, `class`, `ABC`), with deterministic local initialization inference and flow-sensitive `is None` / `is not None` narrowing.
* **Zero-Overhead Data Structures & OOP**:
  * Classes lower to flat LLVM structs with direct `getelementptr` field offsets (zero string-keyed attribute dicts).
  * Non-recursive `@dataclass`es use value semantics passed/returned directly in registers.
  * `list[T]`, `set[T]`, and `dict[K, V]` are monomorphized in LLVM IR with unboxed element storage, inline open-addressing hash tables, and stack-backed non-escaping local lists.
  * Polymorphic hierarchies and cooperative diamond `super()` dispatch via `type_id` switches, with `O(1)` bitmask `isinstance()` checks.
  * `range()` loops, `dict.items()`/`values()` loops, comprehensions, and `sum(genexpr)` / matrix multiplication lower directly to native loops.
* **Single-File C11 Compiler**: `turbo.c` compiles in milliseconds with zero external dependencies and emits LLVM IR linked against a lean typed C runtime (`runtime.h` / `runtime.c`).

---

## 🚀 Quick Start

### Prerequisites
* `clang` (with LLVM IR support) and `make`.

### Build & Run All Tests

```bash
make
./build.sh
```

### Compile a `.turbo` Program

Compile directly to a native executable:
```bash
./turbo hello.turbo -o hello
./hello
```

Or emit LLVM IR (`.ll`):
```bash
./turbo hello.turbo -o hello.ll
```

---

## 📊 Performance: Turbo vs Python 3 vs C (`clang -O2`)

Benchmark suite (`perf/*.turbo`) comparing Python 3.14, Turbo (`turbo.c` → LLVM IR → `clang -O2`), and hand-written C (`perf/*.c` with `clang -O2`).

| Benchmark | Python 3 (ms) | Turbo (ms) | C `clang -O2` (ms) | Turbo vs Python | Turbo vs C |
|---|---:|---:|---:|---:|---:|
| `ackermann` | 2,856.49 | **18.74** | 16.51 | **152.4×** | 0.88× |
| `class_hierarchy` | 122.45 | **18.28** | 17.25 | **6.7×** | 0.94× |
| `collatz` | 9,947.40 | **209.60** | 208.16 | **47.5×** | 0.99× |
| `dict_manip` | 105.76 | **24.58** | 18.12 | **4.3×** | 0.74× |
| `fib_iterative` | 10,702.81 | **15.47** | 16.88 | **691.7×** | **1.09×** |
| `fib_recursive` | 9,899.13 | **10.73** | 329.88 | **922.5×** | **30.74×** |
| `leibniz` | 82,406.10 | **1,462.16** | 1,113.55 | **56.4×** | 0.76× |
| `matmult` | 10,210.05 | **47.19** | 10.66* | **216.4×** | 0.23×* |
| `pi_wallis` | 512.91 | **12.89** | 13.60 | **39.8×** | **1.06×** |
| `set_ops` | 139.75 | **46.81** | 58.43 | **3.0×** | **1.25×** |
| `sieve` | 269.43 | **19.81** | 19.87 | **13.6×** | **1.00×** |
| `tree_structures` | 281.98 | **25.96** | 18.63 | **10.9×** | 0.72× |

*\*Note: `perf/matmult.c` frees `C` without reading it, so `clang -O2` dead-code eliminates the multiplication loop (`10.66 ms`). Turbo executes the full loop-interchanged 512×512 multiplication in `47.19 ms` (216.4× faster than Python 3).*
