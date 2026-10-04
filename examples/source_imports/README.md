# Source imports

Build from this directory with `discc build`, or from the repository root:

```sh
discc build --config examples/source_imports/discoc.toml
```

Only `src/main.dc` is listed as a project root. The compiler discovers `math.dc`
and `graphics.dc` through `compiler.import_paths`, then their shared `types.dc`.
Each source is parsed and compiled once, into its own object; function bodies
and storage definitions are not pasted into the importing source.

Imports expose public declarations: functions, RAM/ROM data, types, enums and
constants. Symbols are public by default (`export` makes this explicit);
`internal` hides implementation details such as `identity` and `LANES`.

The payload writes these words to GSU RAM:

| Offset | Value | Meaning |
| --- | --- | --- |
| `$0100` | 149 | `46 + 103`, through two imported functions |
| `$0102` | 8 | Shared mutable counter, initialized once |
| `$0104` | 4 | Imported packed struct size |
| `$0106` | 6 | Public compile-time constant plus read-only RAM data |
| `$0108` | 2 | Imported enum value |
| `$010A` | 6 | Imported initialized array sum |

As with other GSU examples, this is a payload, not a complete SNES ROM. The
manifest enables runtime initialization and puts static RAM data at `$0600`.
