# Pakker

Pakker is a fast, lightweight asset archive library designed for game engines. Bundle assets into a single `.pak` file at build time, then read them at runtime with O(1) lookups, LZ4 compression, and zero console output.

## Features

- **PakReader** -- runtime read-only class optimized for shipping builds
  - Open once, read many -- persistent file handle with cached file table
  - O(1) file lookup via hash map (no linear scanning)
  - LZ4 per-file compression (fast decompression, ~4 GB/s)
  - Batch reads sorted by disk offset for sequential I/O
  - Silent by default -- no console output unless you set a log callback
- **Pakker** -- build-time API for creating, extracting, and modifying PAK files
- **PAK format v2** -- backward compatible with v1, adds compression metadata
- **Optional XOR encryption** -- disabled by default for best performance
- **No external dependencies** -- LZ4 is vendored, only needs C++20 and a C compiler

## Dependencies

- C++20 compatible compiler
- CMake 3.0+

## Quick Start

### Build-time: create a compressed PAK

```cpp
#include "Pak.h"

Pakker pakker;
std::map<std::string, std::vector<uint8_t>> files;
// ... load your assets into the map ...

// Create a compressed PAK (LZ4)
pakker.CreatePak("assets.pak", files, true);

// Or create from a folder
pakker.CreatePakFromFolder("assets.pak", "path/to/assets", true);
```

### Runtime: read assets with PakReader

```cpp
#include "Pak.h"

PakReader reader;
reader.Open("assets.pak");

// O(1) existence check -- no disk I/O
if (reader.FileExists("textures/diffuse.png")) {
    auto info = reader.GetFileInfo("textures/diffuse.png");
    // info.originalSize, info.compressed, etc.
}

// Read a single file (seek + read + decompress)
std::vector<uint8_t> data = reader.ReadFile("textures/diffuse.png");

// Or get a shared_ptr
auto shared = reader.LoadFile("textures/diffuse.png");

// Batch read -- sorted by offset for sequential I/O
auto batch = reader.ReadFiles({"sounds/hit.wav", "textures/diffuse.png", "models/player.glb"});

// reader.Close() is called automatically by the destructor
```

### Logging

By default, Pakker produces no console output. Set a callback to wire into your engine's logging:

```cpp
PakSetLogCallback([](PakLogLevel level, const char* message) {
    switch (level) {
        case PakLogLevel::Error:   myEngine::LogError(message); break;
        case PakLogLevel::Warning: myEngine::LogWarn(message);  break;
        case PakLogLevel::Info:    myEngine::LogInfo(message);  break;
    }
});
```

### Encryption (optional)

Encryption is opt-in. Pass a key to enable XOR obfuscation:

```cpp
// Build time
Pakker pakker("my_secret_key");
pakker.CreatePak("assets.pak", files, true);

// Runtime
PakReader reader("my_secret_key");
reader.Open("assets.pak");
```

## Build-time API (Pakker)

| Method | Description |
|--------|-------------|
| `CreatePak(filename, files, compress)` | Create PAK from a map of filename to data |
| `CreatePakFromFolder(filename, folder, compress)` | Create PAK from a directory recursively |
| `ExtractPak(filename, outputDir)` | Extract all files to a directory |
| `ListPak(filename)` | Log all file entries via the log callback |
| `AddFileToPak(filename, name, data)` | Append a file to an existing PAK |
| `ValidatePak(filename)` | Validate PAK header and entry integrity |
| `ReadFileFromPak(filename, name)` | Read a single file (one-shot, no caching) |
| `GetFileCount(filename)` | Get file count from header |
| `FileExists(filename, name)` | Check if a file exists |
| `GetFileInfo(filename, name)` | Get file metadata |

## Runtime API (PakReader)

| Method | Description |
|--------|-------------|
| `Open(filename)` | Open PAK, parse file table, build hash map |
| `Close()` | Close handle and free cached data |
| `IsOpen()` | Check if a PAK is currently open |
| `ReadFile(name)` | Read and decompress a file, returns `vector<uint8_t>` |
| `LoadFile(name)` | Same as ReadFile but returns `shared_ptr` |
| `FileExists(name)` | O(1) hash lookup, no disk I/O |
| `GetFileCount()` | Returns cached count, no disk I/O |
| `GetFileInfo(name)` | O(1) lookup for size, compression info |
| `ReadFiles(names)` | Batch read, sorted by disk offset |

## PAK Format

**Header (40 bytes):**
| Field | Type | Description |
|-------|------|-------------|
| magic | char[4] | `"PAK0"` |
| version | uint32_t | 1 (legacy) or 2 (with compression) |
| numFiles | uint32_t | Number of files |
| fileTableOffset | uint64_t | Byte offset to file table |
| reserved | uint32_t[4] | Reserved for future use |

**File table entry (v2):**
| Field | Type | Description |
|-------|------|-------------|
| nameLength | uint16_t | Filename length |
| filename | char[] | UTF-8 filename |
| offset | uint64_t | Byte offset to file data |
| originalSize | uint64_t | Uncompressed size |
| compressedSize | uint64_t | Size on disk |
| flags | uint8_t | Bit 0: LZ4 compressed |

v1 entries omit `compressedSize` and `flags`. PakReader reads both formats.

## Thread Safety

`PakReader` is **not** thread-safe. If you need concurrent reads, synchronize access externally or use one `PakReader` per thread.

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## License

This project is licensed under the MIT License - see the LICENSE file for details.
