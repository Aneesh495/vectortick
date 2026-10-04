# VectorTick implementation status

VectorTick is a prototype. Source files cover the planned architecture, but
the repository does not yet demonstrate a complete, tested query-to-JIT path
or production readiness.

## Tracked implementation

| Area | Source in the repository | Validation boundary |
| --- | --- | --- |
| Core utilities | `include/vectortick/common/`, `src/common/` | Six basic tests cover some primitives |
| Memory and concurrency | `include/vectortick/memory/`, `include/vectortick/concurrency/` | No dedicated tests |
| Protocol | `include/vectortick/protocol/`, `src/protocol/` | No dedicated decoder or PCAP tests |
| Codecs and storage | `include/vectortick/codec/`, `include/vectortick/storage/`, `src/storage/` | No dedicated round-trip tests |
| Query frontend | `include/vectortick/query/`, `src/query/` | No dedicated parser tests |
| IR and execution | `include/vectortick/ir/`, `src/ir/`, `src/execution/` | No dedicated IR or interpreter tests |
| Native code | `include/vectortick/jit/`, `src/jit/` | x86-64 and AArch64 source exists; no JIT tests |
| Apps and benchmarks | `apps/`, `bench/` | Entry points and harness exist; results are not published |

This is a source inventory, not a claim that every component works end to end.

## Build check

On macOS arm64 with AppleClang, CMake configuration succeeds and the build
fails while compiling the JIT sources. Calls in
`include/vectortick/jit/x86_assembler.hpp` pass `X86Reg` values to
`modrm(u8, u8, u8)`, which AppleClang rejects. The tests and benchmark cannot
be run from that build. The repository does not include reproducible results
for other operating systems or architectures.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Tests and missing evidence

`tests/test_main.cpp` defines six checks: type sizes, byte swapping, CRC32C,
canonical event size, checked addition, and checked multiplication. No
coverage percentage has been measured. Protocol, storage, query, IR, and JIT
paths lack dedicated tests. `bench/bench_main.cpp` is a benchmark harness,
but this repository has no recorded benchmark run or reproducible result set.
There is no `fuzz/` directory or fuzz target source.

The next validation steps are to make the clean build pass, add module and
integration tests, exercise both JIT targets on their native architectures,
and publish repeatable benchmark commands with raw results. Until then,
throughput, latency, and cross-platform support remain unverified.

## Build targets

The current CMake file defines a static `vectortick` library, five app targets,
`vectortick_tests`, and `vectortick_bench`. It contains a conditional glob for
fuzz targets, but none are produced because no fuzz sources are tracked.

The [architecture document](ARCHITECTURE.md) and [ADRs](adr/README.md) include
design intent. They should be read with the validation limits above.
