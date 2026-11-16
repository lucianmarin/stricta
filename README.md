# Stricta Programming Language

Stricta is a **strict compiled version of Python** (`.str` extension)—an ahead-of-time statically typed language with Python syntax that compiles directly to native machine code via a single-file C11 compiler (`stricta.c`), LLVM IR (`.ll`), and `clang -O2`, with **zero dynamic fallback or NaN-boxing**.

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
* **Single-File C11 Compiler**: `stricta.c` compiles in milliseconds with zero external dependencies and emits LLVM IR linked against a lean typed C runtime (`runtime.h` / `runtime.c`).

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

Benchmark suite (`perf/*.str`) comparing Python 3.14, Stricta (`stricta.c` → LLVM IR → `clang -O2`), and hand-written C (`perf/*.c` with `clang -O2`).

| Benchmark | Python 3 (ms) | Stricta (ms) | C `clang -O2` (ms) | Stricta vs Python | Stricta vs C |
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

*\*Note: `perf/matmult.c` frees `C` without reading it, so `clang -O2` dead-code eliminates the multiplication loop (`10.66 ms`). Stricta executes the full loop-interchanged 512×512 multiplication in `47.19 ms` (216.4× faster than Python 3).*

---

## 🔒 Python Aliasing Differences

To guarantee memory safety without ARC or GC, Stricta enforces single ownership at compile time, which intentionally breaks Python's implicit reference aliasing in four situations:
1. **Moves on transfer**: Moving an owned value (`b = a` or passing it to a consuming function/container) invalidates `a` until reassigned; use `.copy()` to duplicate.
2. **Explicit `.copy()` / `.pop()` from borrows**: Reading `obj.field`, `lst[i]`, `d[k]`, or a global yields a borrowed view—storing or returning it as an owned value requires `.copy()` (or `.pop()`).
3. **No mutation while borrowed**: You cannot mutate or move a container/object while an active borrowed view or `for x in xs:` loop still references it.
4. **No shared multi-owner graphs**: Patterns that store two owning pointers to the same heap node (such as pushing tree node pointers onto a `path` list while the tree owns them) must use `.copy()` or integer indices instead.

See [SPEC.md](SPEC.md) for the full language specification.
