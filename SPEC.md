# Stricta Language Specification

Stricta is a **strict, safe, compiled version of Python** (`.str` file extension). It uses Python syntax while enforcing strict ahead-of-time static typing and compile-time memory safety with zero dynamic fallback. The single-file C11 compiler (`stricta.c`) lexes, parses, type-checks, and lowers `.str` programs directly to self-contained LLVM IR (`.ll`), which `clang -O2` compiles and links against libc into native executables with no separate C runtime file.

---

## 1. Type System

Stricta enforces strict static types on all bindings, parameters, fields, and containers with zero dynamic NaN-boxing fallback:

### 1.1 Built-in Types
- **Signed Integers**: `int` (alias for `i64`), `i8`, `i16`, `i32`, `i64`
- **Unsigned Integers**: `u8`, `u16`, `u32`, `u64`
- **Floating-Point**: `float` (alias for `f64`), `f32`, `f64`
- **Boolean**: `bool` (`True`, `False`)
- **Strings & Bytes**: `str` (`%StrictaStr*` UTF-8 immutable string), `bytes` (`%StrictaBytes*`)
- **Arbitrary-Precision Integer**: `bigint` (`%StrictaBigInt*`)
- **File Handle**: `File`
- **None / Void**: `None` (return type `-> None`, or nullable value in `T | None` / `Optional[T]`)

### 1.2 Composite & Generic Types
- `list[T]` (`List[T]`): Monomorphized contiguous dynamic array (`%List_T`).
- `set[T]` (`Set[T]`): Monomorphized open-addressing hash set (`%Set_T`).
- `dict[K, V]` (`Dict[K, V]`): Monomorphized dense open-addressing hash table (`%Dict_K_V`).
- `tuple[T1, T2, ...]` (`Tuple[...]`): Unboxed LLVM value struct (`%Tuple_...`).
- `T | None` (`Optional[T]`): Nullable pointer for reference types (`str`, classes, containers, pointers), or `{ i8, T }` tagged struct for value types.
- `Callable[[ArgTypes...], RetType]`: Typed function pointer.
- `Task[T]`: Stackless LLVM coroutine handle (`presplitcoroutine` + `llvm.coro.*`) returned by `async def` functions and consumed via `await`.
- **Safe References & Raw Addresses** *(Stricta extensions beyond Python)*:
  - `ref[T]` (also written `ref[const T]`): Non-null shared immutable reference to a `T`.
  - `mut_ref[T]` (also written `ref[mut T]`): Non-null exclusive mutable reference to a `T`.
  - `addr[T]` (also written `addr[const T]` / `addr[mut T]`): Unchecked C-style address/pointer to a `T`.

### 1.3 Types Beyond Standard Python

Stricta adds several zero-overhead static types that do not exist in standard Python:

| Type | Constructors / Syntax | Description & Semantics |
| :--- | :--- | :--- |
| `i8`, `i16`, `i32`, `i64` | `i8(x)`, `i16(x)`, `i32(x)`, `i64(x)` | Fixed-width signed integers (`int` is an alias for `i64`). Widening (`i8` → `i16` → `i32` → `i64`) is implicit; narrowing uses explicit conversion functions. |
| `u8`, `u16`, `u32`, `u64` | `u8(x)`, `u16(x)`, `u32(x)`, `u64(x)` | Fixed-width unsigned integers with unsigned LLVM arithmetic/comparisons. Widening (`u8` → `u16` → `u32` → `u64`) is implicit. |
| `f32`, `f64` | `f32(x)`, `f64(x)` | IEEE-754 single-precision (`float` in LLVM) and double-precision (`double`, `float` alias) floats. |
| `bigint` | `bigint(x)` | Heap-allocated arbitrary-precision signed integer (`+`, `-`, `*`, `//`, `%`, `**`, `abs`, comparisons, `str`). |
| `File` | `open(path, mode="r")` | Owned file handle supporting `.read()`, `.write()`, `.close()`, and `with open(...) as f:`. |
| `Task[T]` | `async def` + `await` | Owned LLVM coroutine handle (`presplitcoroutine`); takes ownership (`MOVE`) of owned arguments, suspends cooperatively at `await` / `_coro_yield()`, steps via `_task_step()` / `_task_is_done()` for event loops (`asyncio` in `strlib`), returns `T` on `await`, and destroys/drops locals deterministically via `llvm.coro.destroy` if un-awaited. |
| `ref[T]` | `x.ref` | Non-null shared immutable reference checked by the borrow checker (multiple `ref[T]` allowed; no mutation while live; cannot outlive local stack/owner). |
| `mut_ref[T]` / `ref[mut T]` | `x.mut_ref` | Non-null exclusive mutable reference (move-only on assignment, reborrowed across calls; forbids aliasing or accessing the owner while live; cannot outlive local stack/owner). |
| `addr[T]` | `x.addr` | Raw address/pointer (`addr[const T]` / `addr[mut T]` accepted). Not borrow-checked and storable in class fields and containers (`list[addr[T]]`), convertible to integers via `int(p)` / `u64(p)` and formatted as hex via `str(p)` / `hex(p)`. |

#### Pointer & Reference Operations
- **Taking References / Addresses**: `.ref`, `.mut_ref`, and `.addr` work on any addressable place (variables, class/`@dataclass` fields, list elements `xs[i]`, and dereferenced pointers `val(p)`). Taking `.mut_ref` requires a mutable place (rejected through `ref[T]`).
- **Dereferencing (`val`)**: `val(p)` reads the target value (or `val(level, p)` for multi-level dereferencing, e.g. `val(2, pp)`). For mutable references (`mut_ref[T]`, `addr[T]`), `val(p) = expr` and `val(p) += expr` mutate the target in place (dropping any previous owned value before overwriting). Moving an owned `T` out of `val(p)` is rejected; use `val(p).copy()`.
- **Unified `.` Access**: Fields, methods, `@property` getters, subscripts (`r[i]`, `m[i] = v`), slicing, and `len(r)` auto-dereference through `ref[T]`, `mut_ref[T]`, and `addr[T]` (including by-value `@dataclass` types). Mutating field/method/index operations are rejected through `ref[T]`.
- **Nullable Pointers & Coercions**: `ref[T] | None`, `mut_ref[T] | None`, and `addr[T] | None` compile to zero-cost nullable `ptr` values and narrow with `is None` / `is not None`. `mut_ref[T]` coerces implicitly to `ref[T]`, and both `ref[T]` and `mut_ref[T]` coerce implicitly to `addr[T]`.
- **Safety Boundaries**: `ref[T]` and `mut_ref[T]` cannot be returned when pointing to local stack storage and cannot be stored in class fields or heap containers (`list`, `set`, `dict`); use `addr[T]` when raw pointer storage is needed.

### 1.4 Strict Typing & Inference Rules
- Function parameters support explicit annotations (`def add(a: int, b: int) -> int:`), default-value inference (`def f(x = 0)`), and **call-site monomorphization** for unannotated parameters (`def first(xs): return xs[0]`), which specializes a typed LLVM function per distinct call-site argument signature. Parameters with default `None`, `[]`, `{}`, or `set()` still require an explicit annotation (`x: T | None = None`).
- Functions with an explicit `-> RetType` enforce it strictly (including **definite-return checking** on all control-flow paths); functions omitting `-> RetType` infer their static return type (`T`, `T | None`, `list[T]` for generators, or `None` when returning nothing) from their `return` / `yield` statements.
- Local variables and module globals infer their static type deterministically from initializers (`x = 10` → `int`, `ORIGIN = Vec2D(0, 0)` → `Vec2D`), from deferred first-use operations, calls, or returns on empty containers (`items = []; items.append(1)` → `list[int]`, `counts = {}; counts["a"] = 1` → `dict[str, int]`, `seen = set(); seen.add("x")` → `set[str]`), or optional auto-promotion (`best = None; best = node` → `Node | None`, `[None, 10]` → `list[int | None]`).
- Unpopulated empty `[]`, `{}`, `set()`, and bare `None` never promoted in scope are rejected at compile time with `file:line:col: error: ...`.
- Flow-sensitive narrowing refines `T | None` to `T` and `Base` to `Sub` across `is None` / `is not None`, `isinstance(x, Sub)`, `and` / `or` / `not` compound guards, `elif` chains, `assert` statements, early-return guards, and `match` / `case None:` arms.
- Bidirectional inference propagates expected types (including `Callable`, `T | None`, ternaries, comprehensions, and `map`/`filter`/`sorted(..., key=...)`) top-down, while escape and range analysis stack-allocate non-escaping locals, comprehensions, and classes and emit direct bounds-check-free `getelementptr` indexing when `0 <= i < len(arr)` is statically proved.

---

## 2. Classes, Dataclasses, and OOP

- **Class Structs**: Fields are declared at class scope (`key: int`, `left: BSTNode | None = None`) or inferred from assignments to `self.field` across `__init__` and class methods (including `T | None` auto-promotion and empty-container field inference). Fields map to fixed LLVM struct offsets.
- **`@dataclass`**: Automatically supports positional/default field construction and structural equality (`==`). Non-recursive `@dataclass` types compile as unboxed by-value structs in registers.
- **Inheritance & Mixins**: Supports single and multiple inheritance with C3 MRO linearization, `ABC` / `@abstractmethod` enforcement, cooperative `super().method(...)` dispatch, `@property`, `@staticmethod`, `@classmethod`, operator overloading (`__add__`, `__eq__`, `__str__`, `__repr__`, etc.), and bitmask `isinstance(obj, Class)`.

---

## 3. Control Flow, Comprehensions, and Modules

- **Statements**: `if`/`elif`/`else`, `while`/`else`, `for`/`else`, `match`/`case`, `try`/`except [Type [as e]]`/`else`/`finally`, `raise`, `assert`, `with ... as ...:`, `del`, `global`, `nonlocal`, `yield`/`yield from` (eager `list[T]`), `lambda`, custom `@decorator`.
- **Comprehensions**: `[expr for ...]`, `{expr for ...}`, `{k: v for ...}`, `(expr for ...)` (fused directly into zero-allocation loops for `sum(...)`, `any(...)`, and `all(...)`, and stack-allocated when non-escaping).
- **Modules & Packages**: `import mod [as alias]`, `from mod import name [as alias]`, and `from owner/repo import mod` resolving `.str` modules and `pkg/__init__.str` packages across the local directory, `-I <dir>`, `.stricta/pkgs/`, and `STRICTA_PATH`, plus `@extern("c_sym")` C-FFI bindings, `abc`, `dataclasses`, and `typing`. Standard library modules (`sys`, `os`, `time`, `date`, `datetime`) live in the companion `lucianmarin/strlib` package.

---

## 4. Memory Safety, Ownership, and Error Propagation

Stricta provides **compile-time Rust-style memory safety without garbage collection (GC) or automatic reference counting (ARC)**:

- **Single Ownership & Deterministic Drops**: Every heap-allocated value (`str`, `bytes`, `bigint`, `File`, `list[T]`, `set[T]`, `dict[K, V]`, owned `tuple`, and non-value `class` instances) has a single owning binding, field, or container slot. The compiler emits monomorphized `@_drop_<T>` and `@_clone_<T>` helpers, tracks runtime drop flags (`alloca i1`) for conditionally moved/initialized variables, flushes statement temporaries, and drops all alive locals on normal return and exception unwinding. Static string/bytes literals carry an `is_static` flag so drops are zero-cost no-ops.
- **Inferred Parameter Modes & Non-Lexical Lifetimes (NLL)**: Function parameters default to `BORROW` (read-only), upgrade automatically to `MUT_BORROW` when mutated, or `MOVE` when stored/returned/consumed—without requiring lifetime annotations in Python syntax. Local views created by field access (`obj.field`), indexing (`lst[i]`), or iteration (`for x in xs:`) borrow their owner until their last lexical use (NLL), rejecting mutation or moves of the owner while the borrow is live, as well as double mutable borrows (`f(x, x)`).
- **Compiler-Lowered Error Propagation**: Functions that may `raise` lower to `{ i1 is_exc, T val }` (or `i1` for `-> None`) returns instead of `setjmp`/`longjmp`, guaranteeing deterministic stack unwinding and drop execution across `try`/`except`/`else`/`finally` and `with` blocks. Container indexing (`list`, `bytes`, `str`) traps via `stricta_bounds_fail` whenever `0 <= i < len(arr)` is not statically proved.

### 4.1 When Stricta Breaks Python Aliasing

Because values have a single owner and cannot be implicitly aliased across mutable slots, a few dynamic Python idioms become compile-time ownership errors and require `.copy()` or index-based access:

1. **Assigning or Passing a Variable Moves Ownership**: If `b = a` or `consume(a)` moves an owned variable `a`, reading `a` afterward is rejected (`use of moved value 'a'`). Use `b = a.copy()` to keep both.
2. **No Moving Out of Borrowed Fields, Indices, or Globals**: Extracting an owned value from `obj.field`, `lst[i]`, `d[k]`, a borrowed parameter, or a module global into an owning sink (e.g. `return obj.name`, `xs.append(other[0])`) is rejected (`cannot move out of borrowed ...`). Use `.copy()` (or `lst.pop(i)` / `d.pop(k)` to transfer ownership out of a container).
3. **No Mutating or Moving While Borrowed / Iterating**: Holding a borrowed view (e.g. `v = box.items` or `for x in xs:`) prevents mutating or reassigning `box` / `xs` until after the last use of `v` or after the loop ends.
4. **No Shared Mutable Cyclic/DAG Object Graphs**: Patterns that store the same heap node pointer in two owning places at once—for example, recording a traversal path of `TreeNode` pointers via `path.append(curr)` while the tree still owns `curr` (as in `perf/tree_structures.str`)—are rejected. Represent shared references with indices/IDs or clone explicitly with `.copy()`.

---

## 5. Compiler Architecture

- **`stricta.c`**: Single-file C11 compiler containing the arena allocator, indentation lexer, recursive-descent parser, strict semantic/type analyzer, ownership/borrow checker, monomorphization registry, and self-contained LLVM IR code generator (emitting runtime primitives for UTF-8 strings, `bytes`, `bigint`, `File` I/O, thread-local exceptions, bounds traps, and LLVM coroutines directly against libc, with zero NaN-boxing, ARC, or GC).
- **Build & Test**:
  ```bash
  make       # builds ./stricta from stricta.c
  make test  # runs test.str and all tests/*.str
  ```
