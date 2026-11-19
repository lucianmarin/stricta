# Stricta Programming Language

Stricta is a **strict, safe, compiled version of Python** (`.str` extension)—an ahead-of-time statically typed language with Python syntax that compiles directly to native machine code via a single-file C11 compiler (`stricta.c`), LLVM IR (`.ll`), and `clang -O2`, with **zero dynamic fallback or NaN-boxing**.

---

## ⚡ Philosophy & Architecture

* **Strict Static Typing**: Unlike standard Python (which treats type hints as optional runtime-ignored metadata) or gradual compilers (which fall back to dynamic objects), Stricta enforces static types at compile time on every parameter, class field, container, and binding (`int`/`i64`, `i8`–`i64`, `u8`–`u64`, `float`/`f64`, `f32`, `bool`, `str`, `bytes`, `bigint`, `list[T]`, `set[T]`, `dict[K, V]`, `tuple[...]`, `T | None`, `Callable`, `@dataclass`, `class`, `ABC`), with call-site monomorphization, cross-method field inference, flow-sensitive narrowing (`is None`, `isinstance`, `and`/`or`, `elif`, `assert`, `match`), range-based bounds-check elimination, and immediate `file:line:col` errors on untyped or mismatched code.
* **Rust-Style Memory Safety (No GC / No ARC)**: Compile-time ownership and borrow checking with inferred `BORROW` / `MUT_BORROW` / `MOVE` parameter modes, non-lexical lifetimes (NLL), monomorphized deterministic `@_drop_T` / `@_clone_T` generation, compiler-lowered `{ i1, T }` exception propagation, and runtime bounds traps—with zero garbage collection or reference counting overhead.
* **Zero-Overhead Data Structures & OOP**:
  * Classes lower to flat LLVM structs with direct `getelementptr` field offsets (zero string-keyed attribute dicts).
  * Non-recursive `@dataclass`es use value semantics passed/returned directly in registers.
  * `list[T]`, `set[T]`, and `dict[K, V]` are monomorphized in LLVM IR with unboxed element storage, inline open-addressing hash tables, and stack-backed non-escaping local lists and comprehensions.
  * Polymorphic hierarchies and cooperative diamond `super()` dispatch via `type_id` switches, with `O(1)` bitmask `isinstance()` checks.
  * `range()` loops, `dict.items()`/`values()` loops, comprehensions, and `sum`/`any`/`all(genexpr)` / matrix multiplication lower directly to native loops.
* **LLVM Coroutines (`async`/`await`)**: `async def` functions lower directly to LLVM switched-resume coroutines (`presplitcoroutine` + `llvm.coro.*`) rather than OS threads, giving zero-overhead stackless concurrency that pairs with Python-style event loops (`asyncio` in `strlib`).
* **Single-File C11 Compiler**: `stricta.c` compiles in milliseconds with zero external dependencies and emits self-contained LLVM IR linked directly against libc (`clang -O2`), with `@extern` C FFI (`.bin`/`unbin`, `addr[T]`) and `owner/repo` package imports (used by `lucianmarin/strlib` and `lucianmarin/fetch`).

---

## 🚀 Quick Start

### Install with Homebrew

```bash
brew install --HEAD lucianmarin/tap/stricta
```

### Prerequisites (Building from Source)
* `clang` (with LLVM IR support) and `make`.

### Build & Run All Tests

```bash
make
make test
```

### Compile a `.str` Program

Compile directly to a native executable:
```bash
./stricta hello.str -o hello
./hello
```

Or emit LLVM IR (`.ll`):
```bash
./stricta hello.str -o hello.ll
```

---

## 📦 Packages & Standard Library

Stricta's ecosystem centers on two companion projects:

* **[fetch](https://github.com/lucianmarin/fetch)** — Decentralized package manager written in Stricta that uses GitHub repositories and Git tags/SHAs directly via `package.lmon` manifests (no central registry):
  ```bash
  fetch init myapp
  fetch add lucianmarin/strlib
  fetch build main.str -o myapp
  ```
* **[strlib](https://github.com/lucianmarin/strlib)** — Standard library for Stricta written in Stricta using `@extern` C bindings, providing `sys`, `os`, `time`, `date`, `datetime`, `lmon`, `json`, `http`, `math`, `re`, `path`, and `hashlib`:
  ```python
  from strlib import os, sys, datetime
  ```

---

## 📊 Performance: Stricta vs Python 3 vs C (`clang -O2`) vs Rust (`-O2`) vs Nim (`-O2`) vs Swift (`-O`)

Benchmark suite (`perf/*.str`, `perf/*.c`, `perf/*.rs`, `perf/*.nim`, `perf/*.swift`) comparing Python 3.14, memory-safe Stricta (`stricta.c` → LLVM IR → `clang -O2`), hand-written C (`clang -O2`), idiomatic Rust (`rustc 1.100.0 -C opt-level=2`), idiomatic Nim (`nim 2.4.2` with `-d:release --opt:speed --mm:orc`; LLVM machine outliner disabled to keep the `fib_recursive` code layout stable), and idiomatic Swift (`swiftc 6.4.2 -O`). All tests are work-preserving: results are returned, printed, or checksummed so the timed work cannot be deleted by an optimizing compiler. Medians are 5–7 native runs and 2–3 Python runs on an Apple M1 (macOS 27.2.1).

| Benchmark | Stricta (ms) | Python 3 (ms, ×) | C `clang -O2` (ms, ×) | Rust `-O2` (ms, ×) | Nim `-O2` (ms, ×) | Swift `-O` (ms, ×) |
|---|---:|---:|---:|---:|---:|---:|
| `ackermann` | **176.89** | 1,527.32 (**8.63×**) | 176.23 (**1.00×**) | 174.85 (**0.99×**) | 206.02 (**1.16×**) | 191.57 (**1.08×**) |
| `async_tasks` | **10.47** | 1,069.54 (**102.2×**) | 10.99 (**1.05×**) | 9.50 (**0.91×**) | 10.36 (**0.99×**) | 421.73 (**40.3×**) |
| `class_hierarchy` | **6.48** | 53.04 (**8.19×**) | 6.31 (**0.97×**) | 10.85 (**1.67×**) | 13.58 (**2.10×**) | 18.76 (**2.90×**) |
| `collatz` | **102.28** | 5,292.30 (**51.7×**) | 107.72 (**1.05×**) | 110.74 (**1.08×**) | 186.20 (**1.82×**) | 164.26 (**1.61×**) |
| `dict_manip` | **13.07** | 53.60 (**4.10×**) | 10.14 (**0.78×**) | 14.10 (**1.08×**) | 14.55 (**1.11×**) | 24.02 (**1.84×**) |
| `fib_iterative` | **6.06** | 5,689.39 (**938.8×**) | 3.93 (**0.65×**) | 7.91 (**1.31×**) | 10.65 (**1.76×**) | 11.99 (**1.98×**) |
| `fib_recursive` | **177.28** | 4,836.16 (**27.3×**) | 183.33 (**1.03×**) | 184.27 (**1.04×**) | 610.84 (**3.45×**) | 313.56 (**1.77×**) |
| `leibniz` | **594.55** | 44,617.83 (**75.0×**) | 644.40 (**1.08×**) | 619.22 (**1.04×**) | 821.94 (**1.38×**) | 573.69 (**0.96×**) |
| `matmult` | **25.69** | 5,611.90 (**218.4×**) | 113.03 (**4.40×**) | 120.26 (**4.68×**) | 126.74 (**4.93×**) | 157.34 (**6.12×**) |
| `pi_wallis` | **16.30** | 275.36 (**16.9×**) | 10.67 (**0.65×**) | 13.29 (**0.82×**) | 13.67 (**0.84×**) | 11.12 (**0.68×**) |
| `set_ops` | **23.24** | 73.35 (**3.16×**) | 32.76 (**1.41×**) | 47.85 (**2.06×**) | 47.69 (**2.05×**) | 43.74 (**1.88×**) |
| `sieve` | **12.85** | 142.70 (**11.1×**) | 13.36 (**1.04×**) | 15.03 (**1.17×**) | 15.65 (**1.22×**) | 12.55 (**0.98×**) |
| `tree_structures` | **17.52** | 152.82 (**8.72×**) | 16.83 (**0.96×**) | 15.16 (**0.87×**) | 18.38 (**1.05×**) | 40.80 (**2.33×**) |
| **Total** | **1,182.68** | 69,395.31 (**58.7×**) | 1,329.70 (**1.12×**) | 1,343.03 (**1.14×**) | 2,096.27 (**1.77×**) | 1,985.13 (**1.68×**) |

*`perf/ackermann.py` is the Python version of the `ackermann` test; the Stricta source uses `@extern("getpid")` for runtime opacity. `perf/leibniz.c` loops `10^9` times while the other implementations loop `10^9 + 1`, so its printed value differs in the last digits. Stricta's `matmult` is faster because its lowering interchanges the i-j-k loop nest for cache locality; all implementations compute and checksum the full 512×512 product.*

*Python's `fib_iterative` uses arbitrary-precision integers (the result has ~164k digits), while the compiled implementations wrap at 64 bits; that semantics difference accounts for its 2623.9× ratio. Nim's default release build keeps runtime checks; `-d:danger` reduces its total to about `1,926 ms` (1.13× Stricta).*

---

## 🔒 Python Aliasing Differences

To guarantee memory safety without ARC or GC, Stricta enforces single ownership at compile time, which intentionally breaks Python's implicit reference aliasing in four situations:
1. **Moves on transfer**: Moving an owned value (`b = a` or passing it to a consuming function/container) invalidates `a` until reassigned; use `.copy()` to duplicate.
2. **Explicit `.copy()` / `.pop()` from borrows**: Reading `obj.field`, `lst[i]`, `d[k]`, or a global yields a borrowed view—storing or returning it as an owned value requires `.copy()` (or `.pop()`).
3. **No mutation while borrowed**: You cannot mutate or move a container/object while an active borrowed view or `for x in xs:` loop still references it.
4. **No shared multi-owner graphs**: Patterns that store two owning pointers to the same heap node (such as pushing tree node pointers onto a `path` list while the tree owns them) must use `.copy()` or integer indices instead.

See [SPEC.md](SPEC.md) for the full language specification.
