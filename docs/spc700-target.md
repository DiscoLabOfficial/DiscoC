# SPC-700 Target

This document records the SPC-700 target model, its ABI, runtime contract
and code generation. The SPC700 assembler, relocations, linker placement,
runtime startup, assembly export and a baseline compiler backend are
implemented (see [discas](assembler.md) and [Code generation](#code-generation))
and execute in the instruction-level test model.

## Architectural model

The SPC-700 is an 8-bit processor with a 16-bit program counter and a 64 KiB
address space. Its architectural registers are `A`, `X`, `Y`, `SP`, `PC`, and
`PSW`; `YA` is a logical 16-bit pair used by selected instructions. The
hardware stack occupies page 1 (`$0100-$01FF`). Direct-page instructions can
address either page 0 or page 1 depending on the `P` flag in `PSW`.

The `$00F0-$00FF` range is reserved for SPC control, DSP, communication-port,
timer, and counter registers. The four communication ports visible to the SPC
are `$F4-$F7`. The `$FFC0-$FFFF` range may expose the IPL ROM depending on the
processor state; it is not treated as ordinary compiler-owned data.

These boundaries follow the [SPC700 Reference](https://wiki.superfamicom.org/spc700-reference)
and the [SNESdev SPC-700 instruction-set reference](https://snes.nesdev.org/wiki/SPC-700_instruction_set).

## Initial data layout

| Property | Value |
| --- | ---: |
| Address width | 16 bits |
| Address size | 2 bytes |
| `byte` size | 1 byte |
| `word` size | 2 bytes |
| `bool` size | 1 byte |
| Pointer size | 2 bytes |
| Direct-page window | 256 bytes |
| Hardware stack | `$0100-$01FF` |
| Memory-mapped registers | `$00F0-$00FF` |

The SPC-700 target does not have a separate ROM address space: code, data,
static RAM and the software stack share the 64 KiB memory. The compiler must
not treat GSU `rom` addressing or 24-bit `far` pointers as valid SPC-700
operations.

## Memory map

`SPC700Target.hpp` is the single source of these values for the linker, the
runtime startup and the future code generator.

| Range | Owner |
| --- | --- |
| `$0000-$0001` | Software stack pointer (SSP): address of the last allocated byte |
| `$0002-$0003` | Frame pointer (FP) |
| `$0004-$001F` | Compiler scratch and pseudo-registers, caller-saved |
| `$0020-$00EF` | Direct page available to the program |
| `$00F0-$00FF` | I/O registers; never linked |
| `$0100-$01FF` | Hardware stack: return addresses and balanced `PUSH`/`POP` only; never linked |
| `$0200-…` | Default payload origin: CODE, then DATA |
| `--ram-origin` | Static RAM; by default the first suitably aligned address after the payload |
| below the stack top | Software stack, growing downward to the end of the highest image below it |
| `$FFC0-$FFFF` | IPL ROM area; never linked, and the stack top cannot exceed `$FFC0` |

With `--init-runtime`, `$0000-$001F` is also reserved and may not hold linked
code, data or static RAM. `discld` rejects any overlap with a reserved region,
between the payload and static RAM, or with the software stack, and never
writes its output when it does.

## ABI

* Byte results are returned in `A`; word results in `YA` (low byte in `A`).
* Arguments and locals live on the software stack. The caller pushes the
  arguments from last to first (`decw $00` and stores through `[$00]+Y`), so
  the first argument ends at the lowest address, and pops them after the call
  returns. Each argument occupies one little-endian word slot; a byte or bool
  argument uses the slot's low byte. Frames have an even size, so FP and every
  word local or argument stay word-aligned.
* `Y` in `[$02]+Y` is an unsigned index, so a frame is laid out upward from
  FP. A callee with `L` bytes of locals subtracts `L + 2` from SSP, stores the
  caller's FP at SSP and sets FP = SSP:

  | Address | Contents |
  | --- | --- |
  | `FP+0..FP+1` | caller's FP |
  | `FP+2..FP+L+1` | locals |
  | `FP+L+2..` | arguments, first argument first |

  The epilogue sets SSP = FP, reloads the caller's FP from there and adds
  `L + 2` to SSP. `[$02]+Y` reaches FP+255; generated code addresses larger
  frames (up to 32 KiB) through a pointer computed from FP.
* `A`, `X`, `Y`, `PSW` and `$0004-$001F` are clobbered by a call; a value live
  across a call belongs in the caller's frame. SSP and FP are preserved.
  `PSW.P` is clear at every call boundary (direct page = page 0).
* `CALL`/`RET` use the hardware stack; nothing else may be left on it across
  a call. Generated prologues fail with code 2 when the hardware SP is below
  `$20`, so call depth is bounded (about 110 nested calls) instead of letting
  return addresses wrap around page 1.
* `__disco_stack_limit_N`, referenced through an absolute (`!sym`) or
  immediate byte (`#<sym`, `#>sym`) relocation, resolves to the lowest
  software-stack address plus N. The linker rejects a limit above the stack
  top. A function that needs N bytes compares SSP against it.
* Volatile accesses are required for the I/O registers, communication ports,
  DSP registers and timers.

## Runtime

`discld --init-runtime` places a startup sequence at the payload origin:

1. `clrp`, then an empty hardware stack (`SP = $FF`);
2. SSP and FP set to the software stack top (`--stack-pointer`, default and
   maximum `$FFC0`);
3. static RAM cleared, then its nonzero span copied from a copy appended to
   DATA (each step only when needed);
4. `call` to the entry (`--entry`, default `main`);
5. `main`'s `YA` written to ports 2/3 (`$F6/$F7`), the IPL ROM kept mapped
   (`$F1 = $80`) and a jump back to the IPL at `$FFC0`.

The returning IPL announces `$AA`/`$BB` on ports 0/1 again: that is the
completion signal. The host reads the result from `$2142/$2143` and may start
the program again at its origin without re-uploading; the startup
re-initializes the stacks and static RAM on every run. The IPL ROM clears
`$0001-$00EF` when it restarts, so nothing in page 0 survives a run.
`tests/spc700_host/upload.asm` is a minimal SNES host that uploads a payload
through the IPL protocol, runs it twice and records both results.

Without `--init-runtime`, static RAM needs `--host-initialized-globals`, and
the host owns all initialization.

## Fault channel

Run-time checks report failures with the codes of the language
specification's fault table (1 misaligned, 2 invalid address or stack, 3
address overflow, 4 far-to-near mismatch, 5 shift count, 6 division by zero).
A failing check writes the code to port 1 (`$F5`), then `$FA` to port 0
(`$F4`), and executes `STOP`. The host sees `$FA` on `$2140` where a finished
program shows the IPL's `$AA`, reads the code from `$2141`, and must reset the
APU before running anything again: frames are not unwound and `main` has not
completed. `tests/spc700_host/upload.asm` records such a stop as status `$02`
with the code. The constants live in `SPC700Target::Fault`.

## Command-line configuration

The target is selected on the command line:

```bash
discc --target spc700 program.dc -o program.o
```

`discc --target spc700 --check program.dc` checks common language semantics and
verified IR without machine emission; `--emit-ir` prints the IR. Graphics,
instruction-cache, hardware-loop and far-data capabilities are explicitly
unavailable on this target and rejected by the analyzer/verifier. Object and
`--emit-asm` output come from the SPC700 backend; it never falls back to GSU
code. Link the object with `discld --init-runtime` (or a host-provided entry)
as described above.

## Code generation

The backend (`src/SPC700CodeGenerator.*`, `src/SPC700IntegerLowering.cpp`)
lowers the verified IR to SPC-700 assembly text and assembles it in-process,
so `discc -o` and `discc --emit-asm` followed by `discas` produce the same
bytes. Every IR value lives in its own frame slot; `A`, `X`, `Y` and the
scratch bytes `$04-$1F` are used only within one IR instruction. Optimization
levels are accepted, but every level currently compiles the unoptimized IR;
register allocation and IR optimization for SPC700 are later work.

Supported lowering:

| IR / language feature | SPC700 lowering |
| --- | --- |
| `byte`, `word`, `bool`, signed and unsigned | frame slots of 1 or 2 bytes; values are extended by their own type |
| `+ - & \| ^`, unary `- ~ !` | 16-bit `ADDW`/`SUBW`/byte logic, truncated to the result width |
| `*` | three `MUL`s, low 16 bits |
| `/ %` | shared 16-step restoring division helpers (signed: truncation toward zero, `-32768 / -1 = -32768`); fault 6 on zero |
| `<< >>` | checked count (fault 5 outside 0..15), then shift loops; signed `>>` is arithmetic |
| comparisons | `CMPW` (unsigned) or `SUBW` with N⊕V (signed), signedness from the left operand |
| casts, bool normalization | extension, truncation, nonzero → 1 |
| locals, parameters, `&&`/`\|\|` temporaries | frame addresses; over-aligned locals rounded up at run time |
| globals, `const`, `rom` data, strings | `RAM`/`DATA` sections with `#<sym`/`#>sym` addresses; `rom` data lives in DATA |
| pointers, `->`, `[]`, structs, arrays | checked like GSU: misaligned 1, null or beyond 64 KiB 2, offset or member overflow 3 |
| volatile accesses | one access per byte, in program order, low byte first |
| direct calls, returns, `switch` | ABI above; `switch` is a compare chain |
| stack overflow | hardware and software stack guards in every prologue (fault 2) |

Not lowered yet, with a diagnostic instead of GSU code: SSA phi nodes and the
other O2-only IR forms (fused division pairs, bit extraction, local array
pools, scaled pointer recurrences), which the baseline never receives because
it compiles the unoptimized IR. `far` pointers and GSU graphics, cache and
hardware loops are capabilities this target does not have.

## Language coverage

The SPC700 backend compiles the shared language: byte/word/bool integers with
strict conversions and modular arithmetic, bitwise operators and checked
shifts, short-circuit logic, constant expressions and enums, layout queries,
structs and arrays in memory with aggregate initialization, byte strings,
`rom` and `const` data, for/continue/switch control flow, `.dci` module
interfaces, type aliases and `@cfg(spc700)` declarations. Portable core modules
pass the frontend and IR checks; there is no SPC700 machine library or DSP
implementation yet. GSU graphics interfaces use target assertions and must
not be substituted on this processor. 24-bit `far` pointers, floating-point
types, dynamic allocation and DSP abstractions remain out of scope.
