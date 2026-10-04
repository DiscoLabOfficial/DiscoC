# DiscoC Roadmap

DiscoC is a freestanding systems language and cross-compilation toolchain for
specialized hardware. The current executable backend is SuperFX (GSU); SPC700
is the next target under development.

This roadmap separates implemented foundations from planned work. A checked
item means implemented within its documented limits, not production-ready or
complete support for every target. Milestones describe dependencies and
acceptance criteria, not release dates or a fixed number of pull requests.

## Implemented Foundations

### Language and frontend

- [x] Lexer, parser, semantic analysis, lexical scopes, and stable resolved symbol IDs.
- [x] Signed/unsigned byte and word values, bool, strict conversions, and defined modular overflow.
- [x] Bitwise operators, checked shifts, short-circuit logic, and GSU software division/remainder.
- [x] Independent ROM/RAM address spaces, constness, and volatile accesses preserved through IR.
- [x] Near/far data pointers, checked casts/arithmetic/indexing, mandatory word alignment, and near-bank restoration.
- [x] Memory-resident structs and arrays, aggregate initializers, character/string literals, and diagnostics for unsupported aggregate-by-value operations.
- [x] Mutable static RAM, explicit initialization contracts, and internal/export/extern linkage.
- [x] Compile-time constants/enums, layout queries/assertions, and packed/aligned structs.
- [x] Compound updates, real for/continue control flow, and explicit switch fallthrough annotations.
- [x] Lexical plotting with explicit plot coordinates and target-capability checks.
- [x] Attributes, source imports and .dci interfaces, transparent type aliases, and bounded dependency graphs.
- [x] Local-first import search, TOML/CLI import paths, public API projections, and one-object-per-source project discovery.
- [x] Declarative target selection with @cfg; @target remains a target assertion.
- [x] Language warnings and a target-independent conformance suite.

The [language specification](docs/language-spec.md) is the canonical semantic
contract. The ordered language refactoring established this baseline; future
extensions must preserve its integer, qualifier, pointer, and observable-access
rules rather than silently adopt C semantics.

### Compiler, assembler, and linker

- [x] Verified typed IR with explicit basic blocks and control-flow edges.
- [x] Canonical GSU backend, hardware-loop selection, and branch relaxation.
- [x] Linear-scan scalar register allocation using R5/R7/R8.
- [x] Constant switch dispatch and balanced comparison trees for larger dynamic switches.
- [x] Defined GSU stack-frame/call ABI with nested-call regression coverage.
- [x] Standalone discas assembler and byte-equivalent direct/assembly workflows.
- [x] Versioned little-endian objects with CODE/DATA/RAM images, visibility, and alignment metadata.
- [x] Multi-file linking, hardened object parsing, checked relocations, and memory-placement validation.
- [x] Configurable fixed origins, optional runtime initialization, data-bank selection, and stack placement.
- [x] Byte-exact final linked assembly export.
- [x] Bounded TOML manifests, in-process discc build, manifest-relative paths, and explicit CLI overrides.
- [x] Native CMake/direct GCC/Clang build helpers and retained experimental C++14 DOS cross-build configuration.
- [x] SPC700 target/layout foundation, capability diagnostics, and frontend/IR inspection.

GSU binaries are fixed-origin payloads, not complete SNES ROMs or arbitrarily
relocatable routines. SPC700 machine emission is not implemented. See
[GSU loading](docs/gsu-loading.md) and the
[SPC700 foundation](docs/spc700-target.md) for the current boundaries.

### Libraries and verification

- [x] Freestanding near-RAM byte copy/move/fill modules.
- [x] Library-first signed Q8.8/Q12.4 fixed-point arithmetic with explicit scaling.
- [x] Portable core modules separated from GSU graphics wrappers.
- [x] CTest language, negative-diagnostic, ABI, multi-file, placement, and project-manifest regressions.
- [x] Instruction-level GSU execution checks and assembly/object/payload byte comparisons.
- [x] Native Debug/Release CI, a sanitizer job, and optional source/assembly/object/manifest fuzzing entry points.

The GSU model does not validate complete SNES behavior, graphics, cache timing,
bus ownership, or interrupts. Fuzzing entry points do not imply completed fuzz
campaigns. Independent emulator/hardware results remain separate evidence.

## Next Milestone: Consolidate the Refactored Baseline

Prepare a reviewable pre-release baseline before expanding language semantics.

- [ ] Audit the specification, README, examples, library interfaces, CLI help,
  and manifest schema together; distinguish supported features from proposals.
- [ ] Extend boundary and negative cases where the conformance suite still has
  gaps, especially qualifier conversions, aggregate layout, imported interfaces,
  and target-capability rejection.
- [ ] Run reproducible fuzz campaigns, retain minimized failures as regressions,
  and keep sanitizer failures visible.
- [ ] Document release/object-format compatibility and how users rebuild
  objects when upgrading the toolchain.
- [ ] Validate fresh builds and documented examples across native build paths;
  retain the experimental DOS compatibility checks.
- [ ] Make final output replacement resilient to I/O failures. Current project
  builds protect inputs and stop on diagnostics, but are not transactional.

Acceptance: documented workflows succeed from a clean checkout, regressions and
sanitizers pass, failures return actionable diagnostics, and release notes
identify remaining target/runtime limitations. Adding more language features
is not a prerequisite for this consolidation.

## Next Milestone: GSU Integration and Deployment Visibility

Make generated payloads easier to embed, inspect, and validate in real projects.

- [ ] Add linker maps/symbol reports showing code/data addresses, static RAM
  placement, entry points, and runtime configuration.
- [ ] Add a tracked, reproducible SNES host example that loads a RAM payload,
  starts the GSU, checks a host-visible result after STOP, and reports success
  or failure.
- [ ] Record independent emulator validation alongside the instruction-model
  tests; add hardware results when available.
- [ ] Expand graphics/cache/bus integration tests without presenting the
  instruction model as a complete SNES oracle.

Acceptance: a developer can locate linked symbols and reserved storage, build
the host example from documented commands, and reproduce its expected result.
Keep fixed-origin loading and host memory ownership explicit.

## Next Executable Target: SPC700

The foundation is implemented. The following stages are planned; partial
backend support must diagnose unsupported lowering instead of falling back to
GSU emission or changing shared language semantics.

### 1. Assembler and object relocations

- [ ] Implement SPC700 instruction encoding and operand validation.
- [ ] Define target-specific relocations and branch-range handling.
- [ ] Integrate assembly/object metadata with the existing target discriminator.
- [ ] Add golden encodings, malformed-input tests, and assembly round trips.

Acceptance: supported instructions assemble to independently checked bytes;
invalid operands, wrong-target objects, and unrepresentable relocations fail
deterministically.

### 2. ABI, runtime, and linker placement

- [ ] Finalize scalar returns, argument passing, register preservation, and
  caller/callee cleanup from the initial ABI proposal.
- [ ] Keep compiler-managed locals/arguments separate from the hardware
  return-address stack, with explicit memory reservations.
- [ ] Define code, mutable data, static initialization, entry, and runtime
  placement in the shared address space.
- [ ] Protect MMIO, stack, and IPL-related regions according to the selected
  runtime contract; add target settings only after that contract exists.

Acceptance: multi-object layout, static initialization, nested calls, stack
ownership, and invalid overlaps have positive and negative tests. Do not reuse
GSU-specific banks or invent manifest settings ahead of implementation.

### 3. IR lowering and executable language coverage

- [ ] Lower byte/word/bool values, RAM loads/stores, comparisons, branches,
  direct calls, and returns from the shared verified IR.
- [ ] Preserve signedness, modular arithmetic, short-circuit evaluation, and
  observable volatile access on an 8-bit backend.
- [ ] Extend operators, pointers, globals, arrays, and structures in tested
  increments; maintain an explicit supported-lowering matrix.
- [ ] Validate portable core library modules through execution, not just
  frontend acceptance.

Acceptance: the same supported language programs produce the same observable
results as the GSU backend, with explicit capability exceptions. Direct object
and assembled paths agree on linked bytes and behavior.

### 4. Independent execution and SNES integration

- [ ] Add an SPC700 execution test path and validate it against an independent
  emulator/reference implementation.
- [ ] Supply a minimal host upload/start/communication example.
- [ ] Test runtime state, nested calls, repeated execution, and volatile
  communication accesses end to end.

Acceptance: an independently executed linked payload produces the documented
result and can be run repeatedly without corrupting reserved state. Frontend
checks alone do not establish an executable backend.

### 5. Target libraries

- [ ] Add communication-port, timer, and DSP access modules with explicit
  volatile and host-ownership contracts.
- [ ] Build a small audio example after the runtime and hardware-access path
  are verified.
- [ ] Measure direct-page/register use and optimize only with execution
  regressions and reproducible benchmarks.

Target-specific libraries must remain separate from the portable freestanding
core. GSU plotting is not an SPC700 language capability.

## GSU Performance Work

These improvements follow correctness and deployment validation.

- [ ] Improve spill/rematerialization heuristics and evaluate allocation for
  checked-pointer functions, which currently use explicit spill slots.
- [ ] Evaluate dense-switch jump tables using code-size and execution
  measurements, preserving explicit fallthrough behavior.
- [ ] Optimize integer/fixed-point helpers and repeated bank-selection patterns
  without removing observable accesses or required pointer checks.
- [ ] Establish reproducible code-size and measured timing benchmarks before
  claiming speedups; the instruction model is not a cycle-accurate benchmark.

## Later Language, ABI, and Placement Extensions

- [ ] **Aggregate semantics:** specify copying, aliasing, and aggregate
  parameters/returns, including an explicit hidden-result-pointer ABI where
  appropriate, before enabling currently rejected operations.
- [ ] **Inline assembly:** implement typed inputs/outputs, tied operands,
  clobbers, memory effects, and target-state interference through verified IR.
  Opaque text-only assembly is not sufficient.
- [ ] **ABI attributes:** enable interrupt/naked/section/bank/calling-convention
  attributes only after target-specific entry/exit and placement contracts.
- [ ] **Far code and symbol placement:** define interbank calls/returns and
  named far-object placement independently of the existing far data-pointer ABI.
- [ ] **RAM code with ROM constants:** support separately placed code/constant
  sections with bank-aware metadata and checked relocations.
- [ ] **Native WLA-DX export:** preserve final encodings and addresses, with
  round trips checked using the external assembler. Current output remains
  DiscoC assembly; binary .incbin integration is already possible.
- [ ] **Flexible loading:** evaluate position-independent code versus explicit
  load-time relocation metadata. Relative branches alone do not relocate
  absolute calls or data addresses.
- [ ] **Memory layouts:** add richer section/region placement separately from
  the simple project manifest; keep placement out of source-language directives.
- [ ] **Core expansion:** add explicit far/ROM/volatile memory APIs where useful
  and wider fixed-point arithmetic after a defined 32-bit value/return ABI.
- [ ] **Systems types:** introduce function pointers and unions only with
  defined calling, representation, aliasing, and far-code contracts.
- [ ] **Project UX:** consider dependency-aware incremental builds after
  interface/source/configuration invalidation rules are specified.
- [ ] **Logical modules:** evaluate named imports/namespaces independently of
  filenames, with explicit visibility and compatibility rules; retain file
  imports without introducing a textual preprocessor.
- [ ] **Later numerics:** keep BF16/FP16 below deterministic integer and
  fixed-point work.

See [library semantics](docs/freestanding-library.md) and
[ASM/ABI design contracts](docs/inline-assembly-contract.md). Reserved syntax or
a proposal does not imply implemented semantics.

## Scope and Completion Rules

Shared language semantics, reusable IR, explicit target capabilities, and
portable/target library separation should guide additional processor support.
Each backend needs its own instruction, ABI, relocation, runtime, and execution
evidence; recognizing a target name is not enough.

ISO C compatibility, a textual C preprocessor, classes, exceptions, generics,
automatic heap allocation, and variadic functions are not goals of the current
language refactoring.

Every implementation milestone should include its specification/diagnostic
changes, positive and negative regressions, boundary cases, and appropriate
execution or byte-equivalence evidence. Keep documentation synchronized with
what actually works; split work into bounded, reviewable changes.
