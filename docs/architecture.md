# DiscoC Architecture

DiscoC is a small compiler toolchain for the SuperFX/GSU processor. The repository contains a source compiler, a textual assembly emitter, a standalone assembler, and a relocatable object linker.

## End-to-end pipeline

The normal object-file path is:

```text
.dc source
    |
    v
Lexer -> Parser -> AST -> Optimizer -> Analyzer
                                      |
                                      v
                               IRLowerer
                                      |
                                      v
                               IRVerifier
                                      |
                                      v
                            IRCodeGenerator
                                      |
                                      v
                              relocatable .o
                                      |
                                      v
                               discld linker
                                      |
                                      v
                              linked GSU payload
```

The compiler also exposes two inspection or alternate-emission paths:

* `discc --emit-ast` prints the optimized abstract syntax tree.
* `discc --emit-ir` prints the verified IR and its basic blocks.
* `discc --emit-asm` writes textual GSU assembly. That assembly can be passed to `discas` to create a relocatable object file.

The linked `.bin` is a GSU payload. It is not a complete SNES ROM image: it does not provide a SNES header, host-side startup integration, cartridge metadata, or other ROM-level resources.

## Compiler stages

### Lexer and parser

The lexer converts source text into tokens. The recursive-descent parser constructs an owning AST using `std::unique_ptr` for child nodes. Syntax errors are reported before semantic analysis begins.

### AST optimization

The optimizer operates on the AST before semantic analysis. Current transformations include recognizing suitable loops for the GSU hardware `LOOP` instruction and simplifying selected small arithmetic operations.

### Semantic analysis

The analyzer resolves functions, scopes, variables, structures, ROM data, types, pointer operations, and control-flow-related semantic rules. It also calculates stack offsets and local allocation sizes used by the backend.

### IR lowering and verification

`IRLowerer` converts the analyzed AST into a typed, control-flow-aware IR. `IRVerifier` checks structural invariants before code generation. This keeps target-independent compiler structure separate from GSU byte encoding.

### Target backends

The default object path uses `IRCodeGenerator`. It consumes only verified IR plus analyzed symbol/data information and emits GSU instructions into the project object format.

The `AssemblyGenerator` remains available for human-readable assembly export. It is useful for inspection and for the `discas` workflow, but it is a separate textual backend and should not be treated as the canonical implementation of every high-level feature.

## Object and link stages

Each compilation unit can produce a relocatable `.o` file. The object stores code, ROM data, exported symbols, and relocation records. `discld` verifies that all input objects use the same target configuration, concatenates code and data sections, resolves symbols, applies relocations, and writes the final payload.

The linker currently lays out all code before all data. Symbol addresses are calculated from the configured code start address and the accumulated section offsets.

## GSU ABI conventions

The compiler targets the GSU instruction set and ABI described in Nintendo's official SNES Development Manual, Book II, Super FX section. The generated code follows the project's documented GSU conventions while keeping external assembly support optional.

The relevant register conventions used by the compiler are:

| Register | Convention |
| --- | --- |
| `R0` | expression result and first return-value register |
| `R9` | frame pointer used by generated functions |
| `R10` / `SP` | stack pointer |
| `R11` | link/return address register |
| `R12` | hardware-loop counter when a `LOOP` is emitted |
| `R13` | hardware-loop target register when required by setup |
| `R14` | ROM buffer/address register for ROM reads |
| `R15` / `PC` | program counter and call target register |

Generated functions save the link and frame registers, establish `R9` as the frame pointer, allocate aligned local storage, and restore the frame before returning. Parameters use positive frame-pointer offsets beginning at `FP + 4`; locals use negative offsets. Stack arguments are word-aligned.

## Target configuration

The object format carries the memory mapping and code start address. The supported mappings are `LoROM` and `HiROM`. The linker rejects a set of input objects when their target configurations are incompatible.

Source-level configuration is set with directives such as:

```c
set memory_mapping = lorom;
set code_start_address = 0x8000;
```

## Ownership and stability model

AST nodes own their child nodes. IR graphs do not store pointers into resizable instruction or block vectors; values and blocks are referenced by stable numeric IDs owned by an `IRFunction`. The backend builds short-lived lookup tables while processing one function and does not make the IR own target byte buffers.

This separation is intentional: the AST and IR are compiler-phase data, while `ObjectFile` owns the emitted code/data vectors and serialized object contents.

## Current boundaries

The project is pre-release compiler infrastructure. Register allocation is still conservative and generated code relies heavily on `R0`, stack operations, and scratch registers. The assembly-export path also has narrower feature coverage than the IR binary backend for some advanced constructs. These limitations should be considered when using `--emit-asm` as a source of hand-edited assembly.
