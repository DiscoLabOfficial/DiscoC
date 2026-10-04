# Fixed-point and portable memory example

`main.dc` imports the fixed-point and memory interfaces, scales signed Q8.8
values, uses Q12.4 multiplication, and copies four raw unsigned bytes to near
RAM. No floating-point primitive, heap, or target-specific inline assembly is
needed.

From the repository root, with `discc`/`discld` on PATH:

```bash
discc examples/fixed_point/main.dc -o main.o
discc lib/core/fixed.dc -o fixed.o
discc lib/core/memory.dc -o memory.o
discld main.o fixed.o memory.o --init-runtime --emit-asm final.s -o fixed.bin
```

The default payload executes at `$00:8000`, with near RAM/stack in bank `$70`.
After normal STOP, the host can inspect:

| RAM address | Little-endian bytes | Meaning |
| --- | --- | --- |
| `$70:0100` | `00 03` | Q8.8 raw 768: 1.5 times 2 is 3.0 |
| `$70:0102` | `A0 FF` | Q8.8 raw -96: -0.75 divided by 2 is -0.375 |
| `$70:0104` | `00 00` | Truncation of -0.375 toward zero |
| `$70:0106` | `48 00` | Q12.4 raw 72: 3.0 times 1.5 is 4.5 |
| `$70:0110` | `00 80 FF 2A` | Bytes copied without signed reinterpretation |

Tests execute these expectations through direct, assembled, and mixed objects,
then reassemble the final linked export to check identical payload bytes. This
is instruction-model verification, not a complete ROM or hardware test.

For RAM execution, override the link origin (for example `--origin 0x706000`),
copy the whole payload to that exact address, and configure the host's PBR/PC
and bus access accordingly. The runtime bootstrap initializes RAMBR and R10,
not the SNES host. See [loading](../../docs/gsu-loading.md) and
[library semantics](../../docs/freestanding-library.md).
