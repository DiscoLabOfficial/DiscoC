# DiscoC libraries

Freestanding source modules, explicitly compiled and linked with your program:

- `core/fixed.dci` / `fixed.dc`: signed Q8.8 and Q12.4 arithmetic using word-sized
  storage and full intermediate precision before scaling.
- `core/memory.dci` / `memory.dc`: near RAM byte copy, overlapping move, and fill.
- `targets/gsu/graphics.dci` / `graphics.dc`: GSU pixel/rectangle/flush wrappers.

Import interfaces using paths relative to your `.dc` file. Imports do not link
implementations automatically. The core uses portable DiscoC operations; target
libraries must assert their required processor/capabilities. SPC700 machine
emission and a DSP library are not yet implemented.

See [the library contract](../docs/freestanding-library.md) for build commands,
numeric rounding/overflow, memory preconditions, and current limits.
