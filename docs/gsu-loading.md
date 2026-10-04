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
Without static RAM data, the bootstrap adds 12 bytes. Static initialization
adds instructions; layout/relocations account for the complete startup size.

These settings can also live in `discoc.toml`: `discc build` compiles its sources
and links them, or `discld main.o math.o --config discoc.toml` applies the same
defaults to existing objects. CLI options override manifest values. See
[project manifests](project-manifest.md); fixed-origin loading requirements
remain unchanged.

## Optional runtime initialization

Without `--init-runtime`, the linker preserves the host-owned startup contract
and adds no bytes. The caller must initialize the data/stack RAM bank and an
aligned, valid R10 before entering generated functions.

With `--init-runtime` and no globals, the linker prepends these instructions:

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

The bootstrap clobbers R0 (also R1/R2 with globals) and sets R10/RAMBR;
it does not initialize PBR, ROMBR,
SCMR, cache, interrupts, or the SNES host. Enter at the **payload start**, not
at `main`, to run the bootstrap. The entry must name an exported CODE symbol
before the end of its object's CODE section. `--entry` and `--stack-pointer`
require `--init-runtime`; `--ram-bank` also selects the pointer ABI's near bank
when startup is host-owned.

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

## Static RAM globals

Compiler RAM globals/arrays/structures use a separate object initialization
image. The linker combines images with word alignment and assigns addresses
from `--ram-origin` (default $0400, even) in `--ram-bank`. It checks the whole
allocation against bank bounds and payload overlap. With generated startup,
the allocation must end below the initial stack pointer.

`--init-runtime` clears allocated bytes, then writes nonzero initial values
before jumping to the entry. It needs no ROM-data reads, so RAM code works with
host ROM ownership. Code size grows with nonzero initial words and must still
fit the program bank. Restarting at the bootstrap resets globals again.

Without generated startup, nonempty RAM requires
`--host-initialized-globals`: the host must establish the data/stack bank,
initialize the full allocation, and enter with a valid stack. This flag and
`--init-runtime` are mutually exclusive. No public RAM-image loader format is
provided. Generated functions/pushes check a lower stack floor at the aligned
end of static RAM (or zero without globals), stopping with R6=2 before collision.
This does not prove recursion depth or protect arbitrary host buffers/code
below the stack; host-side memory planning remains necessary.

## Near/far data-bank contract

`--ram-bank 0|1` chooses the original near RAM/stack context, default 0 ($70).
Without bootstrap initialization, the host must establish that exact RAMBR;
far operations restore this link-time context, not an arbitrary entry register
value. `--rom-bank` accepts a GSU ROM bank $00-$5F. It defaults to the execution
bank for ROM code and $00 for RAM code. Compiler near ROM reads select this bank,
and far ROM reads restore it after accessing the foreign bank. These settings
do not place constants in a separate ROM section for RAM-execution payloads.

Far data pointers occupy four aligned bytes: canonical bank ($70/$71 for RAM),
zero padding, low offset byte, high offset byte. Far accesses restore the near
data bank before stack traffic or calls. PBR is not changed by these accesses;
interbank code calls are not supported by this ABI.

Checked-pointer programs diagnose statically invalid addresses and stop on
dynamic alignment, bank-domain, span, or narrowing failures. At a fault STOP,
R6 contains the [language-specification failure category](language-spec.md#nearfar-data-pointers).
The host must inspect it and treat the stop as failure; do not resume after it
or assume the stack was unwound. Generated `main` clears R6 on normal
completion. R6 is scratch while running. Validation cannot detect actual RAM
capacity, aliasing with live code, dangling pointers, or exhausted host-reserved
stack space that is still inside the GSU address window.

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

`reconstructed.bin` is byte-identical to `payload.bin`. The final listing already
contains static initialization and resolved RAM addresses; it does not emit a
second RAM image. Do not pass
`--init-runtime` again or change the origin: those addresses are already fixed.
Uncommon instruction/prefix combinations may be represented by explicit
`.byte` directives to preserve exact encoding.

The syntax is DiscoC's assembler dialect, not a native WLA-DX export. WLA-DX
integration via `.incbin` is supported as a storage/loading technique, but
native WLA-DX source generation and execution at arbitrary unknown offsets
require separate implementations. Neither is implied by `--emit-asm`.
