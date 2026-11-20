---
name: discoc
description: Use when writing, fixing or building DiscoC source (`.dc` files, `.dci` interfaces, `discoc.toml` manifests) for the SNES SuperFX/GSU, when running `discc`, `discld` or `discas`, or when a DiscoC diagnostic appears. DiscoC looks like C but rejects many C habits.
---

# Writing and building DiscoC programs

DiscoC is a small freestanding language with C-like spelling, compiled for the
SNES SuperFX (GSU). It is **not ISO C**: there is no preprocessor, no `int`, no
implicit signed/unsigned mixing, no implicit narrowing and no struct copies.
Write it from the rules below, not from C intuition. The normative contract is
[docs/language-spec.md](../../docs/language-spec.md); consult it when a rule
here is not enough.

Output is a **fixed-origin GSU payload** (`.bin`), not a SNES ROM. A host
program on the SNES CPU must load it, configure the GSU and read results.

## Language cheat sheet (where DiscoC differs from C)

**Types**

| Write | Not | Notes |
| --- | --- | --- |
| `byte`, `word` | `char`, `short`, `int`, `long` | 8/16-bit, **signed by default** |
| `unsigned byte`, `unsigned word` | `unsigned` alone | `u8`, `i8`, `u16`, `i16` are builtin aliases |
| `bool` with `true`/`false` | `int` flags | 1 byte; comparisons return `bool` |
| `type Name = u16;` | `typedef` | transparent alias; `unsigned i16` is an error |
| `word f()` | `word f(void)` | empty parameter list |
| `constexpr word N = 8;` | `#define` | no preprocessor, no `#include` |
| integer or fixed-point (`lib/core/fixed.dci`) | `float`, `double` | no floating-point literals |

**Conversions and arithmetic**

- Mixing typed signed and unsigned operands needs an explicit cast:
  `(word)u + s` or `u + (unsigned word)s`. A bare literal adopts the other
  operand's type (`u + 1` is fine).
- Narrowing (`word` to `byte`) and signedness changes in assignments, arguments
  and returns need a cast: `byte b = (byte)w;`.
- Literals are range-checked against their destination: `word a = 40000;` is
  an error. Use `unsigned word`, or cast to request truncation.
- Byte arithmetic promotes to 16 bits, so `b = b + 1;` on a `byte` is a
  narrowing error. Write `b++;`, `b += 1;` (updates wrap to the destination
  width) or `b = (byte)(b + 1);`.
- Arithmetic wraps (two's complement) and is defined. Shift counts must be
  0..15. Division truncates toward zero.
- Runtime faults (dynamic shift count out of range, divide by zero, misaligned
  or invalid pointer) execute `STOP` with a category in R6 (5 = shift,
  6 = division by zero, 1 = misaligned, 2 = invalid address). Constant cases
  are compile-time errors.

**Unsupported (rewrite instead)**: `?:` (use `if`/`else`), `do { } while`
(use `while`/`for`), `goto`, unions, bit-fields, function pointers, variadic
functions, multidimensional arrays, pointer `<`/`>`/difference, struct
assignment/parameters/returns by value.

**Storage, pointers and qualifiers**

- Lookup tables go in ROM as globals: `rom const u8 table[] = {1, 2, 3};`.
  A `rom` local is an error. `const` alone does not mean ROM.
- Locals are not zeroed; globals are zeroed or initialized by the linked
  startup (requires `--init-runtime`).
- Pointers are near (2 bytes, current bank) by default; `far word*` is a
  4-byte bank+offset pointer. Integer-to-pointer needs a cast:
  `(volatile word*)0x0100`. Far literals are 24-bit: `(far word*)0x711000`.
- Every `word` load/store must be at an even address. `(word*)0x0101` is
  rejected; a dynamic odd address stops with R6=1.
- Pointers are not conditions: write `if (p != null)`, not `if (p)`.
- Local arrays do not decay: `word* p = &values[0];`. ROM arrays decay to a
  ROM pointer, which keeps its address space: `rom const u8* t = table;`.
- `volatile` for host-visible or hardware memory; each access is kept.

**Structs**: always `struct Name` (or a `type` alias). Members sit at even
offsets (use `@packed` to opt out). Pass `struct Name*` and use `->`; assign
members, never whole structs.

**Functions, linkage and modules**

- Symbols are public by default; `internal` makes them file-private, `export`
  is explicit public. `main` must stay external.
- Functions may be called before their definition in the same file. Calls to
  another file need its prototype, usually via `import "math.dc";` at the top
  of the file. `extern word shared;` declares RAM data from another unit.
- Importing exposes declarations only. `discc build` compiles imported `.dc`
  files automatically; single-file `discc` and `.dci` imports never do.
- Non-void functions must return on every path (no implicit `return 0`).

**Control flow**: `if`, `while`, `for`, `switch`, `break`, `continue`,
`return`. Switch arms fall through like C, but nonempty fallthrough warns:
end each arm with `break;` or write `fallthrough;` before the next label.

**Reserved words** that are common variable names in C: `color`, `pixel`,
`cursor`, `bitmap`, `use`, `plot`, `at`, `draw`, `flush`, `options`,
`read_pixel`, `cache`, `rom`, `ram`, `far`, `null`, `type`, `module`,
`import`, `internal`, `export`, `fallthrough`. Rename (`colour_index`,
`pixel_x`, ...).

**Compile-time**: `constexpr`, `enum Name : unsigned byte { A, B };`,
`static_assert(expr);` (no message argument), `sizeof`, `alignof`,
`offsetof(struct T, member)`. Attributes: `@cache` (functions/loops),
`@packed`, `@align(N)`, `@cfg(gsu)`, `@target(gsu)`.

**Graphics** (GSU only, see [docs/gsu-graphics.md](../../docs/gsu-graphics.md)):
drawing happens inside `plot { ... }`. Inside it: `options transparent;`
(sets the full option set; `options;` clears it), `color expr;`,
`at (x, y);`, `pixel;` (draws, then hardware advances `cursor.x` by one),
`draw at (x, y) with color c;`, `read_pixel`, `flush;`. A file-scope
`bitmap name { mode bitmap; size 256x192; depth 4bpp; base 0x0000; }` plus
`use bitmap name;` records the framebuffer layout for the host; it emits no
GSU code. Blocks never initialize the cursor, flush or reset colour/options
implicitly: set them before use, and `flush;` when pixels must be complete.

## Build workflow

Build the toolchain once (see [docs/building.md](../../docs/building.md)) and
put `build/<preset>/bin` on `PATH`:

```sh
cmake --preset release && cmake --build --preset release
```

Check fast while editing (no output files):

```sh
discc --check -Wall -Werror main.dc
```

**Project build** (preferred for more than one file). With a `discoc.toml`
next to the sources:

```sh
discc build            # or: discc build --config path/to/discoc.toml
discc build -O2        # -O0 default, -O/-O1 local, -O2 global, -Os size
discc build --check    # analyze every source, no outputs, no link
```

**Separate tools** (compile each unit, then link):

```sh
discc main.dc -o main.o
discc math.dc -o math.o
discld main.o math.o --init-runtime --emit-asm final.s -o program.bin
```

Key link options: `--init-runtime` (generated startup: RAM bank, stack, global
initialization, jump to `main`), `--origin <24-bit>` (execution address,
default `$00:8000` in LoROM), `--stack-pointer` (default `0x2000`, grows
down), `--ram-origin` (static globals, default `0x0400`), `--ram-bank 0|1`.
Inspect with `discc --emit-ir`, `discc --emit-asm` (one relocatable unit) or
the linker's `--emit-asm` (whole payload).

**Running** needs a SNES host: copy or map the payload at its origin, configure
GSU registers and bus ownership, start at the **payload's first byte** (not at
`main`, which skips startup), wait for STOP, check R6 is 0, then read results
from agreed RAM. There is no standalone runner in the toolchain. For a full
ROM, follow [examples/snes/triangle](../../examples/snes/triangle/README.md)
(needs WLA-DX). Loading rules: [docs/gsu-loading.md](../../docs/gsu-loading.md).

**Memory map**: keep the payload, static RAM (from `ram_origin`), the stack
(below `stack_pointer`), host result buffers and any framebuffer apart. The
linker checks its own regions but does not reserve addresses you write
through raw pointers.

## Diagnostics and fixes

| Message (abridged) | Cause | Fix |
| --- | --- | --- |
| `Mixed signed/unsigned operands require an explicit cast.` | `word` op `unsigned word` | Cast one operand to the intended type |
| `Cannot implicitly convert 'word' to 'byte'` / `... to 'unsigned word'` | Narrowing or signedness change | Explicit cast, or change the declared type |
| `Cannot implicitly convert 'unsigned word' to 'unsigned byte'.` | `b = b + 1` on a byte (promoted) | `b++;`, `b += 1;` or `(unsigned byte)(b + 1)` |
| `Value '40000' out of range for signed word.` | Literal too big | `unsigned word`, or `(word)` cast for truncation |
| `Expect ';' after expression.` | `int`, `char`, or `do { } while` | `word`/`byte`; rewrite as `while` |
| `Expect ';' after variable declaration.` | `?:` | `if`/`else` |
| `Expect member name after '.'.` | Float literal `1.5` | Integer or fixed-point raw value |
| `Unknown type alias 'x'` / `'typedef'` | `unsigned x`, `typedef` | `unsigned word x`; `type Name = ...;` |
| `Expected a base type specifier` at line 1 | `#include` / `#define` | `import "file.dc";`, `constexpr` |
| `Expect identifier for variable declaration.` | Reserved word as a name (`color`) | Rename the variable |
| `Void is not a valid function parameter type.` | `f(void)` | `f()` |
| `Pointer address construction requires an explicit cast.` | `word* p = 0x100;` | `(word*)0x0100` |
| `Misaligned 16-bit pointer address is illegal.` | Odd word address | Use an even address, or a byte pointer |
| `Cannot implicitly convert 'word*' to 'bool'.` | `if (p)` | `if (p != null)` |
| `Cannot implicitly convert 'word[4]' to 'word*'.` | Local array decay | `&values[0]` |
| `Cannot implicitly convert 'const rom unsigned byte*' to 'const unsigned byte*'.` | ROM data through a RAM pointer | Declare the pointer `rom const u8*` |
| `Struct assignment by value is not supported; assign members.` | `a = b;` on structs | Copy members, or pass pointers |
| `Local storage resides in RAM; declare ROM data globally.` | `rom` local | Move the table to file scope |
| `Non-void function may reach the end without returning a value.` | Missing return path | Add a final `return` |
| `Binary operators require integer operands.` | Pointer `<`, `-` | Compare with `==`/`!=`, or keep indices |
| `Function pointers and inter-bank code calls are not supported yet.` | `&function` | Use `switch` dispatch |
| `Legacy graphics API removed; ...` | `plot_begin`, `plot(x,y)`, `set_color` | `plot { ... }`, `draw at`, `color` |
| `Source-level set configuration has been removed; ...` | `set origin ...` | Use `--origin` or `discoc.toml` |
| `Switch arm falls through; add fallthrough; or break;.` | Missing `break` | `break;` or `fallthrough;` |
| `Linker Error: Undefined symbol 'add'.` | Unit not linked | Link its `.o`, or use `discc build` |
| `Static globals require --init-runtime or --host-initialized-globals.` | Globals without startup | Add `--init-runtime` (or `[runtime] initialize = true`) |

`-Werror` makes `unused-variable`, `uninitialized` and `implicit-fallthrough`
warnings fatal, so every example below uses all of its locals.

## Example 1: single file, separate tools

Runs from ROM at the default origin `$00:8000`. Memory map: results at
`$70:0100-$70:0107`, the global `calls` from `$70:0400`, stack below `$70:2000`.

```c
// file: sum.dc
struct Stats {
    word total;
    unsigned byte count;
};

rom const unsigned byte samples[] = {10, 20, 30, 40, 250};
word calls;                                      // zeroed by --init-runtime

void accumulate(struct Stats* stats, unsigned byte value) {
    stats->total = stats->total + (word)value;   // u8 -> word needs a cast
    stats->count++;                              // count + 1 would be a word
    calls++;
}

void main() {
    struct Stats stats;                          // locals are not zeroed
    stats.total = 0;
    stats.count = 0;
    for (word i = 0; i < (word)sizeof(samples); i++) {
        accumulate(&stats, samples[i]);
    }
    volatile word* out = (volatile word*)0x0100;  // host-agreed RAM
    out[0] = stats.total;                         // 350
    out[1] = (word)stats.count;                   // 5
    out[2] = calls;                               // 5
    if (stats.total > 300) { out[3] = 1; } else { out[3] = 0; }
}
```

```sh
discc --check -Wall -Werror sum.dc
discc -O2 sum.dc -o sum.o
discld sum.o --init-runtime --emit-asm sum.s -o sum.bin
```

## Example 2: two files, project build

`main.dc` imports `geometry.dc`; `discc build` finds and compiles the import,
links both and adds startup. ROM execution at `$00:8000`; the `boxes` global
lives from `$70:0400`; results at `$70:0100`.

```c
// file: geometry.dc
type Coord = i16;

enum Shape : unsigned byte { SHAPE_EMPTY, SHAPE_SQUARE, SHAPE_RECT };

struct Box {
    Coord width;
    Coord height;
};
type Box = struct Box;

constexpr word MAX_BOXES = 4;

internal Coord clamp_positive(Coord value) {     // private to this file
    if (value < 0) {
        return 0;
    }
    return value;
}

export unsigned word box_area(const Box* box) {
    unsigned word w = (unsigned word)clamp_positive(box->width);
    unsigned word h = (unsigned word)clamp_positive(box->height);
    return w * h;
}

export enum Shape classify(const Box* box) {
    if (box_area(box) == 0) {
        return SHAPE_EMPTY;
    }
    if (box->width == box->height) {
        return SHAPE_SQUARE;
    }
    return SHAPE_RECT;
}
```

```c
// file: main.dc
import "geometry.dc";

static_assert(sizeof(Box) == 4);

rom const i16 widths[] = {3, 4, -2, 6};
rom const i16 heights[] = {3, 5, 7, 6};
Box boxes[MAX_BOXES];

void main() {
    unsigned word total = 0;
    word squares = 0;
    for (word i = 0; i < MAX_BOXES; i++) {
        boxes[i].width = widths[i];
        boxes[i].height = heights[i];
        total += box_area(&boxes[i]);
        switch (classify(&boxes[i])) {
            case SHAPE_SQUARE:
                squares++;
                break;
            default:
                break;
        }
    }
    volatile unsigned word* out = (volatile unsigned word*)0x0100;
    out[0] = total;                  // 9 + 20 + 0 + 36 = 65
    out[1] = (unsigned word)squares; // 2
}
```

```toml
# file: discoc.toml
[project]
name = "boxes"
target = "gsu"
sources = ["main.dc"]

[compiler]
optimization_level = 2

[runtime]
initialize = true

[output]
directory = "build"
binary = "boxes.bin"
assembly = "boxes.s"
```

```sh
discc build --check -Wall -Werror
discc build                       # build/0-main.o, build/1-geometry.o, build/boxes.bin
```

For RAM execution add `[target.gsu] execution_memory = "ram"` and
`origin = 0x700900`, and keep `rom const` tables out of that payload: the
linker cannot place them in ROM while the code runs from RAM. Full schema:
[docs/project-manifest.md](../../docs/project-manifest.md).

## Example 3: SuperFX plotting

Fills a 256x192 4bpp bitmap with colour bands. The framebuffer occupies
`$70:0000-$70:5FFF`, so the stack moves to `$70:8000`, and the program uses no
globals (they would default to `$70:0400`, inside the framebuffer).

```c
// file: bands.dc
rom const u8 palette[] = {1, 2, 3, 4};

bitmap screen {
    mode bitmap;
    size 256x192;
    depth 4bpp;
    base 0x0000;
}

void main() {
    use bitmap screen;
    plot {
        options;                             // opaque, no dithering
        for (word y = 0; y < 192; y++) {
            color palette[(y >> 4) & 3];     // ROM byte read via GETC
            at (0, y);                       // set X once per row
            @cache for (word x = 0; x < 256; x++) {
                pixel;                       // hardware advances cursor.x
            }
        }
        draw at (128, 96) with color 15;
        flush;                               // write pending pixel caches
    }
}
```

```sh
discc --check -Wall -Werror bands.dc
discc -O2 bands.dc -o bands.o
discld bands.o --init-runtime --stack-pointer 0x8000 --emit-asm bands.s -o bands.bin
```

The host writes SCBR/SCMR for this layout (the linked assembly records them
as `__DISCO_BITMAP_SCBR`/`__DISCO_BITMAP_SCMR`) and copies the bitplanes to
VRAM; see [docs/gsu-graphics.md](../../docs/gsu-graphics.md).

## More

- Libraries: `lib/core/fixed.dci` (Q8.8/Q12.4), `lib/core/memory.dci`
  (byte copy/fill), `lib/targets/gsu/graphics.dci`. Import the `.dc` from a
  project build, or compile and link the `.dc` yourself:
  [docs/freestanding-library.md](../../docs/freestanding-library.md).
- Optimization levels and their trade-offs:
  [docs/optimization.md](../../docs/optimization.md).
- Repository examples: [docs/examples.md](../../docs/examples.md).
