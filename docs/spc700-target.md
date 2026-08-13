# SPC-700 Target Foundation

DiscoC currently emits GSU code. This document records the target model and
the initial ABI proposal for the future SPC-700 backend. Selecting `spc700` is
intentionally accepted by the compiler configuration and object format, but code
generation remains disabled until the assembler and lowering backend are
implemented.

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
| Pointer size | 2 bytes |
| Direct-page window | 256 bytes |
| Hardware stack | `$0100-$01FF` |
| Memory-mapped registers | `$00F0-$00FF` |

The SPC-700 target does not have a separate ROM address space. A future
runtime/link configuration must decide where code, mutable data, samples, and
the software-managed compiler stack live in the shared 64 KiB memory. The
compiler must not silently treat GSU `rom` addressing or 24-bit `far` pointers
as valid SPC-700 operations.

## Initial ABI proposal

This is the deliberately small ABI contract for the first backend milestone:

* byte return values use `A`;
* word return values use `YA`, with the low byte in `A` and the high byte in
  `Y`;
* `A`, `X`, and `Y` are caller-clobbered value registers;
* `SP`, `PC`, and `PSW` remain architectural state and are never allocated as
  ordinary IR values;
* calls use the SPC-700 subroutine instructions and return with `RET`;
* argument passing, local storage, and caller/callee cleanup will use a
  compiler-managed software stack separate from the hardware return-address
  stack;
* byte arguments have one-byte alignment and word arguments have two-byte
  alignment;
* volatile memory access will be required for communication ports, DSP
  registers, timers, and other hardware-visible locations.

The stack-base and runtime memory reservation are intentionally not fixed in
this foundation PR. They belong in the object/linker configuration once the
SPC-700 assembler and relocation model exist.

## Command-line configuration

The target is selected on the command line:

```bash
discc --target spc700 program.dc -o program.o
```

`discc --emit-ir` remains available for this source because the IR is target
independent. Normal object and assembly emission currently reports that the
SPC-700 backend is not implemented yet; this prevents the GSU backend from
being used accidentally.

## Initial language subset

The first SPC-700 code-generation milestone should support:

* `byte` and `word` integers;
* 16-bit pointers;
* named functions and direct calls;
* `return`, `if`, `while`, and basic comparisons;
* ordinary RAM loads and stores;
* explicit `volatile` hardware access once the language qualifier exists.

The first milestone should defer structs, arrays, `rom const`, GSU plotting
operations, 24-bit `far` pointers, floating-point types, dynamic allocation,
and target-specific DSP abstractions until the core calling convention is
validated.
