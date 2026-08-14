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

The assembler workflow is useful when inspecting or hand-editing emitted assembly. The textual backend and assembler are separate from the canonical IR binary path.

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

* `.segment "CODE"` and `.segment "DATA"`;
* `.export symbol`;
* `.byte` and `.word` numeric data directives;
* labels in code and data sections;
* `.setcpu` lines as accepted metadata directives;
* semicolon comments.

Labels must be unique within the input file. Exported labels become object-file symbols. Non-exported labels can still be used for local branches and local assembly references.

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
```

Local branch labels are resolved during assembly. External function and data references remain for `discld`, which resolves them after all input objects have been laid out.

## Branch ranges

Relative branches use an 8-bit signed displacement measured from the byte after the displacement field. Short branch targets remain limited to `-128..127` bytes, but `discas` automatically relaxes an out-of-range local branch to an absolute `IWT R15, target` plus `JMP R15` sequence. The linker resolves the relaxed local target relative to its input object. Undefined targets and non-code targets remain assembler errors.

## Editing generated assembly

When hand-editing `.s` files:

1. Keep both `.segment` directives.
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
