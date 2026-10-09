# DiscoC Relocatable Object Format

DiscoC object files use the `.o` extension and are consumed by `discld`. Both the normal IR backend and `discas` produce this format.

The format is intentionally small and implementation-owned. It is not ELF, SNES object format, or a general-purpose replacement for a platform linker format.

## Binary layout

All multi-byte integer fields are serialized explicitly in little-endian byte
order, independent of the host architecture. The current format version is
7. The format is serialized in this order:

Version 7 remains the Baseline 0.1 writer format. The stabilization checks do
not change the wire layout; they reject malformed records previously accepted.
Readers accept documented legacy versions 3–6, not future/unknown versions.

| Field | Encoding |
| --- | --- |
| Magic | 5 ASCII bytes: `DISCO` |
| Format version | `uint8_t`: currently `7` |
| Target | `uint8_t`: `0` = GSU, `1` = SPC700 |
| Memory mapping | `uint8_t`: `0` = LoROM, `1` = HiROM |
| Code start address | `uint32_t` |
| DATA alignment | `uint8_t`: power of two from 1 to 128 (introduced in v4; generalized in v6) |
| RAM alignment | `uint8_t`: power of two from 1 to 128 (version 6) |
| Bitmap present | `uint8_t`: 0 or 1 (version 7) |
| Bitmap profile, if present | `uint8_t` OBJ flag, `uint8_t` depth, `uint32_t` height, `uint32_t` base |
| Code section | `uint32_t` byte count, followed by raw code bytes |
| Data section | `uint32_t` byte count, followed by raw data bytes |
| RAM initialization image | `uint32_t` byte count, followed by initial RAM bytes (version 5) |
| Symbol count | `uint32_t` |
| Symbols | Repeated symbol records |
| Relocation count | `uint32_t` |
| Relocations | Repeated relocation records |

Bitmap metadata records the selected SNES host configuration, not instructions.
The OBJ flag is 0/1; depth is 2/4/8; height is 128/160/192 for bitmap or 256
for OBJ. Base is a 1024-byte-aligned offset from `$70:0000`; the complete
framebuffer must fit 128 KiB. Metadata is GSU-only. It survives compiler,
assembler and final linked assembly exports. The linker merges identical
profiles, rejects conflicts, and checks framebuffer overlap with static RAM,
RAM payloads and initialized stack placement. See [GSU graphics](gsu-graphics.md).

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
Names must contain 1–4,096 bytes and no ASCII control characters, except the
single leading private-identity marker. The marker alone is not a name.
Definitions must be unique within one object, including private definitions.
Offsets may equal the section size to describe an end label, but may not exceed
it. An end label is not a valid call/runtime-entry destination.

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
Operand spans must fit the selected section and must not overlap another
relocation's operand bytes. An adjacent bank/offset pair is valid. The format
does not carry an instruction-boundary map or cross-unit function signatures.

### Input bounds and rejection

Both reading and writing validate target/mapping/section/relocation enums,
alignment, names, symbol offsets, bitmap profiles and relocation spans.
Truncated fields/records and trailing bytes are rejected. The reader bounds
counts against bytes remaining before allocating table entries; a tiny file
cannot request a huge table by its count alone. Resource caps are:

- 128 MiB for the complete serialized object;
- 64 MiB each for CODE and DATA; 64 KiB for a RAM image;
- 4,096 bytes per symbol/relocation name;
- 1,000,000 entries each for symbol and relocation tables, additionally bounded
  by file size.

These are container bounds, not target-placement permissions: a linked GSU
payload must still fit its single accessible program bank. The linker also caps
combined input sections/names/table records at 128 MiB, charged in the v7 layout
even for legacy inputs, and rejects impossible GSU section totals while loading
instead of retaining the full input set first. Structural writer
validation runs before opening its destination. Link configuration validation
runs before opening payload/assembly destinations. I/O failure during final
writing is not a transactional-output guarantee.

## Linker layout

`discld` performs these operations:

1. Reads every input object.
2. Checks compatible targets, mapping and effective origins (an explicit CLI
   origin overrides all object defaults), and merges compatible bitmap metadata.
3. Concatenates object code sections in input order.
4. Resolves code symbols against the configured code base.
5. Aligns the DATA start and each input DATA section, then appends their bytes.
6. Resolves data symbols.
7. Applies relocations.
8. Writes code, alignment padding, and data to the output file.

Versions 6/7 allocate each object's RAM image with its declared alignment, from
`--ram-origin` in `--ram-bank`. RAM bytes are not appended to ROM DATA.
`--init-runtime` encodes zeroing/nonzero initialization in startup; otherwise
nonempty RAM requires `--host-initialized-globals`. Public/private RAM targets
use their own object's RAM base, independently of CODE/DATA bases.
`ADDR16_RAM` requires a RAM symbol or reserved stack limit and validates the
near RAM bank before narrowing; CODE/DATA cannot masquerade as static RAM.

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
Calls additionally require a target inside CODE, not DATA, RAM or a one-past
CODE label. The format does not prove that an arbitrary hand-authored CODE
offset is an instruction boundary. Compiler near ROM materializations that
use the reserved near-ROM bank context must resolve DATA in that selected ROM
bank; a RAM-resident copy is not a ROM-buffer source.
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
Reserved `__disco_stack_limit_<bytes>` references resolve to the static RAM floor,
raised by selected framebuffer reservations and (with runtime initialization)
RAM payloads below the configured descending stack, plus the required stack
space. Known frames that cannot fit the initial R10 are rejected at link time;
otherwise generated frame/push checks stop with R6=2 on dynamic exhaustion.
Recursive/cumulative depth is not proved by linking. Input definitions of these
symbols are forbidden. Without known reservations the floor is zero. These
checks do not reserve unrelated host resources or infer host-owned stack space.

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
alignment 2. Version 6 records RAM alignment explicitly. Writers emit version 7,
which additionally records optional host bitmap metadata. Versions 3 through 6
have no bitmap field; the reader supplies an unselected profile.
Versions 3/4 cannot contain RAM symbols or relocation type 4. Other versions,
invalid alignment bytes, truncated bitmap records, invalid flags and invalid
profiles are rejected. RAM images are bounded to 64 KiB. Rebuild
compiler objects when changing the pointer ABI: reading an old container does
not make its old far argument layout ABI-compatible, and cross-unit signatures
are not stored or checked by this format.

The format has no checksum, per-symbol alignment table, or section flags. The
version byte is validated by `ObjectFile::read`. Changes to the
serialization order or enum values require coordinated changes to
`ObjectFile::write`, `ObjectFile::read`, `discld`, and `discas`.

O2 cached function entries can request 16-byte CODE object alignment using the
private symbol `\x01__disco_cache_align16` at CODE offset zero. This version-7
performance hint is not an addressable/exported symbol; malformed hints and
relocations to it are rejected. Older linkers may ignore it without changing
program semantics. The current linker inserts explicit NOP padding at the final
origin and uses the resulting per-object base for all local/global references.
The compiler assembly path carries `.define __DISCO_CODE_ALIGNMENT 16`; linked
assembly already contains its final padding and does not request realignment.
