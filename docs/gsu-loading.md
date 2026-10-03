# GSU loading, startup, and final assembly

DiscoC produces a fixed-origin GSU payload, not a self-relocating routine or a
complete SNES ROM. The execution address and the ROM location used to store
the payload are separate. WLA-DX `.incbin` can store the bytes in a ROM section;
copying those bytes later does not adjust calls, long jumps, or data addresses.

## Choose the execution address at link time

```sh
discc main.dc -o main.o
discc math.dc -o math.o
discld main.o math.o --origin 0x706000 --init-runtime \
  --ram-bank 0 --stack-pointer 0x2000 --entry main \
  --emit-asm final.s -o payload.bin
```

Copy the first byte to `$70:6000` and enter it with `PBR=$70`, `R15=$6000`.
The host must configure clock/screen registers as appropriate, grant GSU RAM
access, and coordinate CPU/GSU ownership. If loading elsewhere, relink with
that execution origin. `--origin` replaces the effective origin of every
input object before layout and relocation. Target and cartridge mapping must
still agree; LoROM remains the default mapping. The entire payload must fit
in one accessible GSU program bank.

For the original multi-file example's local `result`, this origin changes calls
to `$6000 + function offset`, rather than the default `$8000 + function offset`.
The bootstrap adds 12 bytes and relocations include that addition.

## Optional runtime initialization

Without `--init-runtime`, the linker preserves the host-owned startup contract
and adds no bytes. The caller must initialize the data/stack RAM bank and an
aligned, valid R10 before entering generated functions.

With `--init-runtime`, the linker prepends these instructions:

```asm
iwt r0, #0       ; --ram-bank: 0 selects $70, 1 selects $71
ramb            ; ALT2; GETC encoding, uses source-register bit zero
iwt r10, #$2000 ; --stack-pointer
iwt r15, #entry ; relocated --entry, default main
nop             ; required prefetched delay slot
```

`PBR` selects the instruction bank. `RAMBR` independently selects the RAM bank
used by loads, stores, and stack accesses. RAMB uses the low bit of its source
register: `$70` also selects bank 0, but the CLI deliberately accepts only 0/1.
The [Mesen GSU instruction implementation](https://github.com/SourMesen/Mesen2/blob/master/Core/SNES/Coprocessors/GSU/Gsu.Instructions.cpp)
provides a cross-check of this bank-bit behavior.

The bootstrap clobbers R0 and sets R10/RAMBR; it does not initialize PBR, ROMBR,
SCMR, cache, interrupts, or the SNES host. Enter at the **payload start**, not
at `main`, to run the bootstrap. The entry must name an exported CODE symbol
before the end of the combined CODE section. `--entry`, `--ram-bank`, and
`--stack-pointer` require `--init-runtime`.

R10 must be even and in `$0008-$FFFE`; the default is `$2000`. The initial
stack word must not overlap the linked payload in the selected RAM bank.
This is not a complete stack reservation or recursion-depth proof: the host
must reserve enough descending stack space for all frames, temporaries, and
nested calls, without overlapping code, result buffers, or graphics data.
Select bank 1 only on hardware with that RAM capacity.

`main` ends in STOP. Its frame remains available for inspection; a local variable
is not a stable public result API. Prefer a caller-agreed RAM result buffer,
as in [the RAM-result example](../examples/ram_result/README.md). Read results
only after GSU completion and after restoring the appropriate CPU RAM access.

## Two assembly exports, one backend

`discc --emit-asm -o unit.s` exports the canonical IR backend's **relocatable**
encoded object. Symbols and relocations remain symbolic. Assembling it with
`discas` and linking with identical options must produce the same payload as
direct compilation, byte for byte, including delay slots and frame operations.
Changing the link origin legitimately changes relocation bytes.

`discld --emit-asm final.s` exports the **complete linked payload**, including
optional startup, all input CODE/DATA, and resolved numeric addresses. Verify:

```sh
discas final.s -o final.o
discld final.o -o reconstructed.bin
```

`reconstructed.bin` is byte-identical to `payload.bin`. Do not pass
`--init-runtime` again or change the origin: those addresses are already fixed.
Uncommon instruction/prefix combinations may be represented by explicit
`.byte` directives to preserve exact encoding.

The syntax is DiscoC's assembler dialect, not a native WLA-DX export. WLA-DX
integration via `.incbin` is supported as a storage/loading technique, but
native WLA-DX source generation and execution at arbitrary unknown offsets
require separate implementations. Neither is implied by `--emit-asm`.
