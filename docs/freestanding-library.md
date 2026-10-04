# Freestanding libraries

DiscoC's libraries are source modules, not a hosted libc. They do not assume an
operating system, heap, file streams, exceptions, or automatic initialization of
hardware. Portable code lives in `lib/core/`; processor-specific operations live
under `lib/targets/<target>/`. Library functions use the ordinary language, IR,
and calling convention: there is no intrinsic shortcut around pointer checks.

## Building and importing

Import a relative `.dci` interface, compile the matching `.dc` implementation,
and pass its object to the linker. Imports do not compile or link implementations
automatically. Use the same target/placement flags for all objects.

From the repository root, with the tools on PATH:

```bash
discc examples/fixed_point/main.dc -o main.o
discc lib/core/fixed.dc -o fixed.o
discc lib/core/memory.dc -o memory.o
discld main.o fixed.o memory.o --init-runtime --emit-asm final.s -o fixed.bin
```

This creates a fixed-origin GSU payload, not a SNES ROM. See
[GSU loading](gsu-loading.md) for host initialization, memory ownership, and RAM
execution. See the [example](../examples/fixed_point/README.md) for result bytes.
Libraries are not automatically dead-stripped: linking an implementation also
includes its unused functions. Combined code/data must fit the linker's single
program-bank limit.

The core sources pass common-language/IR checks with `--target spc700 --check`.
SPC700 machine emission remains unavailable; these checks are not evidence of
an executable SPC700 library.

## Signed fixed-point

`core/fixed.dci` declares transparent aliases of `i16`:

| Alias | Fraction bits | Scale | Value range |
| --- | ---: | ---: | --- |
| `q8_8` | 8 | 256 | -128 through 127 + 255/256 |
| `q12_4` | 4 | 16 | -2048 through 2047 + 15/16 |

The names count the sign bit among the integer bits. Stored values are signed
16-bit two's-complement raw bits; the represented number is `raw / scale`.
These aliases are not primitive or nominal numeric types. Assignment does not
scale values, mixing the two formats is not diagnosed, and ordinary `*`/`/`
remain integer operations. Use the matching functions for scaled arithmetic.

Each format supplies `from_int`, `to_int`, `add`, `sub`, `mul`, and `div`, with
names such as `dc_q8_8_mul(left, right)`:

- `from_int(n)` keeps the low 16 bits of `n * scale`.
- `to_int(raw)` truncates `raw / scale` toward zero.
- `add` and `sub` keep the low 16 bits of the raw sum/difference.
- `mul(a, b)` computes `truncate_toward_zero(a * b / scale)`, then keeps the
  low 16 bits. Partial products retain the full intermediate product, rather
  than multiplying as one word and losing its upper bits before scaling.
- `div(a, b)` computes `truncate_toward_zero(a * scale / b)`, then keeps the
  low 16 bits. Magnitude division and bounded fractional-bit generation retain
  precision without requiring a 32-bit primitive.

All returned raw bits are interpreted as `i16`. Overflow wraps; there is no
saturation, overflow flag, nearest-even rounding, or implicit conversion between
formats. Signed scaling truncates toward zero, including negative results; it
is not the floor rounding of an arithmetic right shift.

```c
import "../lib/core/fixed.dci"; // relative to this example's containing file

void main() {
    q8_8 one_and_half = 384;
    q8_8 two = dc_q8_8_from_int(2);
    q8_8 three = dc_q8_8_mul(one_and_half, two); // raw 768
    i16 whole = dc_q8_8_to_int(three);          // 3
}
```

A zero divisor follows the language's arithmetic-fault contract. Through a
library call it is checked dynamically: GSU stops with R6=6, without returning
a fabricated value. The host must treat that STOP as failure and must not resume
it. These are software routines with calls, checked accesses, and bounded
arithmetic loops, not single instructions or measured cycle guarantees.

`-Wall` intentionally reports the software division in this implementation via
`-Wexpensive-helper`. If enforcing `-Werror` while building this known software
library, explicitly acknowledge that cost with `-Wno-expensive-helper`; other
enabled warnings remain fatal. The regression suite checks both the warning
and its specific suppression.

Q16.16 is deferred until a 32-bit value representation and its argument/return
ABI are implemented. BF16/FP16, generic numeric types, and saturation variants
are later work; neither a new primitive nor floating-point support is implied
by this library.

## Byte memory operations

`core/memory.dci` exposes:

```c
void dc_memcpy(u8* destination, const u8* source, u16 count);
void dc_memmove(u8* destination, const u8* source, u16 count);
void dc_memset(u8* destination, u8 value, u16 count);
```

`memcpy` copies forward and requires nonoverlapping ranges. `memmove` supports
overlap, choosing forward/backward copying according to near RAM offsets.
`memset` fills bytes, preserving all eight bits of its unsigned value. The
functions return void, not the destination pointer.

These APIs accept ordinary, nonvolatile near RAM pointers. Both ranges must
remain valid in the same configured near RAM bank for the full count. They do
not accept ROM, far addresses, or MMIO/volatile pointers; qualifiers and address
spaces cannot be cast away to use them. An explicit far/ROM/volatile API needs
its own contract before implementation. Lifetime and allocated-object bounds
remain caller responsibilities.

Count zero performs no memory access and permits null pointers. Count one at
offset `$FFFF` is valid for a byte. A larger count that escapes the bank triggers
the checked pointer fault. Checks happen per access, not as a transactional
range preflight: earlier bytes may already have been written when a later byte
faults. A nonzero access through null faults. No atomicity or rollback is implied.

## Target libraries

`targets/gsu/graphics.dci` offers `dc_gsu_plot_pixel`, `dc_gsu_fill_rect`, and
`dc_gsu_flush_pixels`. Implementations use lexical plot contexts and explicit
GSU capability operations. Compile/link `graphics.dc` like any other module.
`@target(gsu)` assertions on the declarations reject use on SPC700.

The caller owns screen memory, graphics mode, bus access, and coordinate/color
ranges. A nonpositive rectangle width/height draws no pixels. Positive dimensions
use ordinary modular `i16` coordinate arithmetic; no clipping is performed.
Flush explicitly when the host requires completed pixel writes. Byte-exact
assembly tests cover these routines, not rendered pixels or graphics timing.

There is no SPC700 DSP library yet. Its implementation must accompany an
executable SPC700 backend and explicit volatile-register/runtime ownership.
Target selection can choose interfaces/implementations with `@cfg`, without
introducing a textual preprocessor; see
[the language specification](language-spec.md#16-conditional-target-declarations).
