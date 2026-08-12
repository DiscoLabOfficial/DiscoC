# DiscoC: A C++23 Toolchain for SuperFX (GSU)

[![CMake](https://github.com/DiscoLabOfficial/DiscoC/actions/workflows/cmake.yml/badge.svg)](https://github.com/DiscoLabOfficial/DiscoC/actions/workflows/cmake.yml)

DiscoC is a custom **systems programming language and complete toolchain** (Compiler, Assembler, and Linker) targeting the **Super Nintendo's SuperFX (GSU)** RISC co-processor.

Written in **Modern C++ (C++23)**, this project aims to bring modern development practices to legacy hardware constraints. It features a custom **Relocatable Object File format**, a dedicated Linker for memory mapping resolution, and hardware-specific optimizations.

> **Status: Pre-Alpha / Experimental**
> This project is currently a work-in-progress. While fully functional (compiles and links binaries), the generated assembly focuses on correctness over speed. Advanced register allocation is on the roadmap.
> The toolchain produces relocatable objects and a linked GSU payload. The final `.bin` still needs to be embedded into a valid SNES ROM image.

## Features

### Language Features
*   C-like syntax (`if`, `for`, `while`, variables, functions)
*   `word` (16-bit) and `byte` (8-bit) integer types
*   Full 24-bit pointer support with or without `far` keywords
*   `struct` definitions with member access
*   `rom const` for placing data directly into the ROM
*   Direct hardware access via special keywords (`plot`, `set_color`, etc.)

### Compiler & Toolchain
*   **Two-Stage Build Process:** A separate compiler (`discc`) and linker (`discld`) for multi-file projects.
*   **Assembly Export:** Inspect the compiler's output with the `--emit-asm` flag.
*   **Standalone Assembler:** `discas` consumes the emitted ca65-like `.s` syntax and produces the same relocatable `.o` format used by `discc`.
*   **Intermediate Representation and CFG:** `discc --emit-ir` prints the verified, target-independent IR and its basic blocks/control-flow graph. Normal object compilation verifies this stage before the IR backend emits GSU code.
*   **Hardware-Specific Optimizations:**
    *   Recognizes `for` loops and converts them to the ultra-fast `LOOP` instruction.
    *   Optimizes small integer arithmetic into single-byte immediate instructions.
*   **Custom Object File Format** for relocatable code.

### Architecture & Engineering Highlights

This is not just a parser; it is a full-stack compiler implementation including:

*   **Custom Object File Format:** Designed a binary format supporting code/data sections, symbol tables, and relocation entries (patching `JAL` targets and global variable addresses at link-time).
*   **The `discld` Linker:** A custom linker built from scratch to handle symbol resolution, memory placement, and binary patching across multiple compilation units.
*   **Peephole Optimization:** The compiler analyzes the AST to detect high-level patterns (like decrementing `while` loops) and emits specialized hardware instructions (`LOOP` opcode) for zero-overhead looping.
*   **Manual ABI Management:** Implements a full stack frame convention (Frame Pointer `R9`, Stack Pointer `R10`) to support recursion and local scope management.
*   **Numerics Roadmap:** Planned software-defined **BF16 (Brain Float 16)** and **FP16** support for modern numerics on a 16-bit integer chip.

## "Hello, World!" (The "Add" Example)

Here is a simple example of a DiscoC program with two functions.

```c
// main.dc

// Forward declare the function from another file.
word add(word a, word b);

void main() {
    word result = add(30, 12);
    // After this, R0 would hold the value 42.
}
```

```c
// math.dc

word add(word a, word b) {
    return a + b;
}
```

## Building from Source

This project uses C++23 and CMake.

```bash
# 1. Clone the repository
git clone https://github.com/DiscoLabOfficial/DiscoC.git
cd DiscoC

# 2. Configure the build using CMake
mkdir build
cd build
cmake ..

# 3. Build the executables (discc, discas, and discld)
cmake --build .
```

After a successful build, the `discc`, `discas`, and `discld` executables will be located in the `build/Debug` or `build` directory.

## Usage

The DiscoC toolchain uses a two-step compile-and-link process.

**1. Compile directly:** Use `discc` to compile each `.dc` source file into an intermediate `.o` object file.

```bash
# Compile main.dc into main.o
./discc ../main.dc -o main.o

# Compile math.dc into math.o
./discc ../math.dc -o math.o
```

**2. Assemble emitted assembly (optional):** Use `--emit-asm` followed by `discas` when you want to inspect or hand-edit the assembly before linking.

```bash
./discc ../main.dc --emit-asm -o main.s
./discas main.s -o main.o

./discc ../math.dc --emit-asm -o math.s
./discas math.s -o math.o
```

**3. Inspect the IR/CFG (optional):** Use `--emit-ir` to print the verified intermediate representation without producing an object file.

```bash
./discc ../main.dc --emit-ir
```

**4. Link:** Use `discld` to link all your `.o` files into a final GSU payload.

```bash
# Link main.o and math.o into the final GSU payload
./discld main.o math.o -o my_game.bin
```

The resulting `my_game.bin` is a linked SuperFX/GSU payload, not a complete runnable SNES ROM. It must still be placed into a valid ROM image with the appropriate SNES header, mapping configuration, startup integration, and host-side resources before it can be executed by an emulator or console.

## Project Roadmap

This project is not as actively developed. You can view our roadmap and upcoming features in the `ROADMAP.md` file.

## Technical Documentation

Detailed design and format documentation is available in the [`docs/`](docs/) directory:

* [`architecture.md`](docs/architecture.md) — compiler pipeline, backends, ABI, and ownership boundaries.
* [`ir-cfg.md`](docs/ir-cfg.md) — the typed intermediate representation and control-flow graph model.
* [`object-format.md`](docs/object-format.md) — relocatable object-file layout and linker behavior.
* [`testing.md`](docs/testing.md) — CTest regressions, sanitizer checks, and optional fuzzing.
* [`assembler.md`](docs/assembler.md) — the `discas` input language and supported assembly workflow.

## License

DiscoC is free software: you can redistribute it and/or modify it under the terms of the **GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.**

This program is distributed in the hope that it will be a useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
