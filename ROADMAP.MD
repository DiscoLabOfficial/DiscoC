# Project Roadmap & Technical Debt

DiscoC is an evolving research project. The current focus is on stability and feature completeness, followed by aggressive optimization passes.

## Completed Milestones
- [x] **Frontend:** Recursive Descent Parser & Lexer supporting C-like syntax.
- [x] **Middle-end:** Semantic Analyzer for type checking and scope resolution.
- [x] **IR/CFG:** Verified typed intermediate representation with explicit basic blocks and control-flow edges.
- [x] **Backend:** Code Generator emitting raw SuperFX binary opcodes (no external assembler required).
- [x] **Linker:** Custom `discld` linker handling relocatable addresses and multi-file support.
- [x] **Control Flow:** Implementation of `if`, `while`, `for`, and `switch/case`.
- [x] **Hardware loops:** Automatic detection of loops suitable for the GSU `LOOP` instruction.

## In Progress
- [ ] **Graph-Coloring Register Allocation:**
    - *Current State:* The compiler relies heavily on the stack (`push`/`pop`) and accumulation registers (`R0`).
    - *Goal:* Implement a graph-coloring algorithm (Chaitin-Briggs) to map variables to general-purpose registers (`R1`-`R14`), drastically reducing RAM access.
- [ ] **Switch Statement Optimization:**
    - *Current State:* Uses linear $O(N)$ comparison chains.
    - *Goal:* Implement **Jump Tables** ($O(1)$) or Binary Search lookup for large case sets.
- [ ] **Struct ABI Alignment:**
    - *Goal:* Optimize struct memory layout to align with 16-bit word boundaries automatically.

## Future Goals
- [ ] **Standard Library (libc-ish):**
    - `memcpy`, `memset` optimized for GSU `PLOT` circuitry.
    - Software-defined floating point math library (BF16/FP16).
- [ ] **Inline Assembly:** Allow embedding raw ASM instructions for critical sections.
