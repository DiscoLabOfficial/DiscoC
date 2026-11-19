# SPC-700 Target

DiscoC currently emits GSU code. This document records the SPC-700 target
model, its ABI and runtime contract. The SPC700 assembler, relocations, linker
placement, runtime startup and assembly export are implemented (see
[discas](assembler.md)) and execute in the instruction-level test model;
compiler code generation remains disabled until the lowering stage exists.

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
  returns. Stack slots are packed: a word argument occupies two consecutive
  bytes, low byte first, with no alignment padding.
* `Y` in `[$02]+Y` is an unsigned index, so a frame is laid out upward from
  FP. A callee with `L` bytes of locals subtracts `L + 2` from SSP, stores the
  caller's FP at SSP and sets FP = SSP:

  | Address | Contents |
  | --- | --- |
  | `FP+0..FP+1` | caller's FP |
  | `FP+2..FP+L+1` | locals |
  | `FP+L+2..` | arguments, first argument first |

  The epilogue sets SSP = FP, reloads the caller's FP from there and adds
  `L + 2` to SSP. Saved FP, locals and arguments together span at most 256
  bytes.
* `A`, `X`, `Y`, `PSW` and `$0004-$001F` are clobbered by a call; a value live
  across a call belongs in the caller's frame. SSP and FP are preserved.
  `PSW.P` is clear at every call boundary (direct page = page 0).
* `CALL`/`RET` use the hardware stack; nothing else may be left on it across
  a call.
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

## Command-line configuration

The target is selected on the command line:

```bash
discc --target spc700 program.dc -o program.o
```

`discc --target spc700 --check program.dc` checks common language semantics and
verified IR without machine emission. `--emit-ir` also remains available.
Graphics, instruction-cache, hardware-loop and far-data capabilities are
explicitly unavailable on this target and rejected by the analyzer/verifier. Normal object and assembly emission currently reports that the
SPC-700 backend is not implemented yet; this prevents the GSU backend from
being used accidentally.

## Initial language subset

The first SPC-700 code-generation milestone should support:

* `byte` and `word` integers;
* 16-bit pointers;
* named functions and direct calls;
* `return`, `if`, `while`, and basic comparisons;
* ordinary RAM loads and stores;
* explicit `volatile` hardware access using the language/IR qualifiers.

The shared frontend now supports const/volatile qualifiers, bool, strict
integer conversions, modular arithmetic, bitwise/shifts, short-circuit logic,
constant expressions/enums, layout queries, aggregate initialization, byte
strings, real for/continue control flow and .dci module interfaces.
Transparent aliases (`u8`/`i8`/`u16`/`i16` and user-defined `type` declarations)
and `@cfg(spc700)` declarations/imports share the same frontend. Portable
fixed-point and memory core modules pass these frontend/IR checks; there is no
SPC700 machine library or DSP implementation yet. GSU graphics interfaces use
target assertions and must not be substituted on this processor.
IR inspection can represent common constructs; this does not supply SPC-700 machine
lowering. A future backend must preserve observable accesses, including
`$F0-$FF` register accesses, and follow the ABI and runtime above.

The first milestone should defer structs, arrays, `rom const`, GSU plotting
operations, 24-bit `far` pointers, floating-point types, dynamic allocation,
and target-specific DSP abstractions until the core calling convention is
validated.
