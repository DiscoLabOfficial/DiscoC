---
name: Bug report
about: Report build failures, crashes, incorrect diagnostics, or generated-code bugs.
title: "[Bug]:"
labels: bug
assignees: ''
type: Bug

---

## Summary

Describe the problem clearly and briefly.

## Environment

- DiscoC version or commit:
- Operating system and architecture:
- Host compiler and version:
- Build method: CMake / build script / direct compilation / other
- Target: GSU / SPC700
- Affected tool: discc / discas / discld / build system

## Steps to reproduce

1.
2.
3.

Include the exact build, compilation, assembly, and linking commands relevant to the failure:

```text
Paste commands here.
```

For multi-file projects, include all required source files and the object-file order used when linking.

## Minimal reproducer

Provide the smallest source or assembly example that reproduces the problem:

```c
// Paste the reproducer here.
```

If reducing the example is difficult, attach the relevant files or link to a public repository.

## Expected behavior

Describe what should happen. For incorrect execution results, include the expected value or behavior.

## Actual behavior

Describe what happens instead. Include the actual result, diagnostic, crash, or generated output.

```text
Paste complete relevant logs here.
```

Please use text rather than screenshots for commands and diagnostics.

## Generated code, if applicable

For code-generation or runtime bugs, please include any available:

- Assembly generated with `discc --emit-asm` (`.s` file).
- Disassembly of the linked payload, including the disassembler name/version and starting address.
- Relevant object files (`.o`) or linked payload (`.bin`).

If possible, highlight the instructions or addresses where the behavior differs from what you expected.

These files are helpful but not required to submit a report.

## GSU execution context, if applicable

Complete this section only for generated-code or runtime problems:

- Cartridge mapping: LoROM / HiROM
- Execution memory: ROM / cartridge RAM
- Configured code start address:
- Emulator and version, or hardware:
- Relevant host initialization or payload-loading steps:

## Additional context

Add anything else that may help reproduce or diagnose the issue.

Generated assembly, object files, linked payloads, or comparisons with another assembler are welcome when relevant, but are not required.

Do not include credentials, tokens, or private source code.
