# Triangle regression entry point

The canonical source, manifest, SNES host and Mesen checks now live in the
public [SNES triangle example](../../../examples/snes/triangle/README.md).
`graphics_triangle` compiles that same source, checks its project manifest,
compares direct/assembled/project payloads and executes the instruction model.

The old script command remains a compatibility entry point:

```sh
cmake -DVERIFY_MESEN=ON -P tests/graphics/triangle/build-snes.cmake
```

It delegates to the public script and keeps the old `build/plot-triangle`
output directory. There is no duplicate source or host to maintain.
