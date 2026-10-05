# DiscoC Roadmap

DiscoC is a freestanding systems language and cross-compilation toolchain for
specialized hardware. The current executable backend is SuperFX (GSU). The
first release milestone is its reproducible v0.1.0 baseline; performance work
and an executable SPC700 backend follow it, not the other way around.

This roadmap separates implemented foundations from planned work. A checked
item means implemented within its documented limits, not production-ready or
complete support for every target. Milestones describe dependencies and
acceptance criteria, not release dates or a fixed number of pull requests.

## Milestone v0.1.0: DiscoC 0.1 Stabilization

v0.1.0 means the first functional, reproducible DiscoC toolchain for SuperFX,
not an already optimized compiler or completed multi-target implementation.
The ordered consolidation work is:

1. [x] Freeze **DiscoC Language Baseline 0.1**; allow correctness/diagnostic fixes
   but defer new keywords, types and major features. Keep `cursor.x/y` canonical
   and diagnose the obsolete plotting/placement APIs.
2. [x] Make [language-spec.md](docs/language-spec.md) the normative contract for
   integers/conversions, qualifiers, banked pointers, aggregates, globals,
   linkage/imports, control flow, graphics, bitmap metadata and capabilities.
3. [x] Extend positive/negative [language conformance](tests/language/README.md),
   especially feature combinations, and label it separately from backend tests.
   Compiler crashes/timeouts and internal verifier errors are not valid rejections.
4. [x] Harden GSU graphics state and verification: cursor snapshots/updates,
   PLOT auto-X, observable RPIX/flush, COLOR versus ROM-buffer GETC, POR options,
   all bitmap/OBJ depth/size combinations and final planar allocation boundaries.
5. [x] Publish a tracked [SNES triangle example](examples/snes/triangle/README.md)
   with `main.dc`, `discoc.toml`, host ASM, portable build and independent Mesen
   checks. Consume linked origin/SCBR/SCMR metadata and compare project/direct/
   assembled payloads byte for byte; retain the deliberate red-screen failure test.
6. [x] Consolidate toolchain UX: `cmake --preset release`, its build/test presets,
   Windows MinGW static variants, predictable `<build-dir>/bin` executables and
   `discc build` from a documented `discoc.toml`; exercise platform paths in CI.
7. [x] Harden supported linker/object/runtime contracts: bounded versioned
   parsing/writing, relocation spans/targets and per-object bases, metadata,
   placement/overlap diagnostics, known stack floors and frame constraints.
   Preserve output on rejected configurations and execute dynamic stack guards.
8. [ ] Close the internal **v0.1.0-rc** [release-readiness checklist](docs/release-readiness-0.1.md):
   retain fresh local evidence and require green Windows/MinGW, Ubuntu, macOS
   and sanitizer CI for the candidate commit. A configured CI job is not a passed
   platform build. Publication still requires explicit maintainer permission.

Acceptance: spec == parser == analyzer == verified IR == executable GSU backend
== examples within the documented limits; reproducible native tests and the
complete SNES example pass. See [baseline-0.1.md](docs/baseline-0.1.md) for exact
commands, release/object compatibility, evidence boundaries and non-goals.

Release publication is a separate maintainer action after candidate CI and
review pass. No release/tag is created by completing this implementation work.

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
- [x] Stateful lexical plotting with `cursor.x`/`cursor.y`, hardware X advancement,
  ROM-aware color selection, symbolic POR options, and observable RPIX/flush.
- [x] Validated bitmap/OBJ declarations and linked SCMR/SCBR host metadata,
  with framebuffer overlap checks and bitplane-level execution regressions.
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

The GSU model checks functional graphics and instruction-cache state, not a
complete SNES, cycle timing, bus ownership or interrupts. Static and rotating
complete-ROM examples have separate independent Mesen evidence; physical
hardware remains unverified. Fuzzing entry points are not completed campaigns.

## After v0.1.0: Hardening and Deployment Visibility

Make generated payloads easier to embed, inspect, and validate in real projects.

- [ ] Add linker maps/symbol reports showing code/data addresses, static RAM
  placement, entry points, and runtime configuration.
- [ ] Run longer reproducible fuzz campaigns and retain minimized failures,
  preserving sanitizer failure visibility.
- [ ] Make final output replacement resilient to I/O failures; current project
  builds protect inputs and stop on diagnostics but are not transactional.
- [ ] Add physical-hardware results and more host/cartridge configurations to
  the existing independent emulator validation.
- [ ] Expand graphics/cache/bus integration tests without presenting the
  instruction model as a complete SNES oracle.

Acceptance: a developer can locate linked symbols and reserved storage and
reproduce additional host/hardware configurations beyond the baseline example.
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
