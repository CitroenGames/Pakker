# Pakker

Pakker is a high-performance asset archive library designed for 3D game engines. Bundle assets into a single `.pak` file at build time, then read them at runtime with memory-mapped I/O, zero-copy reads, and thread-safe concurrent access.

## Features

- **Memory-mapped I/O** -- maps the entire PAK into the address space; the OS handles caching, prefetching, and page management
- **Zero-copy mapped views** (`PakView`) -- returns a direct pointer into mapped memory for uncompressed assets, eliminating heap allocation entirely
- **Handle-based runtime API** -- resolve paths once with `Find()`/`Resolve()`, cache `PakFileHandle`, then read without repeated path normalization or lookup allocation
- **Caller-owned reads** -- `Read(handle, span)` fills engine-owned buffers and reports precise `PakStatus` errors
- **Page-aligned data** (v8 format) -- file data aligned to configurable boundaries (16 B, 4 KB, 64 KB) for efficient page faults and GPU upload
- **Per-entry content integrity hashing** -- XXH64 checksum of each entry's on-disk bytes, with opt-in deep verification (`ValidatePak(deepVerify)`, `PakReader::VerifyEntry()`, `PakOpenOptions::verifyOnRead`) to catch corrupted or tampered archives before they reach the engine
- **Layered archives** (`PakMount`) -- mount a base archive plus patch/DLC/language archives with override semantics, resolved through one composed namespace
- **Loose-file hot-reload override** (`PakLooseOverlay`) -- dev-only wrapper that serves an edited-on-disk asset over the archive's copy, for iteration without repacking
- **Lock-free read path** -- reads reach an immutable published snapshot through a single atomic load, taking no lock and no reference count, so lookup and read throughput scale with worker-thread count
- **Hybrid decoded caching** -- OS page cache/mmap remains the fast path, with default in-memory and persistent decoded caches for compressed, encrypted, and non-mmap reads
- **O(1) file lookup** via a flat open-addressing index built over stored path hashes -- no per-entry allocation, no string hashing at open
- **LZ4 or Zstd per-file compression** -- LZ4 for latency-sensitive hot-reload style reads (~4 GB/s decode), Zstd for a better ratio at similar decode cost; selected per-file, both pluggable side by side in the same archive
- **Partial/streaming reads** (`PakReader::ReadRange`) -- read a slice of a large asset without materializing the whole entry; works on compressed entries too when built with chunking
- **Chunked compression** -- split compressed entries into independently-decodable blocks, bounding time-to-first-byte on a large asset to one block instead of the whole file
- **Parallel packing** -- entry compression, encryption and hashing fan out across worker threads while output stays byte-identical and deterministically ordered
- **Scatter reads** (`PakReader::ReadBatch`) -- serve a whole set of reads in archive-offset order with batched prefetch hints, instead of one call per asset
- **Hash-keyed lookups** (`PakPathHash`, `FindByHash`) -- carry baked 64-bit asset ids instead of path strings, and optionally drop the name blob entirely in shipping builds
- **Silent by default** -- no console output unless you set a log callback
- **Optional XOR encryption** -- disabled by default for best performance; this is lightweight obfuscation, not a security boundary -- see Encryption below
- **No external dependencies** -- LZ4 and Zstd are vendored, only needs C++20 and a C compiler

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
opts.compression = PakCompression::LZ4;   // or PakCompression::Zstd for a smaller archive
opts.alignment = 4096;                    // 4 KB page alignment

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

// Optional tuning: caching is enabled by default with conservative budgets.
PakCacheOptions cacheOptions;
cacheOptions.memoryBudgetBytes = 128ull * 1024ull * 1024ull;
cacheOptions.persistentCacheDirectory = "my/game/cache"; // useful on Android/Web
reader.SetCacheOptions(cacheOptions);

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

### Layered archives: PakMount

`PakMount` composes multiple already-open (or opened-on-mount) `PakReader`
archives into one logical namespace with override semantics: later-mounted
layers win on name conflicts. This is how a base archive plus patch/DLC/
language archives are typically shipped -- mount the base, then layer patches
and DLC on top without rebuilding anything.

```cpp
#include "Pak.h"

// Base archive, patch, and DLC/language pack -- later mounts win on conflict
PakMount mount;
mount.Mount("base.pak");
mount.Mount("patch_1_2.pak");
mount.Mount("dlc_desert_pack.pak");

// Same ergonomics as PakReader: resolve once, dispatch by handle
PakMountHandle texture = mount.Find("textures/diffuse.dds");
if (texture) {
    std::vector<uint8_t> data;
    mount.Load(texture, data); // served from whichever layer wins -- dlc > patch > base
}

// Share an already-open PakReader between direct access and a mount
auto baseReader = std::make_shared<PakReader>();
baseReader->Open("base.pak");
mount.MountReader(baseReader);
baseReader->GetFileCount(); // still usable directly

// Per-layer cache tuning
if (auto base = mount.GetLayerReader(0)) {
    PakCacheOptions opts;
    opts.memoryBudgetBytes = 256ull * 1024ull * 1024ull;
    base->SetCacheOptions(opts);
}

// Deduplicated, override-aware enumeration
for (const auto& name : mount.ListFiles()) { /* ... */ }
```

`PakMount` v1 has no per-layer `Unmount()` -- only `Clear()` (drop every
layer). The dominant mount pattern is "mount everything once at a load-screen
boundary," which `Clear()` + re-`Mount()` covers; selective removal is a
natural follow-up if a use case needs it.

### Partial reads for large assets

`PakReader::ReadRange()` reads a slice of an entry's decoded content without
materializing the whole thing -- useful for large single assets like video or
audio that only need a chunk streamed in at a time:

```cpp
PakFileHandle handle = reader.Find("video/intro.h264");
std::vector<uint8_t> chunk(64 * 1024);
uint64_t written = 0;
PakStatus status = reader.ReadRange(handle, /*rangeOffset=*/1'048'576, chunk, &written);
```

Supported for uncompressed entries (encrypted or not), and for compressed
entries built with `PakOptions::compressionChunkSize` set -- see Chunked
compression below. A compressed entry stored as one whole-entry frame has no
internal index to seek into, so it returns `PakStatus::Unsupported` rather
than silently decoding the whole entry to fake partial-read semantics.
Media that's already compressed
in its own codec (most video/audio) should be stored uncompressed at the
archive level if streaming reads are needed.

### Chunked compression

By default a compressed entry is one compression frame over the whole file, so
reaching any byte of it means decoding all of it. On a large streaming asset
that is a multi-millisecond stall, and `ReadRange()` cannot work at all.

`PakOptions::compressionChunkSize` splits compressed entries into
independently-decodable blocks:

```cpp
PakOptions opts;
opts.compression = PakCompression::Zstd;
opts.compressionChunkSize = 256 * 1024;   // 256 KiB blocks
pakker.CreatePak("streaming.pak", files, opts);
```

Only entries larger than the chunk size are chunked, so small assets are
unaffected. `PakFileInfo::chunkSize` reports the block size of an entry
(`0` when it is not chunked), which lets a streamer align its requests to
block boundaries and avoid decoding a block twice.

Measured on a 64 MiB Zstd-3 asset, reading the first 64 KiB:

| | whole-entry | chunked (256 KiB) |
|---|---|---|
| time to first 64 KiB | 9,848 us | **44.9 us** |
| seek to the last 64 KiB | 9,848 us | **34.8 us** |
| `ReadRange()` | `Unsupported` | supported |
| archive size | 1.85:1 | 1.78:1 |

It is off by default because that last row is a real cost: compressing each
block independently gives up cross-block matches, and roughly 4% of ratio on a
full-size install is gigabytes of download. Turn it on for streaming builds,
measure the ratio cost on your own content, and decide.

### Parallel packing

Entry compression, encryption and hashing run on `PakOptions::workerThreads`
workers (default: one per hardware thread). Entries are encoded in parallel but
written in input order, so the archive is byte-identical no matter how many
workers ran.

```cpp
PakOptions opts;
opts.compression = PakCompression::Zstd;
opts.zstdLevel = 19;
opts.workerThreads = 0;   // 0 = one per hardware thread, 1 = sequential
```

At the default level 19, compression completely dominates pack time. Measured
on 64 MiB across 360 entries on a 32-thread machine:

| workers | time | speedup |
|---|---|---|
| 1 | 7.48 s | 1.00x |
| 4 | 2.16 s | 3.46x |
| 8 | 1.20 s | 6.25x |
| 32 | 0.70 s | **10.72x** |

Memory is bounded by batching: only one batch of entries is resident at a
time, capped by both entry count and total bytes, so a handful of very large
assets cannot balloon the working set.

### Scatter reads

`ReadBatch()` serves a whole set of reads in one call, visiting entries in
ascending archive offset order rather than in the order they were requested:

```cpp
std::vector<std::vector<uint8_t>> buffers(count);
std::vector<PakReadRequest> requests(count);
for (size_t i = 0; i < count; ++i) {
    requests[i].handle = handles[i];
    requests[i].destination = buffers[i];
}

size_t ok = reader.ReadBatch(requests);
// per-request outcomes in requests[i].status / requests[i].bytesWritten
```

The request array is not reordered -- ordering is internal. Two things make
this worth having over a loop of `Read()` calls: a scattered access pattern
becomes a forward sweep, which is what storage readahead is built to
accelerate, and every entry is prefetch-hinted up front so faults overlap
instead of serializing.

`ReadBatch()` does not create threads. Reads are lock-free, so split a frame's
requests across your own job-system workers and call it once per worker.

`PakBatchOptions::prefetch` (default on) controls the hinting. It pays when
the data is not resident -- which is the normal case for streaming -- and is a
straight loss when it already is, since the hint then costs a syscall for
nothing. Turn it off for re-reads of data known to be hot. Measured warm, with
256 scattered 4 KiB reads:

| | mean | p99 | max |
|---|---|---|---|
| `Read()` loop | 40.5 us | 74.0 us | 352.6 us |
| `ReadBatch()`, ordering only | 40.7 us | 62.4 us | **70.2 us** |
| `ReadBatch()` + prefetch | 71.9 us | 130.5 us | 146.5 us |

Ordering is free even warm, and cuts worst-case latency 5x. The prefetch cost
shown here is the price of a benefit this warm-cache benchmark cannot measure.

### Loose-file hot-reload override: PakLooseOverlay

`PakLooseOverlay` wraps an already-open `PakReader` plus a directory on disk.
Lookups check the loose directory first and serve from there when present,
else fall back to the archive -- so an artist/programmer can edit an asset on
disk and see the change without repacking:

```cpp
#include "PakLooseOverlay.h"

auto reader = std::make_shared<PakReader>();
reader->Open("assets.pak");

PakLooseOverlay overlay(reader, "path/to/loose/override/dir");

std::vector<uint8_t> data;
overlay.Load("textures/diffuse.dds", data); // loose copy if present, else the archive's
```

This is a **dev-only convenience, not a shipping-path optimization**: every
lookup does a filesystem stat, which doesn't belong in a shipping hot path
(unlike `PakReader`/`PakMount`, which are handle-based specifically to avoid
this). Loose files are read raw -- no compression or encryption.

### Integrity verification

Every entry stores an XXH64 hash of its on-disk (post-compression,
post-encryption) bytes, computed at build time. Verification is opt-in and
off the default hot path:

```cpp
// Build-time / QA: deep-verify every entry's bytes against its stored hash
Pakker pakker;
bool ok = pakker.ValidatePak("assets.pak", /*deepVerify=*/true);

// Runtime: verify one entry off the hot path (QA sweep, "verify game files")
PakReader reader;
reader.Open("assets.pak");
PakFileHandle handle = reader.Find("textures/diffuse.dds");
if (reader.VerifyEntry(handle) != PakStatus::Ok) {
    // corrupted or tampered -- handle accordingly
}

// Runtime: fail closed on every Read()/Load() instead of returning bad data
PakOpenOptions openOptions;
openOptions.verifyOnRead = true;
reader.Open("assets.pak", openOptions);
```

`View()` is never verified, even under `verifyOnRead` -- it returns a raw
pointer into mapped memory by contract, and hashing on every call would
defeat that zero-copy guarantee the same way decoded caching deliberately
skips `View()` (see Caching below). Callers needing an integrity guarantee
for `View()`-eligible content should call `VerifyEntry()` once (e.g. at
startup) rather than relying on `View()`'s pointer directly.

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

### Hash-keyed lookups

Every entry stores an XXH64 hash of its normalized path, and `PakPathHash()`
computes the same value for a string. Bake those hashes into your asset
references at build time and the runtime never has to touch a path string:

```cpp
// Build time: emit a header/blob of asset ids.
uint64_t kPlayerDiffuse = PakPathHash("textures/player/diffuse.dds");

// Runtime: no hashing, no string compare, no allocation.
PakFileHandle handle = reader.FindByHash(kPlayerDiffuse);
```

`FindByHash()` skips both the path hashing and the name confirmation that
`Find()` performs, which is most of `Find()`'s cost. The trade is that a
64-bit hash collision resolves to the wrong entry instead of failing --
around a 3e-8 chance across a million assets. `Open()` rejects any archive
whose entries do not all have distinct path hashes, so a collision cannot be
introduced silently at build time.

Once every lookup goes through hashes, the name blob is dead weight in a
shipping build. `PakOpenOptions::loadNames = false` leaves it on disk:

```cpp
PakOpenOptions options;
options.loadNames = false;  // ~50 bytes/entry saved, much faster Open()
reader.Open("assets.pak", options);
```

Keep names loaded for tools, editors, and anything that reports an asset path
to a human.

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
| `CreatePak(filename, files, PakOptions)` | Create v8 PAK with alignment, compression and worker-thread control |
| `CreatePakFromFolder(filename, folder, PakOptions)` | Create PAK from directory with options |
| `ExtractPak(filename, outputDir)` | Extract all files to a directory |
| `ListPak(filename)` | Log all file entries via the log callback |
| `AddFileToPak(filename, name, data, compression=None, zstdLevel=19)` | Append a file to an existing PAK |
| `ValidatePak(filename, deepVerify=false)` | Validate header/entry bounds; `deepVerify` additionally re-hashes every entry against its stored content hash (O(archive size)) |
| `ReadFileFromPak(filename, name)` | Read a single file (one-shot, no caching) |
| `GetFileCount(filename)` | Get file count from header |
| `FileExists(filename, name)` | Check if a file exists |
| `GetFileInfo(filename, name)` | Get file metadata |

## Runtime API (PakReader)

| Method | Description |
|--------|-------------|
| `Open(filename)` / `Open(filename, PakOpenOptions)` | Open PAK, parse file table, memory-map file; `PakOpenOptions::verifyOnRead` enables fail-closed content-hash verification on every `Read`/`Load` |
| `Close()` | Unmap file, close handle, free cached data |
| `IsOpen()` | Check if a PAK is currently open |
| `IsMapped()` | Check if memory-mapped I/O is active |
| `Find(name)` | O(1) lookup, returns `PakFileHandle`; confirms the match against the stored name |
| `FindByHash(pathHash)` | O(1) lookup by a precomputed `PakPathHash()`; skips path hashing and name comparison entirely |
| `Resolve(names, handles)` | Batch lookup for engine asset registration |
| `Info(handle)` | Returns cached metadata for a handle |
| `InfoByIndex(index)` | Returns cached metadata for the entry at a dense `[0, GetFileCount())` index; used by enumeration/composition layers such as `PakMount` |
| `View(handle, view)` | Zero-copy mapped view for uncompressed, unencrypted data; never content-hash verified, even under `verifyOnRead` |
| `Read(handle, span)` | Fill caller-owned memory, returns `PakStatus` |
| `Load(handle, vector)` | Allocating convenience read into a vector |
| `ReadRange(handle, rangeOffset, span)` | Partial read of an uncompressed entry's decoded content; `PakStatus::Unsupported` for compressed entries |
| `Prefetch(handle)` | Best-effort mapped range prefetch hint |
| `VerifyEntry(handle)` | Off-hot-path integrity check: re-hashes on-disk bytes against the stored content hash, returns `PakStatus::HashMismatch` on failure |
| `FileExists(name)` | O(1) hash lookup, no disk I/O |
| `GetFileCount()` | Returns cached count, no disk I/O |
| `GetFileInfo(name)` | Convenience lookup for size, compression info |
| `ListFiles()`, `ListFilesWithPrefix(prefix)` | Enumerate all entries from the cached file table, no disk I/O |
| `ReadFile`, `LoadFile`, `ReadFileZeroCopy`, `ReadFiles` | Convenience wrappers |
| `SetCacheOptions`, `GetCacheStats`, `ClearCache` | Tune and inspect decoded asset caching |

### PakView

`View(handle, view)` fills a `PakView`:

| Field | Type | Description |
|-------|------|-------------|
| `data` | `const uint8_t*` | Pointer to file data |
| `size` | `uint64_t` | Size in bytes |
| `mapped` | `bool` | `true` when `data` points into the mapped PAK. The view keeps mapping ownership, so it remains valid after `Close()` |

## Runtime API (PakMount)

| Method | Description |
|--------|-------------|
| `Mount(filename, encryptionKey="")` | Opens a new `PakReader` and mounts it as the highest-priority layer so far |
| `MountReader(reader)` | Mounts an externally-owned, already-open `shared_ptr<PakReader>` as the highest-priority layer |
| `Clear()` | Unmounts every layer |
| `LayerCount()` | Number of mounted layers |
| `GetLayerReader(index)` | Direct access to a layer's own `PakReader` (index 0 = first mounted); use for per-layer `SetCacheOptions` |
| `Find(name)` / `Resolve(names, handles)` | Resolve across layers, highest-priority match wins; returns `PakMountHandle` |
| `Info(handle)`, `View(handle, view)`, `Read(handle, span)`, `Load(handle, vector)`, `Prefetch(handle)` | Dispatch straight to the winning layer's own `PakReader` method |
| `ReadFile`, `LoadFile`, `ReadFileZeroCopy` | Convenience wrappers |
| `FileExists(name)`, `GetFileCount()` | Query the deduplicated merged namespace |
| `ListFiles()`, `ListFilesWithPrefix(prefix)` | Deduplicated, override-aware enumeration |

`PakMount` introduces no additional caching layer -- reads are dispatched to
whichever layer's `PakReader` owns the winning entry, and that reader's own
decoded cache applies transparently.

## Runtime API (PakLooseOverlay)

| Method | Description |
|--------|-------------|
| `PakLooseOverlay(reader, looseDirectory)` | Wraps an already-open `shared_ptr<PakReader>` plus a loose directory path |
| `FileExists(name)` | True if the file exists in the loose directory or the wrapped reader |
| `Read(name, span)` | Serves from the loose directory when present (`BufferTooSmall` if the buffer is too small), else delegates to the wrapped reader |
| `Load(name, vector)` | Same precedence as `Read`, into an owned resized buffer |
| `WrappedReader()`, `LooseDirectory()` | Accessors |

Filename-keyed, not handle-based -- every lookup stats the filesystem by
design (that's what makes hot-reload work), so a pre-resolved handle would
just cache a stale answer. No `View()`/zero-copy equivalent: a loose file
read from disk has no mapping to back that contract.

### PakOptions

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `compression` | `PakCompression` | `None` | Per-file compression method: `None`, `LZ4`, or `Zstd` |
| `zstdLevel` | `int` | `19` | Zstd compression level (1-19); only consulted when `compression == Zstd` |
| `alignment` | `uint32_t` | `16` | Data alignment in bytes (must be power of 2) |
| `workerThreads` | `uint32_t` | `0` | Threads used to compress, encrypt and hash entries. `0` means one per hardware thread; `1` forces a sequential pipeline. Output is byte-identical regardless |
| `compressionChunkSize` | `uint32_t` | `0` | Split compressed entries into independently-decodable blocks of this many uncompressed bytes. `0` disables chunking; otherwise a power of two between 4 KiB and 16 MiB |

### PakOpenOptions

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `verifyOnRead` | `bool` | `false` | Re-hash on-disk bytes on every `Read()`/`Load()` and fail closed with `PakStatus::HashMismatch` on a mismatch. Costs real throughput (forces a full read+hash pass even on the mmap path); prefer `VerifyEntry()` for off-hot-path verification |
| `loadNames` | `bool` | `true` | Read the archive's name blob into memory. Turning it off saves roughly 50 bytes per entry and makes `Open()` markedly faster, at the cost of `PakFileInfo::filename`, `ListFiles()`, and name-based collision confirmation |

### PakCacheOptions

Runtime decoded caching is enabled by default:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enabled` | `bool` | `true` | Master switch for decoded caching |
| `memoryCacheEnabled` | `bool` | `true` | Keep decoded entries in process memory |
| `persistentCacheEnabled` | `bool` | `true` | Store decoded entries in a platform cache directory |
| `memoryBudgetBytes` | `uint64_t` | `64 MiB` | O(1) LRU memory cache budget |
| `persistentBudgetBytes` | `uint64_t` | `512 MiB` | LRU persistent cache budget |
| `maxSingleEntryBytes` | `uint64_t` | `64 MiB` | Largest decoded entry eligible for caching |
| `persistentCacheDirectory` | `std::string` | empty | Empty uses the OS default when available |

`View()` remains mmap-only and never returns decoded cache storage. `Read()` and
`Load()` cache decoded data for compressed, encrypted, and non-mmap reads. On
Android and Web, set `persistentCacheDirectory` to an app-owned writable path if
you want persistent caching; otherwise memory caching still works.
`PakOpenOptions::verifyOnRead` bypasses decoded caches so every read verifies
the current archive bytes.

## PAK Format

### Header (56 bytes)

| Field | Type | Description |
|-------|------|-------------|
| magic | char[4] | `"PAK0"` |
| version | uint32_t | 8 |
| numFiles | uint32_t | Number of files |
| alignment | uint32_t | Data alignment in bytes |
| fileTableOffset | uint64_t | Byte offset to the entry-record array |
| nameBlobOffset | uint64_t | Byte offset to the packed name bytes |
| nameBlobSize | uint64_t | Size of the name blob |
| reserved | uint64_t[2] | Reserved for future use |

### File table entry (v8) -- fixed 48 bytes

| Field | Type | Description |
|-------|------|-------------|
| offset | uint64_t | Byte offset to file data |
| originalSize | uint64_t | Uncompressed size |
| compressedSize | uint64_t | Size on disk |
| contentHash | uint64_t | XXH64 of the on-disk (compressed+encrypted) bytes |
| pathHash | uint64_t | XXH64 of the normalized name -- see `PakPathHash()` |
| nameOffset | uint32_t | Byte offset of the name within the name blob |
| nameLength | uint16_t | Name length in bytes |
| flags | uint8_t | Bit 0: LZ4 compressed. Bit 1: Zstd compressed (mutually exclusive with bit 0). Bit 2: chunked |
| chunkSizeLog2 | uint8_t | log2 of the uncompressed bytes per block, when bit 2 is set |

Entry records are fixed-size and all names live together in one contiguous
blob after the record array. That is what lets `PakReader::Open()` load the
whole file table in two bulk reads and use the record array directly as its
runtime representation, instead of parsing entry by entry and allocating a
string per file. Because the path hash is stored, opening does no string
hashing at all, and layered mounts can merge namespaces without re-hashing.

File data offsets are padded to the alignment boundary specified in the
header. This ensures page-aligned access when memory-mapped.

### Chunked entry payload

When flag bit 2 is set, the entry's payload is not one compression frame over
the whole entry. It is:

| Part | Type | Description |
|------|------|-------------|
| chunkSize | uint32_t | Uncompressed bytes per block (power of two) |
| chunkCount | uint32_t | Number of blocks |
| compressedSizes | uint32_t[chunkCount] | On-disk size of each block |
| blocks | bytes | Each block's data, back to back |

Each block is compressed independently, so any one of them can be decoded
without touching the others. A block that does not compress is stored raw
(its compressed size equals its uncompressed size), so incompressible data
never pays an expansion penalty.

This is what makes `ReadRange()` work on a compressed entry, and it bounds
time-to-first-byte on a large asset to one block instead of the whole file.
See `PakOptions::compressionChunkSize`.

Archives are v8-only: `PakReader::Open()` and `Pakker::ValidatePak()` reject
any other version, including v7 (no chunked-entry flag), v6 (variable-length
file table), v5 (no Zstd flag bit) and v4 (no `contentHash` field). There is
no in-place upgrade tool -- rebuild older archives with
`CreatePak()`/`CreatePakFromFolder()` under the current library version.

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

### PakMount (runtime)

`PakMount` is **thread-safe** for concurrent reads, and composes with `PakReader`'s own contract:

- `Find()`, `Resolve()`, `Info()`, `View()`, `Read()`, `Load()`, `Prefetch()`, `ListFiles()`, `ListFilesWithPrefix()`, and convenience wrappers -- safe to call concurrently from any number of threads, and safe alongside reads issued directly against a `shared_ptr<PakReader>` an engine also holds outside the mount.
- `Mount()`, `MountReader()`, and `Clear()` take exclusive locks and rebuild the merged lookup index. Do not call them while reads are in progress -- same rule as `PakReader::Open()`/`Close()`.
- `PakMountHandle` values remain valid as long as the layer they reference is still mounted; do not use a handle resolved before a `Clear()` call.
- Moving a `PakMount` acquires an exclusive lock. The source must not be in use by other threads during the move.

### Pakker (build-time)

`Pakker` is **not thread-safe**. Use separate instances for concurrent PAK creation (each instance has no shared mutable state).

### Logging

`PakSetLogCallback()` is thread-safe. The callback itself must be safe for concurrent invocation.

## Performance

| Operation | mmap | ifstream fallback |
|-----------|------|-------------------|
| File lookup | O(1) hash, no I/O | O(1) hash, no I/O |
| Uncompressed read | `View()`: zero-copy; `Read()`: mapped memcpy | seek + read |
| Compressed read | Decompress from mmap (no read syscall) | seek + read + decompress |
| Batch resolve | O(1) handle lookup per name | O(1) handle lookup per name |
| Parallel reads | Dispatch cached handles through engine jobs | Serialized by stream mutex |
| Concurrent reads | Fully parallel | Serialized through mutex |
| Repeated compressed/encrypted reads | Decoded memory/persistent cache | Decoded memory/persistent cache |

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_EXAMPLES` | `ON` | Build the example executable |
| `BUILD_TESTING` | `ON` | Build and register runtime tests through CTest |
| `BUILD_BENCHMARKS` | `ON` | Build the runtime benchmark executable |
| `PAK_NO_MMAP` | `OFF` | Disable memory-mapped I/O (uses ifstream only) |
| `PAK_WARNINGS_AS_ERRORS` | `OFF` | Treat compiler warnings as errors (`/WX` or `-Werror`) |
| `PAK_ENABLE_LTO` | `OFF` | Enable interprocedural optimization (LTO) for the library target |

```bash
cmake -B build -DPAK_NO_MMAP=ON  # Disable mmap
```

## Installing / consuming via CMake

Pakker exports a CMake package, so it can be consumed either in-tree or as an installed dependency:

```cmake
# In-tree (add_subdirectory or FetchContent)
add_subdirectory(path/to/Pakker)
target_link_libraries(YourTarget PRIVATE Pakker::Pakker)
```

```bash
# Or install it and consume via find_package
cmake -S . -B build
cmake --build build --config Release
cmake --install build --prefix /path/to/install
```

```cmake
find_package(Pakker CONFIG REQUIRED)
target_link_libraries(YourTarget PRIVATE Pakker::Pakker)
```

The exported target propagates its C++20 requirement and public include directory automatically -- no manual `CMAKE_CXX_STANDARD` bump needed on the consumer side.

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## License

This project is licensed under the MIT License - see the LICENSE file for details.
