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

## 📊 Performance: Stricta vs Python 3 vs C (`clang -O2`) vs Rust (`rustc -C opt-level=2`)

Benchmark suite (`perf/*.str`) comparing Python 3.14, memory-safe Stricta (`stricta.c` → LLVM IR → `clang -O2`), hand-written C (`perf/*.c` with `clang -O2`), and idiomatic Rust (`perf/*.rs` with `rustc 1.99.1 -C opt-level=2`).

| Benchmark | Stricta (ms) | Python 3 (ms, ×) | C `clang -O2` (ms, ×) | Rust `-O2` (ms, ×) |
|---|---:|---:|---:|---:|
| `ackermann` | **7.65** | 1,446.32 (**188.9×**) | 8.54 (**1.12×**) | 10.51 (**1.37×**) |
| `class_hierarchy` | **7.54** | 63.93 (**8.5×**) | 8.29 (**1.10×**) | 9.61 (**1.27×**) |
| `collatz` | **98.18** | 4,991.55 (**50.8×**) | 98.96 (**1.01×**) | 101.71 (**1.04×**) |
| `dict_manip` | **12.22** | 57.95 (**4.7×**) | 12.14 (0.99×) | 17.71 (**1.45×**) |
| `fib_iterative` | **8.30** | 5,229.56 (**630.3×**) | 7.15 (0.86×) | 8.49 (**1.02×**) |
| `fib_recursive` | **157.50** | 5,139.58 (**32.6×**) | 158.58 (**1.01×**) | 160.06 (**1.02×**) |
| `leibniz` | **526.92** | 42,341.75 (**80.4×**) | 526.15 (1.00×) | 548.39 (**1.04×**) |
| `matmult` | **24.75** | 5,358.41 (**216.5×**) | 8.37* (0.34×*) | 116.26 (**4.70×**) |
| `pi_wallis` | **7.53** | 270.66 (**35.9×**) | 7.50 (1.00×) | 9.52 (**1.26×**) |
| `set_ops` | **21.66** | 76.65 (**3.5×**) | 28.97 (**1.34×**) | 43.16 (**1.99×**) |
| `sieve` | **13.68** | 144.53 (**10.6×**) | 13.42 (0.98×) | 16.09 (**1.18×**) |
| `tree_structures` | **14.03** | 149.50 (**10.7×**) | 13.98 (1.00×) | 16.01 (**1.14×**) |
| **Total** | **899.96** | 65,270.39 (**72.5×**) | 892.05* (0.99×*) | 1,057.52 (**1.18×**) |

*\*Note: `perf/matmult.c` frees `C` without reading it, so `clang -O2` dead-code eliminates the multiplication loop (`8.37 ms`). Stricta executes the full loop-interchanged 512×512 multiplication in `24.75 ms` (`216.5×` faster than Python 3, `4.70×` faster than `perf/matmult.rs`).*

**Memory Safety Performance Impact**: Compared against Stricta's prior unmanaged baseline (no drops, no bounds traps, `setjmp`/`longjmp` exceptions) on the same machine, adopting compile-time ownership/borrow checking, deterministic `@_drop_T` cleanup, bounds traps, and compiler-lowered `{ i1, T }` error propagation runs at **1.00× parity (~0.5% faster overall: `1,049.26 ms` vs `1,054.09 ms` across the 11 comparable benchmarks)**—gaining zero-leak memory safety with zero ARC or GC runtime overhead (`fib_iterative` +23%, `dict_manip` +12%, `pi_wallis` +53% from bounds-check elimination and stack-allocated drops).

---

## 🔒 Python Aliasing Differences

To guarantee memory safety without ARC or GC, Stricta enforces single ownership at compile time, which intentionally breaks Python's implicit reference aliasing in four situations:
1. **Moves on transfer**: Moving an owned value (`b = a` or passing it to a consuming function/container) invalidates `a` until reassigned; use `.copy()` to duplicate.
2. **Explicit `.copy()` / `.pop()` from borrows**: Reading `obj.field`, `lst[i]`, `d[k]`, or a global yields a borrowed view—storing or returning it as an owned value requires `.copy()` (or `.pop()`).
3. **No mutation while borrowed**: You cannot mutate or move a container/object while an active borrowed view or `for x in xs:` loop still references it.
4. **No shared multi-owner graphs**: Patterns that store two owning pointers to the same heap node (such as pushing tree node pointers onto a `path` list while the tree owns them) must use `.copy()` or integer indices instead.

See [SPEC.md](SPEC.md) for the full language specification.
