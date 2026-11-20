# Security Policy

DiscoC is experimental, unaudited compiler infrastructure under active
development. Compiler input is treated as untrusted: a source file, assembly
file, object file or project manifest must never be able to crash the tools,
corrupt memory, hang them or make them write outside their declared outputs.
We take every report seriously and credit researchers publicly.

## Supported versions

| Version | Supported |
| --- | --- |
| Latest release, [v0.3.0](https://github.com/DiscoLabOfficial/DiscoC/releases/tag/v0.3.0) | Yes |
| `main` branch | Yes |
| Older releases and release candidates | No |
| Experimental DOS cross-build | No (best effort) |

Fixes land on `main` and ship in the next release.

## Reporting a vulnerability

**Please do not open a public issue, pull request or discussion for security
problems.**

Report privately through either channel:

- GitHub [private vulnerability reporting](https://github.com/DiscoLabOfficial/DiscoC/security/advisories/new)
- Email **[security@discolab.dev](mailto:security@discolab.dev)**

Include:

- A description of the issue and its impact
- The affected tool (`discc`, `discld`, `discas`, the build helpers or a
  release package), and the release or commit hash
- The host platform and the exact command line
- A minimized input that reproduces it (source, assembly, object or manifest),
  a sanitizer report or a failing test
- Whether you want to be credited, and under what name or handle

## What to expect

| Step | Target time |
| --- | --- |
| Acknowledgement of your report | within 72 hours |
| Initial assessment and severity | within 7 days |
| Status updates | at least every 14 days |
| Fix and coordinated disclosure | within 90 days, sooner for critical issues |

We will work with you on the disclosure timeline. If a fix needs more time, we
will explain why and agree on a new date with you.

## Scope

### In scope

- **Memory safety:** out-of-bounds access, use-after-free, other undefined
  behavior or sanitizer reports in `discc`, `discld` or `discas` triggered by
  their inputs
- **Resource exhaustion:** hangs, unbounded recursion, stack overflow or
  unbounded allocation from a small or malformed input
- **Filesystem:** a source, import, object or manifest that makes a tool read
  or write outside the files it was given or declares, or overwrite its own
  inputs
- **Release integrity:** published release packages that do not match their
  `SHA256SUMS.txt`, or build helpers that modify system or shell settings

### Out of scope

- Miscompilations and wrong diagnostics with no memory-safety or filesystem
  impact: please open a normal [issue](https://github.com/DiscoLabOfficial/DiscoC/issues/new/choose)
- The behavior of generated SNES code on the console or in an emulator, which
  has no security boundary
- Third-party tools the tests can use, such as WLA-DX, Mesen and the Luna
  emulator, unless DiscoC itself invokes them unsafely
- Unsupported versions listed above
- Social engineering, phishing or physical attacks
- Reports from automated scanners without a demonstrated impact

## Safe harbor

We will not pursue legal action against anyone who, in good faith:

- Tests only on their own machines and copies of the toolchain
- Does not access or modify other people's systems or data
- Gives us reasonable time to fix the issue before public disclosure

If you are unsure whether something is allowed, ask first at
[security@discolab.dev](mailto:security@discolab.dev).

## Recognition

The project has no bounty budget. Every valid report is credited in the
published advisory and the release notes, unless you prefer to stay anonymous.

## Fuzzing

The repository includes libFuzzer targets for the lexer, parser, assemblers,
object reader, manifest parser, semantic analysis and GSU code generation.
See [testing](docs/testing.md#optional-libfuzzer-targets) to run them.

Thank you for helping keep DiscoC and its users safe.
