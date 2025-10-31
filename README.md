# VectorTick

[![CI](https://github.com/Aneesh495/VectorTick/actions/workflows/ci.yml/badge.svg)](https://github.com/Aneesh495/VectorTick/actions/workflows/ci.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

VectorTick is a high-performance C++20 columnar market-data analytics engine designed for financial tick ingestion, durable columnar storage, and low-latency SQL query execution.

It features wire-speed PCAP ingestion, durable VTS1 columnar segments with hardware CRC32C, a typed SQL query compiler, a vectorized SIMD executor (NEON & AVX2), native host JIT compilation with strict $W \oplus X$ security, and deterministic replay capabilities.

---

## Key Features

- **Wire-Speed Market Ingestion**: Decodes VTP1 packets from standard PCAP files supporting little/big-endian capture formats, nanosecond/microsecond timestamps, and 802.1Q VLAN tagging.
- **Durable Columnar Storage (VTS1)**: 12 canonical market columns stored in 64-byte aligned blocks with bitpacking, dictionary encoding, run-length encoding (RLE), CRC32C integrity checksums, zone maps, and bloom filters.
- **Transactional Catalog & Durability**: Append-only WAL journal with torn-write detection, atomic 2-phase commits, crash recovery, and segment compaction.
- **Typed SQL Query Engine**: Case-insensitive SQL lexer and parser supporting projections, aliases, `WHERE` expressions, `GROUP BY`, aggregate functions (`COUNT`, `SUM`, `MIN`, `MAX`, `AVG`), `ORDER BY`, and `LIMIT`.
- **Multiple Execution Backends**:
  - **Scalar Reference Interpreter**: Pure functional semantic oracle with checked arithmetic.
  - **Portable Vectorized Executor**: Cache-conscious 1024-row chunked executor with runtime SIMD dispatch (ARM NEON & x86 AVX2).
  - **Host JIT Compiler**: Native machine code emission for Apple Silicon AArch64 and x86-64 with strict $W \oplus X$ policy and macOS `MAP_JIT` write protection.
- **Deterministic Market Replay**: Sequence monotonicity verification, microsecond pacing, and line-rate replay (>220 million events/sec).
- **Hardened Verification**: 18 CTest suites, 67 unit/integration tests, fuzz targets with smoke testing, and 100% clean AddressSanitizer and UndefinedBehaviorSanitizer passes.

---

## Verified Performance

The following metrics were measured on an Apple Silicon host (`Darwin arm64`, AppleClang, Release build):

| Benchmark Stage | Measured Throughput | Description |
|---|---|---|
| **CRC32C Integrity Check** | **7.56 GB/s** | Hardware-accelerated / table CRC32C checksums |
| **Scalar Interpreter Oracle** | **12.88 Mop/s** | Pure functional SSA interpreter |
| **Host JIT Execution** | **136.56 Mop/s** | Native compiled machine code (10.6x speedup) |
| **Vectorized Filter Scan** | **16.00 Mrows/s** | Columnar filter scan with selection vectors |
| **Vectorized Aggregation (NEON)** | **114.02 Mrows/s** | SIMD-accelerated sum/min/max/count reductions |
| **VTS1 Storage Write** | **20.31 Mev/s** | Full 12-column encoded disk serialization |
| **VTS1 Storage Read** | **23.65 Mev/s** | Columnar memory-mapped deserialization |
| **Deterministic Replay** | **224.68 Mev/s** | Line-rate replay with monotonicity verification |

---

## Architecture Overview

```
                      +-----------------------------+
                      |   PCAP / Network Capture    |
                      +-----------------------------+
                                     |
                                     v
                      +-----------------------------+
                      |     VTP1 Frame Decoder      |
                      +-----------------------------+
                                     |
                                     v
                      +-----------------------------+
                      |   VTS1 Columnar Writer      |
                      | (BitPack, RLE, CRC32C, WAL) |
                      +-----------------------------+
                                     |
                +--------------------+--------------------+
                |                                         |
                v                                         v
+-------------------------------+         +-------------------------------+
|       SQL Query Engine        |         |     Deterministic Replay      |
|  (Lexer -> Parser -> Typer)   |         |   (Monotonic Sequence Check)  |
+-------------------------------+         +-------------------------------+
                |
                v
+-------------------------------+
|     SSA IR Representation     |
+-------------------------------+
        |               |
        v               v
+---------------+ +---------------+
|  Vectorized   | |   Host JIT    |
|  SIMD (NEON)  | |  (A64 / X86)  |
+---------------+ +---------------+
```

---

## CLI Applications

VectorTick provides five standalone command-line utilities:

### 1. `vectortick_demo`
Runs a complete end-to-end pipeline demonstration:
```bash
./build/bin/vectortick_demo
```

### 2. `vectortick_inspect`
Inspects segment headers, column descriptors, zone maps, bloom filters, and previews data rows:
```bash
# Print summary, descriptors, zone maps, bloom filter, and first 10 rows:
./build/bin/vectortick_inspect -s -m -d 10 market_data.vts
```

### 3. `vectortick_query`
Executes SQL queries against columnar segment files:
```bash
# Aggregation query
./build/bin/vectortick_query market_data.vts "SELECT COUNT(*), SUM(quantity), AVG(price_ticks) WHERE price_ticks > 15000"

# Projection query with limits
./build/bin/vectortick_query -l 20 market_data.vts "SELECT instrument_id, price_ticks, quantity WHERE instrument_id = 101"
```

### 4. `vectortick_replay`
Replays a segment file deterministically with rate limiting and monotonicity checks:
```bash
# Maximum line-rate replay:
./build/bin/vectortick_replay -s market_data.vts

# Replay paced at 10,000 events/sec:
./build/bin/vectortick_replay -r 10000 market_data.vts
```

### 5. `vectortick_ingest`
Ingests PCAP captures into durable VTS1 segments:
```bash
./build/bin/vectortick_ingest input.pcap output.vts
```

### 6. `vectortick_bench`
Executes the comprehensive benchmark suite and outputs JSON performance data:
```bash
./build/bin/vectortick_bench --json bench/results.json
```

---

## Building and Testing

### Prerequisites
- C++20 compliant compiler (`AppleClang 15+`, `Clang 16+`, or `GCC 13+`)
- CMake 3.20+
- Ninja or Make

### Quick Start
```bash
# Configure and build Release
make build BUILD_TYPE=Release

# Run the 18 CTest suites in parallel
make test

# Run end-to-end demo
make demo

# Run benchmark suite and verify results
make benchmark
make verify
```

### Sanitizer Builds (ASan + UBSan)
```bash
# Clean, build, and test with AddressSanitizer and UndefinedBehaviorSanitizer
make sanitize
```

### Full Acceptance Pipeline
To run the complete automated acceptance pipeline (Release build, test suites, sanitizers, fuzz smoke testing, demo, dataset generation, benchmarks, stress testing, and LOC audit):
```bash
make acceptance
```

---

## Documentation

- [Detailed Repair & Audit Resolution Status](docs/REPAIR_STATUS.md)
- [Architecture & Design Specifications](docs/ARCHITECTURE.md)
- [Architecture Decision Records (ADRs)](docs/adr/README.md)

---

## License

MIT. See [LICENSE](LICENSE).
