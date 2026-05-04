# Pakker

Pakker is a high-performance asset archive library designed for 3D game engines. Bundle assets into a single `.pak` file at build time, then read them at runtime with memory-mapped I/O, zero-copy reads, and thread-safe concurrent access.

## Features

- **Memory-mapped I/O** -- maps the entire PAK into the address space; the OS handles caching, prefetching, and page management
- **Zero-copy mapped views** (`PakView`) -- returns a direct pointer into mapped memory for uncompressed assets, eliminating heap allocation entirely
- **Handle-based runtime API** -- resolve paths once with `Find()`/`Resolve()`, cache `PakFileHandle`, then read without repeated path normalization or lookup allocation
- **Caller-owned reads** -- `Read(handle, span)` fills engine-owned buffers and reports precise `PakStatus` errors
- **Page-aligned data** (v4 format) -- file data aligned to configurable boundaries (16 B, 4 KB, 64 KB) for efficient page faults and GPU upload
- **Thread-safe PakReader** -- `shared_mutex` allows concurrent reads from multiple threads; ideal for asset streaming on worker threads
- **O(1) file lookup** via hash map -- no linear scanning
- **LZ4 per-file compression** -- fast decompression (~4 GB/s); each entry records whether it needs decompression
- **Silent by default** -- no console output unless you set a log callback
- **Optional XOR encryption** -- disabled by default for best performance
- **No external dependencies** -- LZ4 is vendored, only needs C++20 and a C compiler

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

pakker.CreatePak("assets.pak", files, opts);

// Or create from a folder
pakker.CreatePakFromFolder("assets.pak", "path/to/assets", opts);
```

### Runtime: read assets with PakReader

```cpp
#include "Pak.h"

PakReader reader;
reader.Open("assets.pak");

// Check if mmap is active
if (reader.IsMapped())
    std::cout << "Using memory-mapped I/O" << std::endl;

// Resolve once during asset registration/loading
PakFileHandle texture = reader.Find("textures/diffuse.dds");
if (!texture) {
    // Not found
}

const PakFileInfo* info = reader.Info(texture);

// Caller-owned read buffer
std::vector<uint8_t> data(info->originalSize);
PakStatus readStatus = reader.Read(texture, data);

// Zero-copy read -- direct pointer into mmap for uncompressed assets
PakView view;
PakStatus viewStatus = reader.View(texture, view);
if (viewStatus == PakStatus::Ok) {
    // view.data points directly into mapped memory -- no copy, no allocation
    // Upload to GPU: glTexImage2D(..., view.data) or equivalent
}

// Batch resolve, then dispatch handle reads through your engine job system
std::string_view names[] = {"sounds/hit.wav", "textures/diffuse.dds", "models/player.glb"};
PakFileHandle handles[3];
size_t found = reader.Resolve(names, handles);
```

### Thread-safe concurrent reads

```cpp
PakReader reader;
reader.Open("assets.pak");

// Safe to read from multiple threads simultaneously
std::vector<std::thread> workers;
for (int i = 0; i < 8; ++i) {
    workers.emplace_back([&reader]() {
        PakFileHandle h = reader.Find("textures/diffuse.png");
        std::vector<uint8_t> data;
        reader.Load(h, data);
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

### Zero-copy GPU upload

For textures stored in GPU-ready formats (DDS, KTX2, Basis), use `View(handle, PakView&)` with `alignment = 4096` or `65536`. The returned pointer is page-aligned and can be passed directly to graphics API upload functions without an intermediate copy.

### Asset streaming pattern

```cpp
// Worker thread: stream assets from PAK
void AssetStreamWorker(PakReader& reader, AssetQueue& queue) {
    while (auto request = queue.pop()) {
        PakView view;
        if (reader.View(request->handle, view) == PakStatus::Ok) {
            request->callback(view.data, view.size);
        }
    }
}
```

## Build-time API (Pakker)

| Method | Description |
|--------|-------------|
| `CreatePak(filename, files, PakOptions)` | Create v4 PAK with alignment and compression control |
| `CreatePakFromFolder(filename, folder, PakOptions)` | Create PAK from directory with options |
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
| `Find(name)` | O(1) lookup, returns `PakFileHandle` |
| `Resolve(names, handles)` | Batch lookup for engine asset registration |
| `Info(handle)` | Returns cached metadata for a handle |
| `View(handle, view)` | Zero-copy mapped view for uncompressed, unencrypted data |
| `Read(handle, span)` | Fill caller-owned memory, returns `PakStatus` |
| `Load(handle, vector)` | Allocating convenience read into a vector |
| `Prefetch(handle)` | Best-effort mapped range prefetch hint |
| `FileExists(name)` | O(1) hash lookup, no disk I/O |
| `GetFileCount()` | Returns cached count, no disk I/O |
| `GetFileInfo(name)` | Convenience lookup for size, compression info |
| `ReadFile`, `LoadFile`, `ReadFileZeroCopy`, `ReadFiles` | Convenience wrappers |

### PakView

`View(handle, view)` fills a `PakView`:

| Field | Type | Description |
|-------|------|-------------|
| `data` | `const uint8_t*` | Pointer to file data |
| `size` | `uint64_t` | Size in bytes |
| `mapped` | `bool` | `true` when `data` points into the mapped PAK. The view keeps mapping ownership, so it remains valid after `Close()` |

### PakOptions

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `compress` | `bool` | `false` | Enable LZ4 per-file compression |
| `alignment` | `uint32_t` | `16` | Data alignment in bytes (must be power of 2) |

## PAK Format

### Header (36 bytes)

| Field | Type | Description |
|-------|------|-------------|
| magic | char[4] | `"PAK0"` |
| version | uint32_t | 4 |
| numFiles | uint32_t | Number of files |
| fileTableOffset | uint64_t | Byte offset to file table |
| alignment | uint32_t | Data alignment in bytes |
| reserved | uint32_t[3] | Reserved for future use |

### File table entry (v4)

| Field | Type | Description |
|-------|------|-------------|
| nameLength | uint16_t | Filename length |
| filename | char[] | UTF-8 filename |
| offset | uint64_t | Byte offset to file data |
| originalSize | uint64_t | Uncompressed size |
| compressedSize | uint64_t | Size on disk |
| flags | uint8_t | Bit 0: LZ4 compressed |

File data offsets are padded to the alignment boundary specified in the header. This ensures page-aligned access when memory-mapped.

## Thread Safety

### PakReader (runtime)

`PakReader` is **thread-safe** for concurrent reads:

- `Find()`, `Resolve()`, `Info()`, `View()`, `Read()`, `Load()`, `Prefetch()`, and convenience wrappers -- safe to call concurrently from any number of threads.
- `Open()` and `Close()` take exclusive locks. Do not call them while reads are in progress.
- `PakView` objects keep mapped memory alive via shared ownership. They remain valid even after `Close()` is called.
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
| Batch resolve | O(1) handle lookup per name | O(1) handle lookup per name |
| Parallel reads | Dispatch cached handles through engine jobs | Serialized by stream mutex |
| Concurrent reads | Fully parallel | Serialized through mutex |

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_EXAMPLES` | `ON` | Build the example executable |
| `BUILD_TESTING` | `ON` | Build and register runtime tests through CTest |
| `BUILD_BENCHMARKS` | `ON` | Build the runtime benchmark executable |
| `PAK_NO_MMAP` | `OFF` | Disable memory-mapped I/O (uses ifstream only) |

```bash
cmake -B build -DPAK_NO_MMAP=ON  # Disable mmap
```

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## License

This project is licensed under the MIT License - see the LICENSE file for details.
