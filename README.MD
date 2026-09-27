# Pakker

Pakker is a C++20 asset archive library for game engines. It provides
memory-mapped I/O, zero-copy views, LZ4 and Zstd compression, hash-based
lookups, layered archives, integrity checks, and concurrent runtime reads.

## Simple usage

```cpp
#include "Pak.h"

// Build assets.pak from a directory.
Pakker builder;
PakOptions options;
builder.CreatePakFromFolder("assets.pak", "assets", options);

// Open it and load one asset.
PakReader reader;
reader.Open("assets.pak");

std::vector<uint8_t> data;
reader.Load(reader.Find("textures/diffuse.dds"), data);
```

See the [usage guide](USAGE.md) for compression, streaming reads, layered
archives, caching, integrity verification, CMake integration, installation,
and the complete API.

## Highlights

- Memory-mapped reads and zero-copy views for eligible assets
- O(1) path and precomputed-hash lookup
- LZ4 and Zstd compression, including independently decodable chunks
- Layered base, patch, DLC, and language archives
- Parallel archive creation and thread-safe runtime reads
- Batched prefetch-aware reads and bounded decoded caches
- No external runtime dependencies; codecs are vendored

## Current large-game benchmark

Current local Windows x64 Release results for 50,000 entries and eight mount
requests:

```powershell
.\build\Release\PakkerScaleBenchmark.exe 50000 8
```

| Measurement | Result |
|-------------|-------:|
| Archive creation | 196.365 ms |
| Archive size | 7.63221 MiB |
| Open with names | 32.8174 ms |
| Resident metadata with names | 6.42969 MiB (134 bytes/entry) |
| Random path lookup | 382.194 ns/lookup |
| Missing path lookup | 128.432 ns/lookup |
| Precomputed-hash lookup with names | 45.144 ns/lookup |
| Verify all 50,000 entries | 2.1726 ms |
| Enumerate and sort all names | 9.9907 ms |
| Open without names | 3.4889 ms |
| Resident metadata without names | 4.11719 MiB (86 bytes/entry) |
| Precomputed-hash lookup without names | 31.618 ns/lookup |
| Eight duplicate mount requests | 6.6946 ms total (0.836825 ms/request) |
| Resident duplicate-mount state | 6.08203 MiB |
| Duplicate-mount result | 1 unique layer, reference count 8 |

The mount measurement uses `PakOpenOptions::loadNames = false`. Results are a
point-in-time local baseline and vary with hardware, compiler, storage state,
and background load.

## Documentation

- [Usage guide](USAGE.md) - integration, configuration, examples, and API
- [Contributor handbook](HANDBOOK.md) - architecture, invariants, and testing
- [Example application](example/main.cpp) - complete executable example

## Requirements

- C++20 compiler
- CMake 3.10 or newer

## License

Pakker is available under the [MIT License](LICENSE.txt).
