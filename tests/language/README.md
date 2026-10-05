# Language Baseline 0.1 conformance

These cases enforce the source-language contract in
[language-spec.md](../../docs/language-spec.md). They run `discc --check`, which
lexes/parses, resolves imports, analyzes types, optimizes and verifies IR without
emitting or executing machine code. GSU and SPC700 check the same portable
language; SPC700 does not yet emit machine code.

## Run

From a built repository:

```sh
ctest --test-dir build/native -C Release -L conformance --output-on-failure
ctest --test-dir build/native -C Release -L graphics --output-on-failure
```

The first command is semantic conformance; the second is a separate GSU backend
oracle. The same case registry is used by the direct GCC/Clang test runner.

## Rule index

| Specification section | Frontend cases | Separate execution/verification evidence |
| --- | --- | --- |
| 1: units, targets, build configuration | `configuration/`, `cfg/`, `imports/` | `target_foundation`, `project_*`, `module_import*` |
| 2, 12: literals, comments, encoding | `types/`, `conversions/`, `strings/` | `language_contract`, lexer/parser unit cases, `language_arrays_execution` |
| 3: widths, bool, near/far, alignment | `types/`, `pointers/`, `far/`, `layout/` | `language_numeric`, `pointer_*`, `ir_verifier` |
| 4: ROM/RAM, const, volatile | `qualifiers/`, `volatile/`, `globals/` | `language_qualifiers`, `language_aggregates`, `graphics_colors` |
| 5: conversions, wrapping, shifts, division, evaluation order | `conversions/`, `updates/`, `volatile/` | `language_numeric`, `language_operators`, `language_control_execution` |
| 6: arrays, structs, layouts, aggregate restrictions | `arrays/`, `structs/`, `layout/` | `language_aggregates`, `language_arrays_execution`, `pointer_nested` |
| 7, 8: scopes, functions, linkage, RAM startup, ROM storage | `types/`, `globals/`, `imports/`, `strings/` | `shadowing`, `multifile`, `language_globals`, `language_linkage` |
| 9: loops, continue, switch/fallthrough | `control-flow/`, `updates/` | `language_control_execution`, `switch_abi`, `ir_and_cfg` |
| 10: plot cursor, pixel/color/read/flush, capabilities | `plot/`, `bitmap/`, `attributes/` | `graphics_state`, `graphics_colors`, `graphics_bitmaps`, `graphics_diagnostics`, `ir_verifier` |
| 11: constants, enums, layout/assertions, attributes | `constants/`, `layout/`, `attributes/` | `language_extensions_execution`, `language_contract` |
| 13: imports and visibility | `imports/`, `modules/`, `aliases/` | `module_loader`, `module_import*`, `language_modules_execution` |
| 14: diagnostics and bounded compilation | negative cases in every category | `language_warnings`, `language_contract`, malformed-object tests, fuzzing |
| 15, 16: aliases and target selection | `aliases/`, `cfg/` | `language_target_libraries`, `module_import_paths` |
| 17: portable/target libraries, deferred systems features | `attributes/` rejection cases | `language_fixed_point`, `language_memory_library`, `language_target_libraries` |

Cases deliberately combine rules: far/volatile pointers in struct fields and
calls; packed byte aggregates in aligned struct arrays; private constants in a
public imported layout; diamond imports sharing RAM/ROM data; bool with numeric
wrapping; plot updates with continue, calls and snapshots; imported bitmap
selection with target-capability rejection. Dynamic bank restoration, access
counts and pixel caches require the execution groups, not frontend acceptance.

## Adding a case

Each category has `valid*.dc` and `invalid*.dc` root cases. Dependencies belong
in a subdirectory, not beside root cases. The harness sorts files for deterministic
order and checks both targets by default.

- `// error: diagnostic regex` is mandatory in a negative case. It must exit
  with status 1 and a source-located language error matching that regex.
- `// spc-error: diagnostic regex` gives a portable root case an expected SPC700
  capability rejection while GSU succeeds.
- `// target: gsu` restricts a GSU-only negative case when the other target would
  reject an earlier capability instead of the rule being tested.

Timeouts, process exceptions/signals, missing diagnostics and internal IR
verification errors are failures, never accepted negative results. Assertions
about runtime values belong in backend tests. New or corrected rules require
both acceptance and rejection coverage where meaningful; do not weaken a rule
or diagnostic expectation just to make a regression green.
