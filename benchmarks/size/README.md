# Size-policy helper probe

`division_pool.dc` keeps four runtime generic division sites, two signed and
two unsigned. Seed 3 produces 2019, returned in R0 and stored at `$70:0120`.
The signed result must survive the subsequent unsigned function call.

With Mesen/WLA-DX configured, this is the optional serial CTest
`benchmark_mesen_size_pool`. It can also run directly:

```sh
cmake "-DMESEN=/absolute/path/Mesen" "-DWLA_65816=/absolute/path/wla-65816" "-DWLALINK=/absolute/path/wlalink" -P benchmarks/size/run-mesen.cmake
```

The script compares O2 and Os using the existing cold-cache ROM/fast-GSU
benchmark host. It requires both private kernel kinds in the Os object, a
smaller complete Os payload, exact final-assembly reconstruction, independent
functional results and matching Mesen opcode counts. RAMBR and the entry stack
are checked after STOP. The established function-call snapshot checker is
reused only for its seed/result contract; this is a different source workload.

Artifacts go to `build/benchmarks/size-pool/O2/` and `Os/`: linked payload,
assembly, ROM, model metrics, emulator timing and RAM/register snapshots.
The report names this workload `division_pool`. Fewer bytes do not guarantee
fewer cycles; measurements are emulated, not physical hardware.

The retained [O2 report](../results/division-pool-O2.json) is 657 bytes and
9,535 emulated fast-GSU cycles; [Os](../results/division-pool-Os.json) is 536
bytes and 9,735 cycles. That is 18.4% fewer bytes for about 2.1% more cycles.
Both execute 26 stack accesses and return/store 2019. Os executes four
additional private LINK calls; both signed/unsigned kernel selections are
asserted rather than inferred from the size reduction alone.
