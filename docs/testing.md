# Testing and Fuzzing

The repository includes an automated CTest suite covering the compiler, linear-scan allocator, switch dispatch, ABI call sequences, textual assembly path, standalone assembler, linker, object-file reader, diagnostics, CFG output, lexical shadowing, and multi-object data relocations.

## Run the regression suite

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The native CI matrix runs these tests for Debug and Release builds. A separate Ubuntu job builds the same targets with AddressSanitizer and UndefinedBehaviorSanitizer enabled.

The `gsu_memory_map` unit test checks documented ROM/RAM windows, invalid
SNES-only regions, exact bank boundaries, arithmetic overflow, and near-target
bank checks. The `gsu_mapping` regression exercises the actual linker with
hand-authored objects and compiler output: low/high-bank origins, all relocation
types, multi-object DATA relocations, combined code/data boundary failures,
unrepresentable targets, and preservation of an existing output on rejection.
Golden payload bytes check the encoded addresses, and the instruction model
checks calls relocated to `$40:8000`, `$70:8000`, and `$71:0000`. That model checks
the PC and execution results, not cartridge RAM capacity, the physical bus,
ROM-file placement, or the SNES host's startup routine.

`target_foundation` checks LoROM defaults, retained HiROM support, optional RAM
execution, directive ordering, explicit origins, region mismatches, and GSU-only
diagnostics. `gsu_execution_memory` checks golden object-header bytes for both
backends, mixed IR/assembly multi-file linking, rejected incompatible origins,
malformed assembly metadata, and relocated call results. It also compares
LoROM/RAM payload bytes when only the bank changes and all references are near.

`assembly_export` checks byte-exact reassembly of all 256 raw opcodes with four
prefix configurations (1,024 encoding cases), plus private/far relocations,
placement, DATA bytes, and malformed export input. These are encoding tests,
not execution coverage for every opcode.

`gsu_runtime_loading` links multi-file programs at `$70:6000`, `$70:0000`, and
`$71:6000`, verifies hand-encoded bootstrap bytes, and starts the instruction
model with deliberately incorrect RAMBR/R10 state. It checks both data banks,
shared RAM code/data storage, nested calls, wrong-offset loading failures,
invalid origins/options, stack-word overlap, output preservation, and final
linked assembly round trips. Additional ABI tests enter `main` when it is not
the first function in CODE. The model implements RAMB's bank-bit selection but
does not emulate cache timing, bus ownership, interrupts, or a complete SNES.

## Optional libFuzzer target

The optional frontend fuzzer exercises the lexer, parser, textual assembler, and in-memory object-file reader. It requires Clang with libFuzzer support:

```bash
cmake -S . -B build-fuzz \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDISCO_BUILD_FUZZERS=ON
cmake --build build-fuzz --target disco_frontend_fuzzer
mkdir -p corpus
./build-fuzz/disco_frontend_fuzzer -max_len=4096 corpus
```

Malformed input is expected to be rejected. A crash, sanitizer report, hang, or unbounded resource use is a compiler defect and should become a minimized regression test under `tests/`.
