# DiscoC Relocatable Object Format

DiscoC object files use the `.o` extension and are consumed by `discld`. Both the normal IR backend and `discas` produce this format.

The format is intentionally small and implementation-owned. It is not ELF, SNES object format, or a general-purpose replacement for a platform linker format.

## Binary layout

All multi-byte integer fields are serialized explicitly in little-endian byte
order, independent of the host architecture. The current format version is
3. The format is serialized in this order:

| Field | Encoding |
| --- | --- |
| Magic | 5 ASCII bytes: `DISCO` |
| Format version | `uint8_t`: currently `3` |
| Target | `uint8_t`: `0` = GSU, `1` = SPC700 |
| Memory mapping | `uint8_t`: `0` = LoROM, `1` = HiROM |
| Code start address | `uint32_t` |
| Code section | `uint32_t` byte count, followed by raw code bytes |
| Data section | `uint32_t` byte count, followed by raw data bytes |
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

The section value is `0` for code and `1` for data. The offset is relative to that section within the object.

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

The section value in a relocation identifies the section containing the placeholder. The current compiler primarily emits code-section relocations for calls and global addresses.

## Linker layout

`discld` performs these operations:

1. Reads every input object.
2. Checks that all objects use the same mapping and code start address.
3. Concatenates object code sections in input order.
4. Resolves code symbols against the configured code base.
5. Places all data sections after the combined code.
6. Resolves data symbols.
7. Applies relocations.
8. Writes code followed by data to the output file.

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

LoROM/ROM execution remains the default. `set execution_memory = ram;` selects
`$70:8000` unless `code_start_address` is specified explicitly; it does not
change the cartridge mapping. The full origin carries this selection through
the existing version-3 object format. Both direct compilation and emitted
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

The format currently has no checksum, alignment table, or section flags. The
version byte is validated by `ObjectFile::read`; incompatible versions are
rejected instead of being interpreted as a different layout. Changes to the
serialization order or enum values require coordinated changes to
`ObjectFile::write`, `ObjectFile::read`, `discld`, and `discas`.
