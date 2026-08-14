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
| Format version | `uint8_t`: currently `2` |
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
