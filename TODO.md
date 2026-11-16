# Stricta TODO

Implementation status of Python features in Stricta (`stricta.c` + `runtime.c`).

## Types

- [x] `int` / sized integers (`i8`–`i64`, `u8`–`u64`)
- [x] `float` / `f32` / `f64` — float literal parsing (`3.14`, `2.5e2`)
- [ ] `complex` — imaginary literals (`4j`, `3+4j`) and complex arithmetic
- [x] `bytes` — `b"..."` literals and immutable byte sequences
- [ ] `bytearray` — mutable byte arrays
- [x] `tuple` — unboxed value structs and literals (`(1,)`, `(1, 2)`, `()`)
- [x] `set` — monomorphized open-addressing hash sets (`{1, 2}`)
- [ ] `frozenset` — immutable hash sets
- [x] `bigint` — arbitrary-precision integers
- [x] `T | None` (`Optional[T]`) & `Callable[[...], Ret]`

## Control Flow

- [x] `try` / `except` / `else` / `finally` — implemented via `setjmp`/`longjmp`; supports bare `except:`, typed `except ExcType:`, `except ExcType as e:`, `else`, and `finally`
- [x] `raise` — raises a catchable exception via `stricta_raise()`; prints error and exits if unhandled
- [x] `assert` — checks condition and exits on failure; evaluates and prints optional assertion message
- [x] `with` — full context manager protocol (`__enter__`, `try`/`finally` via `setjmp`/`longjmp`, `__exit__`, re-raises when `__exit__` returns false)
- [x] `async` / `await` — parsed and executed synchronously
- [ ] True async event loop / coroutines (`async def` / `await` suspension)
- [x] `match` / `case` — pattern matching supporting literal, capture, and wildcard (`_`) patterns
- [x] `for` / `else`, `while` / `else` — else clauses on loops
- [x] `del` — removes list elements and dict keys
- [x] `global` / `nonlocal` — module-level globals and nested-function variable access
- [x] `yield` / `yield from` — eager list collection (`list[T]`); `yield from` extends from sub-iterables
- [x] `lambda` — compiled to typed LLVM functions with bidirectional type inference
- [x] `@decorator` — parsed and applied to functions and classes; supports stacking
- [x] Ternary conditional: `x if cond else y`
- [x] Walrus operator: `:=`

## Operators

- [x] `**` — exponentiation
- [x] `//` — floor division
- [x] Bitwise: `&`, `|`, `^`, `~`, `<<`, `>>`
- [x] `@` — matrix multiplication on lists of lists with dimension validation
- [x] Augmented assignments: `+=`, `-=`, `*=`, `/=`, `%=`, `//=`, `**=`, `&=`, `|=`, `^=`, `>>=`, `<<=`, `@=`

## Comprehensions

- [x] List comprehensions: `[x for x in y]`
- [x] Dict comprehensions: `{k: v for k, v in x}`
- [x] Set comprehensions: `{x for x in y}`
- [x] Generator expressions: `(x for x in y)` (fused into zero-allocation loops for `sum(...)`)

## String Methods

- [x] `.split()` / `.join(iterable)` — split and join strings
- [x] `.upper()` / `.lower()` — case conversion
- [x] `.replace(old, new)` — substring replacement
- [x] `.find(sub)` / `.index(sub)` — substring search
- [x] `.strip()` / `.lstrip()` / `.rstrip()` — trim whitespace
- [x] `.startswith(prefix)` / `.endswith(suffix)` — prefix/suffix check
- [x] `.count(sub)` — count occurrences
- [x] `.format(*args)` — string formatting (including `:.Nf` float precision)
- [x] f-strings: `f"hello {name}"` — full expression interpolation and `{{`/`}}` escaping
- [x] Triple-quoted strings: `"""..."""` / `'''...'''`

## List, Set, and Dict Methods

- [x] List: `.append()`, `.extend()`, `.insert()`, `.remove()`, `.pop()`, `.index()`, `.count()`, `.sort()`, `.reverse()`, `.copy()`, `.clear()`
- [x] Set: `.add()`, `.remove()`, `.discard()`, `.pop()`, `.clear()`, `.copy()`, `.union()`, `.intersection()`, `.difference()`, `.symmetric_difference()`, `.update()`, `.intersection_update()`, `.difference_update()`, `.symmetric_difference_update()`, `.issubset()`, `.issuperset()`, `.isdisjoint()`
- [x] Dict: `.keys()`, `.values()`, `.items()`, `.get()`, `.pop()`, `.popitem()`, `.update()`, `.clear()`, `.copy()`, `.setdefault()`

## Built-in Functions

- [x] `print`, `len`, `str`, `repr`, `int` (`i8`–`i64`, `u8`–`u64`), `float` (`f32`, `f64`), `bool`, `bigint`
- [x] `ord`, `chr`, `hex`, `bin`, `oct`, `abs`, `round`, `pow`
- [x] `sum`, `min`, `max`, `all`, `any`, `sorted(..., key=...)`, `reversed`
- [x] `map`, `filter`, `enumerate`, `zip` (2 iterables), `range`
- [x] `list`, `set`, `dict`, `tuple`
- [x] `open` (default `"r"`), `input`, `isinstance`, `super()`, `iter`, `next`
- [ ] `type(obj)` — runtime type inspection
- [ ] `hasattr` / `getattr` / `setattr` — dynamic attribute introspection
- [ ] `eval(expr)` / `exec(code)` — dynamic evaluation (requires runtime compiler embedding)

## Object-Oriented Programming

- [x] `@staticmethod`, `@classmethod`, `@property`
- [x] `@dataclass` — auto-generated constructor/equality; by-value structs for non-recursive dataclasses
- [x] `super()` — cooperative C3 MRO method dispatch
- [x] `ABC` / `@abstractmethod` — abstract base class enforcement
- [x] `__str__` / `__repr__` — string representation protocol
- [x] Operator overloading: `__add__`, `__eq__`, `__lt__`, etc.
- [ ] `__slots__` — explicit slot enforcement
- [ ] `__new__` / metaclasses — custom instance creation

## Module System

- [x] `import` / `from ... import ...` — resolves `.str` modules and `pkg/__init__.str` packages
- [x] Built-in modules: `sys.argv`, `abc`, `dataclasses`, `typing`
- [ ] Standard library: `os`, `math`, `json`, `re`, `time`, full `sys`, etc.

## Typing, Memory, and Other

- [x] Static type checking & compile-time inference — parameter/return/field checking, default-value inference, local/container/lambda inference, `is None` & `isinstance()` flow narrowing, stack allocation of non-escaping locals
- [ ] `__name__ == "__main__"` module guard variable
- [ ] Step parameter in slicing: `seq[start:stop:step]`
- [ ] Garbage collector or reference counting (currently stack allocation for non-escaping locals + `malloc`)
