# Stricta Language Specification

Stricta is a **superstrict compiled version of Python** (`.str` file extension). It uses Python syntax while enforcing strict ahead-of-time static typing with zero dynamic fallback. The single-file C11 compiler (`stricta.c`) lexes, parses, type-checks, and lowers `.str` programs directly to LLVM IR (`.ll`), which `clang -O2` compiles and links with the lean C runtime (`runtime.c`) into native executables.

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
- `T | None` (`Optional[T]`): Nullable pointer for reference types (`str`, classes, containers), or `{ i8, T }` tagged struct for value types.
- `Callable[[ArgTypes...], RetType]`: Typed function pointer.

### 1.3 Strict Typing & Inference Rules
- Function parameters (except `self`, `cls`, and parameters with a concrete default value like `def f(x = 0)`) require explicit type annotations: `def add(a: int, b: int) -> int:`. Parameters with default `None`, `[]`, `{}`, or `set()` still require an explicit annotation (`x: T | None = None`).
- Functions with an explicit `-> RetType` enforce it strictly; functions omitting `-> RetType` infer their static return type (`T`, `T | None`, `list[T]` for generators, or `None` when returning nothing) from their `return` / `yield` statements.
- Local variables and module globals infer their static type deterministically from initializers (`x = 10` → `int`, `ORIGIN = Vec2D(0, 0)` → `Vec2D`), or from deferred first-use operations on empty containers (`items = []; items.append(1)` → `list[int]`, `counts = {}; counts["a"] = 1` → `dict[str, int]`, `seen = set(); seen.add("x")` → `set[str]`), or optional auto-promotion (`best = None; best = node` → `Node | None`).
- Unpopulated empty `[]`, `{}`, `set()`, and bare `None` never promoted in scope are rejected at compile time with `file:line:col: error: ...`.
- Flow-sensitive narrowing refines `T | None` to `T` via `is None` / `is not None`, and refines `Base` to `Sub` inside `if isinstance(x, Sub):` blocks.
- Bidirectional inference propagates `Callable` and `map`/`filter`/`sorted(..., key=...)` parameter and return types into `lambda` expressions, while escape and range analysis stack-allocate non-escaping locals/classes and elide negative-index checks on non-negative loop indices.

---

## 2. Classes, Dataclasses, and OOP

- **Class Structs**: Fields are declared at class scope (`key: int`, `left: BSTNode | None = None`) or inferred from typed assignments to `self.field` in `__init__`. Fields map to fixed LLVM struct offsets.
- **`@dataclass`**: Automatically supports positional/default field construction and structural equality (`==`). Non-recursive `@dataclass` types compile as unboxed by-value structs in registers.
- **Inheritance & Mixins**: Supports single and multiple inheritance with C3 MRO linearization, `ABC` / `@abstractmethod` enforcement, cooperative `super().method(...)` dispatch, `@property`, `@staticmethod`, `@classmethod`, operator overloading (`__add__`, `__eq__`, `__str__`, `__repr__`, etc.), and bitmask `isinstance(obj, Class)`.

---

## 3. Control Flow, Comprehensions, and Modules

- **Statements**: `if`/`elif`/`else`, `while`/`else`, `for`/`else`, `match`/`case`, `try`/`except [Type [as e]]`/`else`/`finally`, `raise`, `assert`, `with ... as ...:`, `del`, `global`, `nonlocal`, `yield`/`yield from` (eager `list[T]`), `lambda`, custom `@decorator`.
- **Comprehensions**: `[expr for ...]`, `{expr for ...}`, `{k: v for ...}`, `(expr for ...)` (fused directly into zero-allocation loops for `sum(...)`).
- **Modules & Packages**: `import mod [as alias]`, `from mod import name [as alias]` resolving `.str` modules and `pkg/__init__.str` packages, plus `sys.argv`, `abc`, `dataclasses`, and `typing`.

---

## 4. Compiler & Runtime Architecture

- **`stricta.c`**: Single-file C11 compiler containing the arena allocator, indentation lexer, recursive-descent parser, strict semantic/type analyzer, monomorphization registry, and LLVM IR code generator.
- **`runtime.h` / `runtime.c`**: Lean C11 runtime providing UTF-8 string operations, `bytes`, `bigint`, `File` I/O, `setjmp`/`longjmp` exception stack, CLI `sys.argv`, and typed print helpers—with zero NaN-boxing or dynamic attribute lookup.
- **Build & Test**:
  ```bash
  make        # builds ./stricta from stricta.c
  ./build.sh  # runs test.str and all tests/*.str
  ```
