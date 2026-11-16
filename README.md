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
* **Single-File C11 Compiler**: `stricta.c` compiles in milliseconds with zero external dependencies and emits LLVM IR linked against a lean typed C runtime (`runtime.h` / `runtime.c`), with `@extern` C FFI and `owner/repo` package imports (used by `lucianmarin/strlib` and `lucianmarin/fetch`).

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

## 📊 Performance: Stricta vs Python 3 vs C (`clang -O2`)

Benchmark suite (`perf/*.str`) comparing Python 3.14, memory-safe Stricta (`stricta.c` → LLVM IR → `clang -O2`), and hand-written C (`perf/*.c` with `clang -O2`).

| Benchmark | Python 3 (ms) | Stricta (ms) | C `clang -O2` (ms) | Stricta vs Python | Stricta vs C |
|---|---:|---:|---:|---:|---:|
| `ackermann` | 1,524.28 | **13.01** | 14.25 | **117.2×** | **1.10×** |
| `class_hierarchy` | 128.21 | **15.70** | 13.37 | **8.2×** | 0.85× |
| `collatz` | 5,099.28 | **101.16** | 100.49 | **50.4×** | 0.99× |
| `dict_manip` | 61.39 | **21.29** | 20.08 | **2.9×** | 0.94× |
| `fib_iterative` | 5,432.91 | **14.45** | 7.74 | **376.0×** | 0.54× |
| `fib_recursive` | 5,174.57 | **14.10** | 161.02 | **367.0×** | **11.42×** |
| `leibniz` | 42,308.92 | **756.30** | 529.69 | **55.9×** | 0.70× |
| `matmult` | 5,450.76 | **29.93** | 9.42* | **182.1×** | 0.31×* |
| `pi_wallis` | 274.40 | **13.72** | 12.80 | **20.0×** | 0.93× |
| `set_ops` | 79.72 | **27.84** | 32.56 | **2.9×** | **1.17×** |
| `sieve` | 146.72 | **18.15** | 19.40 | **8.1×** | **1.07×** |
| `tree_structures` | 160.71 | **23.14** | 19.92 | **6.9×** | 0.86× |

*\*Note: `perf/matmult.c` frees `C` without reading it, so `clang -O2` dead-code eliminates the multiplication loop (`9.42 ms`). Stricta executes the full loop-interchanged 512×512 multiplication in `29.93 ms` (182.1× faster than Python 3).*

**Memory Safety Performance Impact**: Compared against Stricta's prior unmanaged baseline (no drops, no bounds traps, `setjmp`/`longjmp` exceptions) on the same machine, adopting compile-time ownership/borrow checking, deterministic `@_drop_T` cleanup, bounds traps, and compiler-lowered `{ i1, T }` error propagation runs at **1.00× parity (~0.5% faster overall: `1,049.26 ms` vs `1,054.09 ms` across the 11 comparable benchmarks)**—gaining zero-leak memory safety with zero ARC or GC runtime overhead (`fib_iterative` +23%, `dict_manip` +12%, `pi_wallis` +53% from bounds-check elimination and stack-allocated drops).

---

## 🔒 Python Aliasing Differences

To guarantee memory safety without ARC or GC, Stricta enforces single ownership at compile time, which intentionally breaks Python's implicit reference aliasing in four situations:
1. **Moves on transfer**: Moving an owned value (`b = a` or passing it to a consuming function/container) invalidates `a` until reassigned; use `.copy()` to duplicate.
2. **Explicit `.copy()` / `.pop()` from borrows**: Reading `obj.field`, `lst[i]`, `d[k]`, or a global yields a borrowed view—storing or returning it as an owned value requires `.copy()` (or `.pop()`).
3. **No mutation while borrowed**: You cannot mutate or move a container/object while an active borrowed view or `for x in xs:` loop still references it.
4. **No shared multi-owner graphs**: Patterns that store two owning pointers to the same heap node (such as pushing tree node pointers onto a `path` list while the tree owns them) must use `.copy()` or integer indices instead.

See [SPEC.md](SPEC.md) for the full language specification.
