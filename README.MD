# Pakker

Pakker is a high-performance asset archive library designed for 3D game engines. Bundle assets into a single `.pak` file at build time, then read them at runtime with memory-mapped I/O, zero-copy reads, and thread-safe concurrent access.

## Features

- **Memory-mapped I/O** -- maps the entire PAK into the address space; the OS handles caching, prefetching, and page management
- **Zero-copy reads** (`PakSpan`) -- returns a direct pointer into mapped memory for uncompressed assets, eliminating heap allocation entirely
- **Page-aligned data** (v3 format) -- file data aligned to configurable boundaries (16 B, 4 KB, 64 KB) for efficient page faults and GPU upload
- **Thread-safe PakReader** -- `shared_mutex` allows concurrent reads from multiple threads; ideal for asset streaming on worker threads
- **Parallel batch decompression** -- `ReadFiles()` decompresses across threads when using the mmap backend
- **O(1) file lookup** via hash map -- no linear scanning
- **LZ4 per-file compression** -- fast decompression (~4 GB/s), smart skip for pre-compressed formats
- **Silent by default** -- no console output unless you set a log callback
- **Optional XOR encryption** -- disabled by default for best performance
- **No external dependencies** -- LZ4 is vendored, only needs C++20 and a C compiler
- **Backward compatible** -- reads v1, v2, and v3 PAK files; legacy API still produces v2

## Dependencies

- C++20 compatible compiler
- CMake 3.10+

## Quick Start

### Build-time: create a page-aligned PAK

```cpp
#include "Pak.h"

Pakker pakker;
std::map<std::string, std::vector<uint8_t>> files;
// ... load your assets into the map ...

// Page-aligned PAK for optimal mmap and GPU upload
PakOptions opts;
opts.compress = true;       // LZ4 per-file compression
opts.alignment = 4096;      // 4 KB page alignment
opts.formatVersion = 3;     // v3 format with alignment

pakker.CreatePak("assets.pak", files, opts);

// Or create from a folder
pakker.CreatePakFromFolder("assets.pak", "path/to/assets", opts);
```

The legacy API still works and produces v2 format:
```cpp
pakker.CreatePak("assets.pak", files, true); // compress=true, v2
```

### Runtime: read assets with PakReader

```cpp
#include "Pak.h"

PakReader reader;
reader.Open("assets.pak");

// Check if mmap is active
if (reader.IsMapped())
    std::cout << "Using memory-mapped I/O" << std::endl;

// O(1) existence check -- no disk I/O
if (reader.FileExists("textures/diffuse.png")) {
    auto info = reader.GetFileInfo("textures/diffuse.png");
}

// Standard read (allocates result buffer)
std::vector<uint8_t> data = reader.ReadFile("textures/diffuse.png");

// Zero-copy read -- direct pointer into mmap for uncompressed assets
PakSpan span = reader.ReadFileZeroCopy("textures/diffuse.dds");
if (span && !span.ownsData) {
    // span.data points directly into mapped memory -- no copy, no allocation
    // Upload to GPU: glTexImage2D(..., span.data) or equivalent
}

// Batch read -- parallel decompression across threads
auto batch = reader.ReadFiles({"sounds/hit.wav", "textures/diffuse.png", "models/player.glb"});
```

### Thread-safe concurrent reads

```cpp
PakReader reader;
reader.Open("assets.pak");

// Safe to read from multiple threads simultaneously
std::vector<std::thread> workers;
for (int i = 0; i < 8; ++i) {
    workers.emplace_back([&reader]() {
        auto data = reader.ReadFile("textures/diffuse.png");
    });
}
for (auto& w : workers) w.join();
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
pakker.CreatePak("assets.pak", files, opts);

// Runtime
PakReader reader("my_secret_key");
reader.Open("assets.pak");
```

Note: encrypted files cannot use zero-copy reads since decryption requires a mutable buffer.

## Game Engine Integration

### Alignment recommendations

| Use case | Alignment | Notes |
|----------|-----------|-------|
| General purpose | 16 | Default. Minimal padding overhead |
| Memory-mapped I/O | 4096 | Matches OS page size. Optimal page fault behavior |
| D3D12/Vulkan textures | 65536 | Matches `D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT` |
| No alignment (legacy) | 1 | Use `formatVersion = 2` for old readers |

### Zero-copy GPU upload

For textures stored in GPU-ready formats (DDS, KTX2, Basis), use `ReadFileZeroCopy()` with `alignment = 4096` or `65536`. The returned pointer is page-aligned and can be passed directly to graphics API upload functions without an intermediate copy.

### Asset streaming pattern

```cpp
// Worker thread: stream assets from PAK
void AssetStreamWorker(PakReader& reader, AssetQueue& queue) {
    while (auto request = queue.pop()) {
        auto span = reader.ReadFileZeroCopy(request->path);
        request->callback(span.data, span.size);
    }
}
```

## Build-time API (Pakker)

| Method | Description |
|--------|-------------|
| `CreatePak(filename, files, PakOptions)` | Create PAK with alignment and format control |
| `CreatePak(filename, files, compress)` | Legacy: create v2 PAK |
| `CreatePakFromFolder(filename, folder, PakOptions)` | Create PAK from directory with options |
| `CreatePakFromFolder(filename, folder, compress)` | Legacy: create v2 PAK from directory |
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
| `Open(filename)` | Open PAK, parse file table, memory-map file |
| `Close()` | Unmap file, close handle, free cached data |
| `IsOpen()` | Check if a PAK is currently open |
| `IsMapped()` | Check if memory-mapped I/O is active |
| `ReadFile(name)` | Read and decompress a file, returns `vector<uint8_t>` |
| `LoadFile(name)` | Same as ReadFile but returns `shared_ptr` |
| `ReadFileZeroCopy(name)` | Zero-copy read, returns `PakSpan` (see below) |
| `FileExists(name)` | O(1) hash lookup, no disk I/O |
| `GetFileCount()` | Returns cached count, no disk I/O |
| `GetFileInfo(name)` | O(1) lookup for size, compression info |
| `ReadFiles(names)` | Batch read with parallel decompression |

### PakSpan

`ReadFileZeroCopy()` returns a `PakSpan`:

| Field | Type | Description |
|-------|------|-------------|
| `data` | `const uint8_t*` | Pointer to file data |
| `size` | `uint64_t` | Size in bytes |
| `ownsData` | `bool` | `false` = zero-copy pointer into mmap (keeps mapping alive via shared ownership -- valid even after Close). `true` = allocated buffer (always valid, freed on destruction) |

### PakOptions

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `compress` | `bool` | `false` | Enable LZ4 per-file compression |
| `alignment` | `uint32_t` | `16` | Data alignment in bytes (must be power of 2) |
| `formatVersion` | `uint32_t` | `3` | PAK format version (2 for legacy, 3 for aligned) |

## PAK Format

### Header (40 bytes)

| Field | Type | Description |
|-------|------|-------------|
| magic | char[4] | `"PAK0"` |
| version | uint32_t | 1 (legacy), 2 (compression), or 3 (alignment) |
| numFiles | uint32_t | Number of files |
| fileTableOffset | uint64_t | Byte offset to file table |
| alignment | uint32_t | Data alignment in bytes (v3 only; 0 in v1/v2) |
| reserved | uint32_t[3] | Reserved for future use |

### File table entry (v2/v3)

| Field | Type | Description |
|-------|------|-------------|
| nameLength | uint16_t | Filename length |
| filename | char[] | UTF-8 filename |
| offset | uint64_t | Byte offset to file data |
| originalSize | uint64_t | Uncompressed size |
| compressedSize | uint64_t | Size on disk |
| flags | uint8_t | Bit 0: LZ4 compressed |

v1 entries omit `compressedSize` and `flags`. PakReader reads all three format versions.

In v3, file data offsets are padded to the alignment boundary specified in the header. This ensures page-aligned access when memory-mapped.

## Thread Safety

### PakReader (runtime)

`PakReader` is **thread-safe** for concurrent reads:

- `ReadFile()`, `ReadFileZeroCopy()`, `LoadFile()`, `ReadFiles()`, `FileExists()`, `GetFileCount()`, `GetFileInfo()`, `IsOpen()`, `IsMapped()` -- safe to call concurrently from any number of threads.
- `Open()` and `Close()` take exclusive locks. Do not call them while reads are in progress.
- Non-owning `PakSpan` objects (from `ReadFileZeroCopy()`) keep the mapped memory alive via shared ownership. They remain valid even after `Close()` is called.
- Moving a `PakReader` acquires exclusive locks. The source must not be in use by other threads during the move.

Internally:
- A `shared_mutex` allows concurrent readers while serializing against `Open()`/`Close()`.
- Read methods release the lock after entry lookup -- I/O and decompression proceed without blocking exclusive operations.
- With memory-mapped I/O, reads proceed in parallel with no syscall overhead.
- The ifstream fallback serializes I/O through a separate mutex.

### Pakker (build-time)

`Pakker` is **not thread-safe**. Use separate instances for concurrent PAK creation (each instance has no shared mutable state).

### Logging

`PakSetLogCallback()` is thread-safe. The callback itself must be safe for concurrent invocation.

## Performance

| Operation | mmap | ifstream fallback |
|-----------|------|-------------------|
| File lookup | O(1) hash, no I/O | O(1) hash, no I/O |
| Uncompressed read | Zero-copy pointer | seek + read |
| Compressed read | Decompress from mmap (no read syscall) | seek + read + decompress |
| Batch read | Parallel decompression across cores | Sequential by offset |
| Concurrent reads | Fully parallel | Serialized through mutex |

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_EXAMPLES` | `ON` | Build the example executable |
| `PAK_NO_MMAP` | `OFF` | Disable memory-mapped I/O (uses ifstream only) |

```bash
cmake -B build -DPAK_NO_MMAP=ON  # Disable mmap
```

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## License

This project is licensed under the MIT License - see the LICENSE file for details.
