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
* **Single-File C11 Compiler**: `stricta.c` compiles in milliseconds with zero external dependencies and emits self-contained LLVM IR linked directly against libc (`clang -O2`), with `@extern` C FFI (`.bin`/`unbin`, `addr[T]`) and `owner/repo` package imports (used by `lucianmarin/strlib` and `lucianmarin/fetch`).

---

## 🚀 Quick Start

### Prerequisites
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

## 📊 Performance: Stricta vs Python 3 vs C (`clang -O2`) vs Rust (`-O2`) vs Nim (`-O2`)

Benchmark suite (`perf/*.str`, `perf/*.c`, `perf/*.rs`, `perf/*.nim`) comparing Python 3.14, memory-safe Stricta (`stricta.c` → LLVM IR → `clang -O2`), hand-written C (`clang -O2`), idiomatic Rust (`rustc 1.100.0 -C opt-level=2`), and idiomatic Nim (`nim 2.4.2` with `-d:release --opt:speed --mm:orc`; LLVM machine outliner disabled to keep the `fib_recursive` code layout stable). All tests are work-preserving: results are returned, printed, or checksummed so the timed work cannot be deleted by an optimizing compiler. Medians are 5–7 native runs and 2–3 Python runs on an Apple M1 (macOS 27.2.1).

| Benchmark | Stricta (ms) | Python 3 (ms, ×) | C `clang -O2` (ms, ×) | Rust `-O2` (ms, ×) | Nim `-O2` (ms, ×) |
|---|---:|---:|---:|---:|---:|
| `ackermann` | **192.10** | 2,977.34 (**15.5×**) | 192.48 (**1.00×**) | 200.86 (**1.05×**) | 259.84 (**1.35×**) |
| `class_hierarchy` | **2.81** | 74.02 (**26.3×**) | 2.44 (**0.87×**) | 3.14 (**1.12×**) | 3.06 (**1.09×**) |
| `collatz` | **134.94** | 7,348.60 (**54.5×**) | 134.03 (**0.99×**) | 135.01 (**1.00×**) | 199.54 (**1.48×**) |
| `dict_manip` | **8.88** | 72.78 (**8.20×**) | 8.25 (**0.93×**) | 13.93 (**1.57×**) | 18.24 (**2.05×**) |
| `fib_iterative` | **2.84** | 7,451.76 (**2623.9×**) | 2.72 (**0.96×**) | 2.98 (**1.05×**) | 2.82 (**0.99×**) |
| `fib_recursive` | **307.47** | 11,286.88 (**36.7×**) | 306.56 (**1.00×**) | 306.43 (**1.00×**) | 608.93 (**1.98×**) |
| `leibniz` | **963.61** | 72,676.92 (**75.4×**) | 992.73 (**1.03×**) | 965.84 (**1.00×**) | 1,282.75 (**1.33×**) |
| `matmult` | **27.91** | 7,186.09 (**257.5×**) | 153.13 (**5.49×**) | 139.89 (**5.01×**) | 159.00 (**5.70×**) |
| `pi_wallis` | **10.78** | 399.59 (**37.1×**) | 10.80 (**1.00×**) | 11.09 (**1.03×**) | 11.08 (**1.03×**) |
| `set_ops` | **25.01** | 97.41 (**3.89×**) | 35.55 (**1.42×**) | 58.46 (**2.34×**) | 57.48 (**2.30×**) |
| `sieve` | **11.49** | 240.88 (**21.0×**) | 11.63 (**1.01×**) | 11.97 (**1.04×**) | 10.75 (**0.94×**) |
| `tree_structures` | **9.82** | 220.04 (**22.4×**) | 9.22 (**0.94×**) | 9.23 (**0.94×**) | 13.47 (**1.37×**) |
| **Total** | **1,697.67** | 110,032.30 (**64.8×**) | 1,859.55 (**1.10×**) | 1,858.83 (**1.09×**) | 2,626.95 (**1.55×**) |

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
