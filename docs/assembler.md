# `discas`: DiscoC Assembler

`discas` is the standalone assembler in the DiscoC toolchain. It consumes the textual assembly emitted by `discc --emit-asm` and produces the same relocatable `.o` format used by direct compilation.

## Basic workflow

```bash
discc program.dc --emit-asm -o program.s
discas program.s -o program.o
discld program.o -o program.bin
```

Direct object compilation uses the verified IR backend:

```bash
discc program.dc -o program.o
```

The exporter decodes the same canonical IR backend object used by direct
compilation. It does not implement a second AST backend. Unedited output,
assembled and linked with identical options, must produce identical payload
bytes. Some uncommon opcode/prefix combinations are preserved as explicit
`.byte` encodings to avoid changing their expansion.

For an exact **post-link** export:

```bash
discld program.o --emit-asm final.s -o program.bin
discas final.s -o final.o
discld final.o -o roundtrip.bin
```

`roundtrip.bin` matches `program.bin` byte for byte. The linked listing contains
numeric addresses and is fixed-origin; do not change its origin or inject a
second bootstrap on the round trip. See [GSU loading](gsu-loading.md).
This is DiscoC assembly, not directly consumable WLA-DX source.

## Command-line interface

```text
discas <input.s> [-o <output.o>]
```

If `-o` is omitted, the assembler replaces the input extension with `.o`. The assembler reports source line and column information for syntax, operand, range, and unresolved-label errors.

## Source structure

The accepted syntax is a deliberately small ca65-like subset. A typical generated file looks like:

```asm
.setcpu "GSU"
.segment "DATA"
.export message
message:
    .byte 1, 2, 3

.segment "CODE"
.export main
main:
    iwt r0, #message
    stop
    nop
```

The assembler recognizes:

* `.segment "CODE"`, `.segment "DATA"`, and `.segment "RAM"` (RAM initialization bytes, no instructions);
* `.export symbol`;
* `.byte` and `.word` numeric data directives;
* exported or private labels in CODE, DATA, and RAM;
* `.setcpu` lines as accepted metadata directives;
* `.define __DISCO_MEMORY_MAPPING lorom` (or `hirom`);
* `.define __DISCO_CODE_START_ADDRESS <24-bit address>`;
* `.define __DISCO_DATA_ALIGNMENT N` (power of two 1..128);
* `.define __DISCO_RAM_ALIGNMENT N` (power of two 1..128, default 2);
* `.define __DISCO_BITMAP_SCBR N` and `.define __DISCO_BITMAP_SCMR N` (selected host screen configuration);
* semicolon comments.

Labels must be unique within the input file. Exported labels become object-file symbols. Non-exported labels can still be used for local branches and local assembly references.

The reserved `.define` names preserve placement/alignment configuration;
they emit no bytes and do not implement general macro expansion. Without them,
the assembler defaults to LoROM and `$00:8000`. For RAM execution, use:

```asm
.define __DISCO_MEMORY_MAPPING lorom
.define __DISCO_CODE_START_ADDRESS $708000
```

An explicit origin takes precedence over the mapping's default origin in either
definition order. Duplicate, unknown, malformed, or out-of-range configuration
definitions are errors. `discld` validates the resulting GSU address and rejects
objects with incompatible mapping/origin configurations, unless `--origin`
explicitly overrides the origins for the whole link. Mapping/target mismatches
remain errors.

Bitmap metadata requires both `__DISCO_BITMAP_SCBR` and
`__DISCO_BITMAP_SCMR`. SCBR is a byte-sized screen-base register value (base
offset divided by 1024). SCMR contains only height/depth mode bits, not RON/RAN
bus-ownership bits. Invalid modes, incomplete pairs and framebuffer ranges
are rejected. These definitions emit no bytes and do not configure the SNES
host automatically. They survive final linked assembly reassembly; see
[SuperFX graphics](gsu-graphics.md).

Numeric literals support decimal, hexadecimal with `$` or `0x`-style forms accepted by the compiler output, binary with `%`, and signed values where the instruction or data directive allows them.

## Supported instruction families

The assembler supports the instruction forms currently emitted by `AssemblyGenerator`, including:

* relative branches: `bra`, `bge`, `blt`, `bne`, `beq`, `bpl`, `bmi`, `bcc`, `bcs`, `bvc`, `bvs`;
* implied GSU operations such as `stop`, `nop`, `loop`, `plot`, `color`, `getc`, and `getb`;
* alternate prefixes: `alt1`, `alt2`, `alt3`;
* register operations: `to`, `with`, `from`, `inc`, `dec`, `move`;
* immediate operations: `ibt`, `iwt`, and `lea`;
* memory operations: `ldw`, `ldb`, `stw`, `stb`, `push`, `pop`, `pushb`, and `popb`;
* arithmetic and logical operations: `add`, `adc`, `sub`, `sbc`, `mult`, `umult`, `cmp`, `and`, `bic`, `or`, and `xor`;
* control-transfer operations: `jal`, `ret`, `jmp`, and `ljmp`;
* long-memory operations: `lm`, `lms`, `sm`, and `sms`.

The accepted operand forms are intentionally constrained. For example, indirect memory operands use `(rN)`, registers use `r0` through `r15`, and immediate arithmetic values are limited to the ranges defined by the GSU encoding.

## Relocations

When an instruction references a symbol that is not a numeric literal, `discas` leaves an address placeholder and records a relocation in the object file. Examples include:

```asm
iwt r0, #global_data
jal helper
ibt r1, #^global_data
iwt r2, #lo24(global_data)
iwt r0, #ram(frame_counter)
```

Local branch labels are resolved during assembly. External function and data references remain for `discld`, which resolves them after all input objects have been laid out.

`#^symbol` records a 24-bit bank relocation and `#lo24(symbol)` records its low
16-bit counterpart; these are restricted symbolic operand forms, not a general
expression language. `#ram(symbol)` selects a near-RAM relocation independently
of the program bank. Absolute references to non-exported CODE/DATA/RAM labels
become object-private symbols; only exported names are visible across objects.
RAM images require generated startup or explicit host-owned initialization at
link time. Compiler exports retain their images/private references; final
linked exports contain resolved startup/addresses, not a second RAM image.

The native single-operand forms `ldw (r0)` / `stw (r0)` (and `ldb` / `stb`) use
the current destination/source selectors. Existing two-operand convenience
forms remain accepted. Exported canonical code preserves explicit `WITH`, `TO`,
and `FROM` selectors rather than silently adding register-copy expansions.

## Branch ranges

Relative branches use an 8-bit signed displacement measured from the byte after
the displacement field. Short branch targets remain limited to `-128..127`
bytes, but `discas` automatically relaxes an out-of-range local branch to
`IWT R15, target; NOP`. Conditional relaxation emits an inverted short branch,
its `NOP` delay slot, and the absolute jump with its own `NOP` delay slot. The
linker resolves the relaxed local target relative to its input object.
Undefined targets and non-code targets remain assembler errors.

`jal` expands to `LINK #4; IWT R15, target`, and `ret` expands to `JMP R11`.
When writing assembly, explicitly place `nop` after these pseudo-instructions
to fill the GSU delay slot, as compiler-generated assembly does. The assembler
does not insert additional delay slots for raw control-transfer instructions.

## Editing generated assembly

When hand-editing `.s` files:

1. Keep the required CODE/DATA/RAM `.segment` directives.
2. Export every symbol that must be visible to another object or to the linker.
3. Keep branch targets in the code section.
4. Use valid GSU register and operand forms.
5. Preserve relocation-bearing symbol references when cross-file linking is required.
6. Assemble the edited file before linking.

The assembler does not implement the full ca65 language or macro system. It uses its own built-in opcode and operand encodings rather than executing ca65 macros. The normative technical reference for the GSU instruction set is the official SNES Development Manual, Book II, Super FX section.

## Diagnostics and validation

Typical failures include:

* missing `.segment` directives;
* malformed registers or indirect operands;
* unsupported instruction forms;
* invalid immediate or data ranges;
* duplicate labels or exports;
* undefined branch labels;
* branch targets that cannot be resolved;
* malformed relaxed local-branch relocations;

After assembling, link with `discld` to validate cross-object symbols and relocations.
