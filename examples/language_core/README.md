# Language core and static RAM example

This two-file GSU example combines internal/export linkage, a mutable volatile
global, const RAM data, memory-resident structs, bool, short-circuit logic,
bitwise arithmetic, software division/remainder, and lexical plot context.
It uses the plot coordinate registers but does not issue graphics instructions.

From the repository root, with the tools on PATH:

```sh
discc examples/language_core/main.dc -o main.o
discc examples/language_core/math.dc -o math.o
discld main.o math.o --origin 0x706000 --init-runtime \
  --ram-bank 0 --ram-origin 0x0400 --stack-pointer 0x2000 \
  --emit-asm final.s -o language-core.bin
```

Copy the payload to $70:6000, set PBR=$70/R15=$6000, grant GSU RAM access, and
start at the payload beginning to run static/stack initialization. This is
fixed-origin code, not a complete SNES ROM or a position-independent routine.

After normal STOP (R6=0), a host that has regained RAM ownership can read:

| RAM address | Word result |
| --- | --- |
| $70:0100 | 149 (46 + 103) |
| $70:0102 | 21 (149 / 7) |
| $70:0104 | 1 (ready) |
| $70:0106 | 1 (frame counter) |

The bootstrap resets globals on each entry. To retain state between runs, use
a deliberately host-owned initialization/entry strategy; restarting this
payload at its beginning initializes the counter again. The host must keep
output, static RAM, stack, code, and graphics allocations separate.

The regression suite compares direct, assembled, and mixed-object paths and
checks these results using the instruction model, not physical hardware.
See [language semantics](../../docs/language-spec.md) and
[GSU loading](../../docs/gsu-loading.md) for the full contracts.
