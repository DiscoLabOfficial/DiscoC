# DiscoC Relocatable Object Format

DiscoC object files use the `.o` extension and are consumed by `discld`. Both the normal IR backend and `discas` produce this format.

The format is intentionally small and implementation-owned. It is not ELF, SNES object format, or a general-purpose replacement for a platform linker format.

## Binary layout

All multi-byte integer fields are serialized explicitly in little-endian byte
order, independent of the host architecture. The current format version is
6. The format is serialized in this order:

| Field | Encoding |
| --- | --- |
| Magic | 5 ASCII bytes: `DISCO` |
| Format version | `uint8_t`: currently `6` |
| Target | `uint8_t`: `0` = GSU, `1` = SPC700 |
| Memory mapping | `uint8_t`: `0` = LoROM, `1` = HiROM |
| Code start address | `uint32_t` |
| DATA alignment | `uint8_t`: power of two from 1 to 128 (introduced in v4; generalized in v6) |
| RAM alignment | `uint8_t`: power of two from 1 to 128 (version 6) |
| Code section | `uint32_t` byte count, followed by raw code bytes |
| Data section | `uint32_t` byte count, followed by raw data bytes |
| RAM initialization image | `uint32_t` byte count, followed by initial RAM bytes (version 5) |
| Symbol count | `uint32_t` |
| Symbols | Repeated symbol records |
| Relocation count | `uint32_t` |
| Relocations | Repeated relocation records |

### Symbol record

Each symbol record contains:

```text
uint32_t name_length
byte[name_length] name_bytes
uint8_t section
uint32_t offset
```

The section value is 0 for CODE, 1 for ROM DATA, and 2 for RAM. Offsets are
relative to that object's section/image. Names beginning with byte 0x01 are
object-private: relocations can resolve them only in that object. Other
definitions participate in public cross-object lookup. Compiler internal
declarations and non-exported assembler labels use private identities.

### Relocation record

Each relocation record contains:

```text
uint32_t target_name_length
byte[target_name_length] target_name_bytes
uint8_t section_to_patch
uint32_t patch_offset
uint8_t relocation_type
```

The relocation types are:

| Value | Name | Meaning |
| --- | --- | --- |
| `0` | `ADDR16_JAL` | 16-bit function address used by a generated `JAL` sequence |
| `1` | `ADDR16_IWT` | 16-bit address used by an `IWT`/address materialization |
| `2` | `ADDR24_BANK` | Bank byte of a far address |
| `3` | `ADDR24_OFFSET` | 16-bit offset portion of a far address |
| `4` | `ADDR16_RAM` | 16-bit address in the configured near RAM bank (version 5) |

The relocation section identifies the CODE/DATA section containing the
placeholder. RAM-image patch records are not supported. The compiler primarily
emits CODE relocations for calls and global addresses.

## Linker layout

`discld` performs these operations:

1. Reads every input object.
2. Checks that all objects use the same mapping and code start address.
3. Concatenates object code sections in input order.
4. Resolves code symbols against the configured code base.
5. Aligns the DATA start and each input DATA section, then appends their bytes.
6. Resolves data symbols.
7. Applies relocations.
8. Writes code, alignment padding, and data to the output file.

Version 6 allocates each object's RAM image with its declared alignment, from
`--ram-origin` in `--ram-bank`. RAM bytes are not appended to ROM DATA.
`--init-runtime` encodes zeroing/nonzero initialization in startup; otherwise
nonempty RAM requires `--host-initialized-globals`. Public/private RAM targets
use their own object's RAM base, independently of CODE/DATA bases.
`ADDR16_RAM` validates the near RAM bank before narrowing.

Compiler DATA sections request alignment 2 and align their individual objects.
Raw assembly defaults to DATA alignment 1 and RAM alignment 2.
`.define __DISCO_DATA_ALIGNMENT N` and `.define __DISCO_RAM_ALIGNMENT N` request aligned
placement. When needed, the linker inserts a NOP after CODE and zero padding
between DATA sections. Each symbol/relocation uses its object's padded section
base, not an unpadded sum. Padding counts toward the payload's bank limit and is
preserved in final assembly exports. This metadata does not align arbitrary
word declarations inside a hand-written packed section.

For 16-bit address relocations, the linker patches the two bytes after the opcode placeholder. For bank relocations, it patches the byte after the opcode. For offset relocations, it patches the two-byte address field.

Undefined symbols and duplicate definitions are linker errors. Objects with incompatible target configurations are rejected before output is written.

### GSU address and bank validation

`code_start_address` is a **GSU execution address**, not a byte offset inside a
SNES ROM file. The default is `$00:8000`; explicit origins such as `$40:0000`,
`$70:8000`, and `$71:0000` are supported. The linker accepts the documented
GSU-visible windows:

| Region | GSU addresses |
| --- | --- |
| Game ROM | `$00-$3F:$8000-$FFFF` |
| Game ROM mirror | `$40-$5F:$0000-$FFFF` |
| Game Pak RAM | `$70-$71:$0000-$FFFF` |

LoROM/ROM execution remains the default. `discc --execution-memory ram` selects
`$70:8000` unless `--origin` is specified explicitly; it does not
change the cartridge mapping. The full origin carries this selection through
the object format. Both direct compilation and emitted
assembly preserve mapping and origin.

The reference is the [Super FX tutorial's GSU memory map](https://en.wikibooks.org/wiki/Super_NES_Programming/Super_FX_tutorial#From_Super_FX_Point_of_View).
Lower-half ROM mirrors and SNES-CPU-only regions are intentionally outside the
supported placement contract. The RAM window describes addressability, not a
guarantee that a particular cartridge contains 128 KiB of RAM.

The combined code **and** data must fit from the origin through the end of one
64 KiB program bank. A payload whose last byte is `$xx:FFFF` is accepted, but
one more byte is rejected. This check includes all objects, not just individual
sections. The linker does not insert bank padding or generate bank-switching
code. `LoROM`/`HiROM` remain compatibility metadata and default-origin choices,
not commands to change the GSU's hardware address map or create a cartridge ROM.

`ADDR16_JAL` and `ADDR16_IWT` targets must be in the origin's bank; high-bank
origins are valid and encode the target's low 16 bits **after** validation.
`ADDR24_BANK` and `ADDR24_OFFSET` targets must fit 24 bits and identify a supported
GSU region. A one-past-section label is legal in the object format, but cannot
be relocated to an inaccessible address or a different bank by a near relocation.
All placement/relocation checks run before the output file is opened, so a
rejected link does not create or truncate the output.

By default this validation does not initialize `PBR`, `ROMBR`, `RAMBR`, bus
access, or the stack, and does not infer the address space of arbitrary runtime
pointers. `--init-runtime` optionally prepends RAMBR/R10 setup and an entry jump;
its bytes count toward the bank limit and shift all CODE/DATA symbol addresses
and relocation patch locations. `--origin` overrides the effective origin of
all input objects without changing object serialization or the format version.
See [GSU loading](gsu-loading.md) for the startup contract.
Compiler bank restoration uses reserved `__disco_near_ram_bank` and
`__disco_near_rom_bank` bank-byte relocations, resolved from `--ram-bank` and
`--rom-bank`. Input definitions of these symbols are rejected. They are not
ordinary exported objects or executable targets.
Reserved `__disco_stack_limit_<bytes>` references resolve to the aligned static
RAM end plus the required stack space. They enforce frame/push checks before
stack traffic and cannot be defined by input objects. Without RAM allocation
their floor is zero. These checks do not reserve unrelated host resources.

RAM execution requires copying the payload to cartridge RAM and matching the
configured program bank/PC. ROM-qualified accesses still require ROM-resident
data and the appropriate ROM bank; copying bytes to RAM does not turn ROM-buffer
reads into RAM loads. Separate placement of RAM code and ROM constants is not
implemented by the current flat code-then-data layout.

## ROM data

Source declarations such as:

```c
rom const word palette = 0x1234;
```

become data-section bytes and a data symbol. A global address reference emits a relocation so the final data placement can be decided by the linker.

## Output is a payload, not a cartridge ROM

The linker's `.bin` output is the final linked GSU payload for the object set. It is not directly a complete, runnable SNES ROM. A separate ROM integration step must provide the SNES header, correct mapping/header metadata, host-side startup code, cartridge layout, and any required resources.

## Compatibility and evolution

The reader accepts version 3 as packed DATA (alignment 1) and version 4 with
DATA alignment, both without a RAM image. Version 5 includes RAM with default
alignment 2. Writers emit version 6, which records RAM alignment explicitly.
Versions 3/4 cannot contain RAM symbols or relocation type 4. Other versions and
invalid alignment bytes are rejected. RAM images are bounded to 64 KiB. Rebuild
compiler objects when changing the pointer ABI: reading an old container does
not make its old far argument layout ABI-compatible, and cross-unit signatures
are not stored or checked by this format.

The format has no checksum, per-symbol alignment table, or section flags. The
version byte is validated by `ObjectFile::read`. Changes to the
serialization order or enum values require coordinated changes to
`ObjectFile::write`, `ObjectFile::read`, `discld`, and `discas`.
