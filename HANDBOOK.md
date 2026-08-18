# Pakker Handbook

This handbook is for contributors working on Pakker itself. The README explains
how to use the library; this file explains how the project is put together, what
runtime guarantees matter, and what to verify when changing it.

## Project Goal

Pakker is a small C++20 asset archive library aimed at game-engine runtime use.
Its main job is to build `.pak` archives offline, then serve assets at runtime
with predictable lookup cost, memory-mapped I/O when available, zero-copy views
for eligible files, and thread-safe concurrent reads.

The current archive format is v8-only.

## Repository Map

| Path | Purpose |
|------|---------|
| `src/Pak.h` | Public API, archive structures, runtime handles, cache options, `PakMount`, and thread-safety contract |
| `src/PakInternal.h` | Private implementation header: codec-neutral size ceiling and FNV fingerprint helpers (non-persisted values only) shared across the files below |
| `src/PakHash.cpp` | XXH64 wrappers behind `PakPathHash()` / `PakInternal::HashBytesFast()`; the only translation unit that includes the vendored xxHash header |
| `src/PakCommon.cpp` | Shared archive-format contract: logging, path/filename validation, header and file-table I/O, encryption |
| `src/PakCompression.h`, `src/PakCompression.cpp` | Shared LZ4/Zstd compress+decompress dispatch; the only translation unit that includes the vendor codec headers |
| `src/PakBuilder.cpp` | `Pakker` build-time API: create, extract, list, validate (incl. deep content-hash verification), and modify PAK files |
| `src/PakReaderCore.cpp` | `PakReader` lifecycle, handle lookup, core read dispatch, range reads, enumeration, content-hash verification, and convenience wrappers |
| `src/PakReaderCache.cpp` | `PakReader` decoded-cache subsystem: O(1) memory LRU, persistent disk cache, cache-key generation, and source-byte verification |
| `src/PakMount.cpp` | `PakMount`: layered virtual filesystem composing multiple `PakReader` instances with override semantics |
| `src/PakLooseOverlay.h`, `src/PakLooseOverlay.cpp` | `PakLooseOverlay`: dev-only loose-file hot-reload override wrapping a `PakReader` |
| `src/PakPlatform.h` | Small platform abstraction for mmap, prefetch hints, and default cache directory discovery |
| `src/PakPlatform.cpp` | Windows/POSIX platform implementation and `PAK_NO_MMAP` fallback |
| `src/vendor/lz4.c`, `src/vendor/lz4.h` | Vendored LZ4 dependency used for per-file compression |
| `src/vendor/zstd/` | Vendored Zstd core (`common/`, `compress/`, `decompress/`, `zstd.h`, `zstd_errors.h`), mirroring upstream's `lib/` layout so its relative includes resolve. `huf_decompress_amd64.S` (optional x86_64 asm fast path) is deliberately not vendored -- `ZSTD_DISABLE_ASM` is set instead (see CMakeLists.txt) |
| `tests/PakRuntimeTests.cpp` | Assertion-based runtime regression tests registered through CTest |
| `benchmarks/PakRuntimeBenchmark.cpp` | Repeatable runtime benchmark for resolve, mapped view, copied reads, cache churn, and hot-cache reads |
| `example/main.cpp` | End-to-end demo using Pakker plus miniaudio |
| `CMakeLists.txt` | Static library, example, tests, benchmark, feature options, and install/export package |
| `cmake/PakkerConfig.cmake.in` | Template for the installed `find_package(Pakker)` config |
| `README.MD` | User-facing usage and API reference |

## Build And Test

Pakker uses CMake and builds a static library named `Pakker`.

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DBUILD_EXAMPLES=ON -DBUILD_BENCHMARKS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

To verify the streaming fallback path without memory mapping:

```bash
cmake -S . -B build_no_mmap -DPAK_NO_MMAP=ON -DBUILD_TESTING=ON -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARKS=OFF
cmake --build build_no_mmap --config Release
ctest --test-dir build_no_mmap -C Release --output-on-failure
```

Useful CMake options:

| Option | Default | Effect |
|--------|---------|--------|
| `BUILD_EXAMPLES` | `ON` | Builds `PakkerExample` |
| `BUILD_TESTING` | `ON` | Builds and registers `PakkerTests` |
| `BUILD_BENCHMARKS` | `ON` | Builds `PakkerBenchmark` |
| `PAK_NO_MMAP` | `OFF` | Disables mmap and forces ifstream reads |
| `PAK_WARNINGS_AS_ERRORS` | `OFF` | Adds `/WX` or `-Werror` to the library target |
| `PAK_ENABLE_LTO` | `OFF` | Enables interprocedural optimization for the library target |

Note: `tests/PakRuntimeTests.cpp` forces `assert()` to stay active regardless
of `NDEBUG` (see the top of that file). The suite uses `assert()` to both
exercise and verify behavior in the same expression (e.g.
`assert(reader.Open(path))`) -- without the `#undef NDEBUG` guard, a
Release build would silently skip every asserted call rather than just
skipping the check. Keep new test code inside this file so it inherits that
guard, or replicate it if you ever split tests into a new file.

## Architecture

Pakker has four public roles:

- `Pakker` is the build-time writer and archive utility API. It creates,
  extracts, lists, validates (structural and, optionally, deep content-hash),
  and modifies PAK files. It is not thread-safe.
- `PakReader` is the runtime read-only API. Open one archive, cache the file
  table, resolve asset paths into `PakFileHandle` values, and dispatch reads by
  handle. It is designed for concurrent asset loading.
- `PakMount` composes one or more already-open `PakReader` instances (owned
  via `shared_ptr`) into a single override-aware namespace -- base archive
  plus patch/DLC/language layers -- without introducing its own caching or
  I/O. It purely re-dispatches to the winning layer's `PakReader`.
- `PakLooseOverlay` is a dev-only wrapper around one `PakReader` plus a loose
  directory on disk, for hot-reload iteration. It is not part of the shipping
  read path -- see PakLooseOverlay below.

`Pakker`/`PakReader`/`PakMount` share compression through
`PakInternal::CompressBuffer`/`DecompressBuffer` (`src/PakCompression.h/.cpp`)
rather than calling LZ4/Zstd directly -- that keeps the two vendored codec
headers confined to one translation unit instead of leaking into
`PakInternal.h`, which is included much more widely.

The platform layer is intentionally narrow. `PakPlatform` owns file mapping,
unmapping, prefetch hints, and cache-directory discovery. Keep OS-specific code
there unless the public API truly needs to know about it.

## Archive Format

The current format is v8:

- Header magic is `PAK0`.
- Header version is `8`.
- The header stores file count, data alignment, the file-table offset, and the
  name-blob offset and size.
- The file table is an array of **fixed-size 48-byte `PakEntryRecord`s**:
  data offset, original size, compressed size, content hash, path hash, name
  offset, name length, flags, and chunk-size log2.
- All entry names live together in a single contiguous **name blob** that
  follows the record array. A record refers to its name by
  `(nameOffset, nameLength)` into that blob.
- Content hashes and path hashes are both XXH64. Content hashes cover the
  on-disk, post-compression/post-encryption bytes; path hashes cover the
  normalized name and are what `PakPathHash()` returns.
- File data is written before the file table.
- File data offsets are padded to `PakOptions::alignment`.
- Bit `0x01` in entry flags means the on-disk data is LZ4-compressed. Bit
  `0x02` means Zstd-compressed. The two are mutually exclusive; helper
  `PakInternal::IsCompressed(flags)` checks either. Bit `0x04` means the
  payload is chunked; helper `PakInternal::IsChunked(flags)` checks it.

### Why fixed-size records plus a name blob

v6 stored a length-prefixed name inline with each entry, which made the table
variable-stride: opening an archive meant reading it entry by entry and heap-
allocating a `std::string` per file, then a second copy inside a node-based
`unordered_map`. That cost about 218 resident bytes per entry and scaled
badly -- a 200k-entry archive took ~200 ms to open, and a 24-layer mount ran
to 28 seconds.

The v7 split let `PakReader::Open()` do exactly two bulk reads (records, then
names) and use the record array as its final runtime representation. Storing
the path hash in the record additionally means opening does no string hashing
at all, and that layered mounts can merge namespaces without re-hashing or
even loading names.

### Chunked entry payload

When flag bit `0x04` is set, the entry's payload is not one compression frame
over the whole entry. It is a `PakChunkHeader` (uncompressed bytes per block,
block count), then one `uint32` on-disk size per block, then each block's
bytes back to back.

Each block is compressed independently, so any one of them can be decoded
without touching the others. A block that does not compress is stored raw --
its recorded size equals its uncompressed size -- so incompressible data never
pays an expansion penalty.

This is what makes `ReadRange()` work on a compressed entry. Without it,
reaching any byte of a large compressed asset means decoding all of it: on a
64 MiB Zstd entry that is ~9.8 ms, against ~45 us chunked. The cost is ratio:
independent blocks give up cross-block matches, measured at roughly 4% larger
on mixed content, which is why `PakOptions::compressionChunkSize` defaults to
`0` (off) and is an explicit opt-in for streaming builds.

The block size is recorded twice on purpose: in `PakEntryRecord::chunkSizeLog2`
so `Open()` can report it without touching payloads, and in the payload header
where the reader cross-checks it. `ParseChunkTable()` validates the whole size
table against the payload before any decode, so a corrupt table cannot steer a
decode off the end of the mapping.

Format invariants:

- Only v8 archives are accepted. v7 (no chunked-entry flag), v6
  (variable-length file table), v5 (no Zstd flag bit) and v4 (no `contentHash`
  field) are all rejected with no dual-format read path and no in-place upgrade
  tool -- rebuild from source with the current library. This is the fifth hard
  version cutover in this project's history (v3->v4, v4->v5, v5->v6, v6->v7,
  v7->v8) -- keep following that precedent rather than introducing a
  dual-format reader unless there's a strong reason to break it.
- File names are normalized to forward slashes.
- Empty, invalid, too-long, duplicate-after-normalization, or traversal-like
  names must be rejected.
- Entry offsets and sizes must be validated against the archive size before
  runtime reads. This structural check stays O(1) per entry and does not read
  file content -- it must not become O(entry size), since it runs on every
  open/list/extract path. Content-hash verification is a separate, explicitly
  invoked operation (see Integrity Verification).
- Every entry's `(nameOffset, nameLength)` must lie inside the name blob. This
  is checked against `header.nameBlobSize` so it holds whether or not the blob
  was actually loaded.
- Path hashes must be unique within an archive. Two entries sharing one means
  either a duplicate path or a genuine 64-bit collision; either way the
  archive is unusable as written and `Open()` rejects it rather than making
  one of the two entries unreachable.
- A chunked entry's chunk table must exactly account for its payload: every
  block in bounds, none larger than its uncompressed size, and the blocks
  together covering the payload with nothing left over.
- Alignment must be a power of two. `0` is treated as `1`.

Any archive-format change must update `PakInternal::PakHeader`,
`PakInternal::PakEntryRecord`, `ReadPakHeader()`, `WritePakHeader()`,
file-table I/O, validation, tests, README, and this handbook.

## Concurrency Model

Two rules explain most of the runtime design.

**The read path never writes to shared memory.** `Open()` builds an immutable
`PakReader::ReadSnapshot` and publishes it with one release store;
`AcquireSnapshot()` is one acquire load. No lock, no reference count. This
matters because a `shared_lock` acquire/release pair and a `shared_ptr`
copy/destroy pair are atomic read-modify-writes, and an atomic RMW must take
the cache line exclusively -- so under a streaming workload every reader
invalidates every other reader's copy and aggregate throughput *falls* as
threads are added. It previously did: `Find()` managed 9.15 Mops/s across 32
threads against 13.96 on one. It is now 435 Mops/s.

The same pattern is applied to `PakMount::MergedIndex` and to
`PakReader`'s cache policy. Counters that must be written on the hot path are
sharded per thread (`PakInternal::ShardedCounter`) for the same reason.

The cost of this is a stricter lifetime contract: `Close()` unpublishes and
then frees, so it must not race with reads. That was already the documented
rule. `PakView` and `PakSpan` hold their own `shared_ptr` to the mapping, so
values already handed out stay valid after `Close()` regardless -- and that
reference count is the one atomic RMW deliberately left on the read path,
because there is no way to honour that guarantee without it.

**Anything that costs a lock is moved off the per-read path.** The decoded
memory cache is split into independently-locked shards, and its policy is read
through a published pointer rather than copied under a mutex. Before that, a
compressed read took the global cache mutex and heap-allocated a string copy
of the options struct, every single time.

When adding to the read path, the question to ask is not "is this lock held
briefly" but "does this write to a cache line another thread also touches."

## Build Pipeline

`CreatePak()` and `CreatePakFromFolder()` encode entries on a worker pool and
write them sequentially in input order. Two properties are load-bearing:

- **Output is byte-identical regardless of worker count.** Encoding is
  parallel, writing is not. `ParallelBuildMatchesSequentialByteForByte` and
  `ParallelFolderBuildMatchesSequential` assert this directly; keep them
  passing for any pipeline change.
- **Memory is bounded by batching.** Entries are processed in batches capped by
  both count (`kMaxBatchEntries`) and total source bytes (`kMaxBatchBytes`), so
  a handful of very large assets cannot balloon the working set. The folder
  builder collects file sizes during directory traversal specifically so it can
  apply the byte cap before reading anything.

The folder builder reads each file straight into its result buffer, so asset
bytes are never copied merely to hand them to the encoder.

## Runtime Read Path

`PakReader::Open()` is the setup boundary:

1. Open the archive and read the header.
2. Compute an archive fingerprint used by the decoded persistent cache.
3. Read and validate the file table.
4. Build an immutable runtime table:
   - `entries` for internal reads.
   - `infos` for public metadata.
   - `indexByName` for O(1) lookup.
5. Try to map the whole archive read-only.
6. If mapping fails, reopen the file for serialized ifstream reads.
7. Resolve the persistent cache directory.

`Find()` and `Resolve()` use the cached table and return cheap handles. Runtime
engine code should resolve paths once, store `PakFileHandle`, and avoid repeated
string lookups inside hot paths.

`View()` is the fastest read path. It returns a `PakView` directly into mapped
archive memory only when all of these are true:

- The archive is open and mapped.
- The file is uncompressed.
- The reader has no encryption key.
- The file bounds are valid.

`Read()` and `Load()` are the general read paths. They support compressed and
encrypted entries, validate destination sizes, decompress with LZ4 or Zstd
(dispatched by `PakInternal::DecompressBuffer` based on entry flags) when
needed, and use decoded caching when the entry is eligible.

`ReadRange()` is a partial-read path for large entries (video/audio) that
shouldn't be fully materialized just to read a slice. It operates in
decoded/original-offset space and covers three cases:

- **Uncompressed** entries, encrypted or not. XOR-with-repeating-key is
  range-safe: the key index is offset by `rangeOffset` rather than decrypting
  from the start of the entry.
- **Chunked compressed** entries, where only the blocks the range actually
  covers get decoded. With encryption in play the payload has to be
  materialized and decrypted first, because the chunk table sits at its head.
- **Whole-entry compressed** entries return `PakStatus::Unsupported`
  immediately, rather than decoding the whole entry to fake partial-read
  semantics. There is no index to seek into; rebuild with
  `PakOptions::compressionChunkSize` set if those entries need range reads.

`ListFiles()`/`ListFilesWithPrefix()` enumerate the cached file table, no
disk I/O -- the single-archive equivalent of `PakMount`'s enumeration.

`ReadFileZeroCopy()` is a convenience wrapper. It returns a mapped span for
eligible files and falls back to an owned buffer for compressed, encrypted, or
non-mapped reads.

## Integrity Verification

Every entry stores an XXH64 hash of its on-disk bytes (post-compression,
post-encryption), computed once at build time in `Pakker::CreatePak()`,
`CreatePakFromFolder()`, and `AddFileToPak()`. Verification is layered so the
default hot path pays nothing for it:

- `Pakker::ValidatePak(filename, deepVerify=false)` -- structural-only
  (`ValidateEntry()`), the existing fast default. `deepVerify=true`
  additionally re-hashes every entry's on-disk bytes -- O(archive size), meant
  for build/QA/patch-verification pipelines, not a hot path.
- `PakReader::VerifyEntry(handle)` -- re-hashes one entry's on-disk bytes via
  `HashEntrySourceBytes()` and compares against the stored hash. Off the hot path
  by design -- call it from a QA sweep or a "verify game files" flow, not from
  `Read()`/`Load()` call sites.
- `PakOpenOptions::verifyOnRead` -- opt-in, set at `Open()` time. When on,
  `ReadEntryToBuffer()` hashes the on-disk bytes it already has in hand
  (mapped pointer or freshly-read buffer) *before* decrypting/decompressing
  them, and fails closed with `PakStatus::HashMismatch` on a mismatch instead
  of handing bad bytes to LZ4 or the caller. This forces a full read+hash pass
  even on the mmap path, so it is not recommended for shipping hot paths.
- `View()` is **never** verified, under any option. Zero-copy is its whole
  contract; hashing on every call would defeat it the same way decoded
  caching deliberately skips `View()` (below). Callers needing an integrity
  guarantee on `View()`-eligible content should call `VerifyEntry()` once
  (e.g. at startup) instead of trusting `View()`'s pointer directly.

## Caching

Decoded caching applies to `Read()` and `Load()`, not to `View()`.

The default policy enables:

- In-memory decoded cache with O(1) LRU promotion and eviction.
- Persistent decoded cache when a writable cache directory is available.
- Source reads counted in `PakCacheStats`.
- Persistent cache invalidation using the archive fingerprint plus each entry's
  stored on-disk content hash; cache lookup does not re-hash source bytes.

Important constraints:

- Do not let `View()` return cache storage. It must mean mapped archive storage.
- Do not cache entries larger than `maxSingleEntryBytes`.
- Persistent cache paths must stay inside the resolved cache directory.
- Android and Web do not provide a default cache directory; callers should set
  `PakCacheOptions::persistentCacheDirectory` if they want persistent caching.
- `verifyOnRead` bypasses both decoded-cache tiers so every call hashes the
  current source bytes as promised; verified reads never load from or store to
  memory/persistent decoded caches.

## Thread Safety And Lifetimes

`PakReader` is thread-safe for concurrent reads.

- `Find()`, `Resolve()`, `Info()`, `InfoByIndex()`, `View()`, `Read()`,
  `Load()`, `ReadRange()`, `Prefetch()`, `VerifyEntry()`, `ListFiles()`,
  `ListFilesWithPrefix()`, and convenience wrappers may run concurrently.
- `Open()` and `Close()` take exclusive locks.
- The runtime table is immutable and shared with in-flight reads.
- `PakView` and `PakSpan` keep the mapped file alive through shared ownership,
  so a view/span can remain valid after `Close()`.
- The ifstream fallback serializes file I/O with `streamMutex_`.
- The decoded cache is protected by `cacheMutex_`; the source-read diagnostic
  counter is relaxed-atomic so uncached mapped reads do not serialize on it.

Do not move a `PakReader` while other threads are using it.

`PakMount` is thread-safe for concurrent reads, and composes with the above:

- `Find()`, `Resolve()`, `Info()`, `View()`, `Read()`, `Load()`, `Prefetch()`,
  `ListFiles()`, `ListFilesWithPrefix()`, and convenience wrappers may run
  concurrently with each other and with reads issued directly against a
  `shared_ptr<PakReader>` an engine also holds outside the mount (e.g. one
  obtained via `GetLayerReader()` or passed into `MountReader()`).
- `Mount()`, `MountReader()`, and `Clear()` take an exclusive lock on
  `PakMount`'s own mutex and rebuild the merged lookup index (a full rebuild,
  not incremental -- mounting is rare/load-screen-scale, so this amortizes
  fine). Do not call them concurrently with any other `PakMount` method on the
  same instance.
- `PakMount` never holds a layer's own `PakReader` lock while blocked on its
  own `mutex_`, and vice versa -- no cross-lock ordering hazard.
- `PakMountHandle` values remain valid as long as the layer they reference is
  still mounted; a handle resolved before `Clear()` degrades safely to
  `PakStatus::InvalidHandle`/`nullptr` afterward (bounds-checked against
  `layers_.size()`), it does not dereference a dangling layer.
- Do not move a `PakMount` while other threads are using it.

`PakLooseOverlay` has no mutable state after construction (the wrapped
`shared_ptr<PakReader>` and loose directory path are fixed for the object's
lifetime), so `FileExists()`/`Read()`/`Load()` are safe to call concurrently
from multiple threads, to the same extent the wrapped `PakReader`'s own
concurrent-read contract holds. Every call still does a filesystem stat --
this is a dev-only convenience, not a shipping hot path.

## Platform Behavior

Default builds use memory mapping:

- Windows uses `CreateFileA`, `CreateFileMappingA`, and `MapViewOfFile`.
- POSIX uses `open`, `fstat`, and `mmap`.
- `PAK_NO_MMAP` compiles mapping out and forces the streaming path.

Prefetch is advisory:

- Mapped Windows builds use `PrefetchVirtualMemory`.
- Linux and supported Android builds use `posix_fadvise` for file ranges.
- POSIX mapped builds use `madvise(MADV_WILLNEED)`.
- Unsupported platforms return success for file-range prefetch when no useful
  operation exists.

## Logging

The library is silent by default. All internal messages go through
`PakInternal::Log()`, which calls the global callback set by
`PakSetLogCallback()`.

Guidelines:

- Do not write directly to `stdout` or `stderr` from library code.
- Use error logs for operation failure, warning logs for recoverable fallback,
  and info logs for lifecycle events.
- The callback may be invoked from multiple threads, so callers own callback
  synchronization.

## Change Guidelines

Preserve the public split:

- Build-time archive authoring belongs in `Pakker`.
- Runtime read-only behavior belongs in `PakReader`.
- Multi-archive composition/override behavior belongs in `PakMount`, which
  should stay a pure consumer of `PakReader`'s public API -- avoid adding
  `PakMount`-specific state or friend access into `PakReader` beyond what's
  already there (`InfoByIndex()` for enumeration).
- Dev-only hot-reload override behavior belongs in `PakLooseOverlay`, which
  should stay a pure consumer of `PakReader`'s public API for the same
  reason `PakMount` does -- do not make `PakMount`'s layer type polymorphic
  to absorb loose-directory support instead of a standalone class.
- OS behavior belongs in `PakPlatform`.
- Compression behavior should stay isolated behind
  `PakInternal::CompressBuffer`/`DecompressBuffer` (`src/PakCompression.h/.cpp`)
  rather than calling the vendored LZ4/Zstd APIs directly from `PakBuilder.cpp`
  or `PakReaderCore.cpp`.

Keep the hot runtime path allocation-conscious:

- Prefer handle-based APIs in new runtime examples.
- Avoid path normalization unless the input actually needs it.
- Avoid holding the reader shared lock during decompression or I/O.
- Keep immutable state shareable with in-flight reads.

Keep compatibility explicit:

- If a change breaks old archives, make the version boundary obvious.
- If a change only affects v8 internals, add tests that prove existing v8
  behavior still works.
- If adding a new public API, update `src/Pak.h`, `README.MD`, examples or tests
  as appropriate.

Known limitations (deliberate, revisitable scope cuts, not oversights):

- `PakMount` v1 has no per-layer `Unmount()`, only `Clear()` (drop every
  layer). Layer identity is ambiguous for `MountReader()`-mounted layers that
  may carry no filename, and the dominant mount pattern -- mount everything
  once at a load-screen boundary -- doesn't need selective removal.
- There is no in-place archive upgrade tool between any format version.
  Rebuild from source with the current library.
- `PakReader::ReadRange()` supports uncompressed entries (encrypted or not)
  and chunked compressed entries. A compressed entry stored as one whole-entry
  frame has no index to seek into and returns `PakStatus::Unsupported`; that
  is a property of how the archive was built, not a missing feature -- set
  `PakOptions::compressionChunkSize` at pack time.
- `PakLooseOverlay` is dev-only: every lookup stats the filesystem, has no
  `View()`/zero-copy equivalent, and loose files are read raw (no
  compression or encryption). Not intended for a shipping hot path.

## Verification Checklist

For normal implementation changes:

```bash
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

For changes touching mmap, platform code, `Read()`, `View()`, cache behavior, or
thread-safety assumptions, also run the no-mmap configuration:

```bash
cmake --build build_no_mmap --config Release
ctest --test-dir build_no_mmap -C Release --output-on-failure
```

For format or writer changes, verify at least:

- Creating an archive with compression off.
- Creating an archive with LZ4 compression on.
- Creating an archive with Zstd compression on.
- An archive containing both LZ4- and Zstd-compressed entries (e.g. via
  `AddFileToPak` with a different method than the archive's `CreatePak` used).
- Empty archive handling.
- Invalid archive rejection.
- Extraction and validation.
- Path normalization and duplicate detection.

For cache changes, verify at least:

- Memory cache hit after repeated read.
- Persistent cache hit after reader reopen.
- Persistent cache invalidation after source archive changes.
- `View()` still bypasses decoded cache.

For platform changes, verify:

- Mapped path when available.
- Streaming fallback with `PAK_NO_MMAP=ON`.
- Zero-length file entries.
- `Prefetch()` on mapped and non-mapped readers.

For integrity-hash changes, verify at least:

- Creating an archive and confirming `ValidatePak(deepVerify=true)` passes.
- Corrupting a single byte in an LZ4-compressed entry, a Zstd-compressed
  entry, and an uncompressed entry, and confirming both
  `ValidatePak(deepVerify=true)` and `VerifyEntry()` catch it in each case,
  while `ValidatePak(deepVerify=false)` does not.
- `PakOpenOptions::verifyOnRead=true` fails closed with
  `PakStatus::HashMismatch`; `verifyOnRead=false` (default) still reads
  corrupted bytes through unchanged.
- `View()` remains unverified by design, under any option.
- Old v5 (no Zstd flag bit) and v4 (no `contentHash` field) archives are both
  rejected by `Open()` and `ValidatePak()`.

For `PakMount` changes, verify at least:

- A file present in two layers resolves to the higher-priority (later
  mounted) layer's content.
- A file present only in a lower layer still resolves.
- `MountReader()` rejects a reader that isn't open, and sharing an
  externally-owned reader between direct access and a mount sees identical
  data through both paths.
- `Clear()` drops every layer; a `PakMountHandle` resolved before `Clear()`
  degrades to `InvalidHandle`/`nullptr` rather than crashing.
- `ListFiles()`/`ListFilesWithPrefix()` deduplicate across layers with the
  highest-priority layer's content winning.
- Concurrent reads across layers from multiple threads.

For `PakReader::ReadRange()` changes, verify at least:

- A partial read of an uncompressed entry matches the corresponding slice of
  a full `Load()`.
- A compressed entry (LZ4 or Zstd) returns `PakStatus::Unsupported`.
- An encrypted-but-uncompressed entry's range read matches the corresponding
  slice of a full decrypted `Load()`, including a range that doesn't start
  at a key-length-aligned offset.
- Out-of-bounds ranges (offset beyond `originalSize`, or offset+length
  overflowing it) are rejected.
- Both the mapped and `PAK_NO_MMAP` streaming paths.

For `PakLooseOverlay` changes, verify at least:

- A file present in the loose directory is served over the archive's copy.
- A file absent from the loose directory falls back to the wrapped reader.
- `FileExists()` checks both the loose directory and the wrapped reader.
- A path-traversal filename (e.g. `../secret.bin`) cannot escape the loose
  directory, even when a real file exists at the resolved location.

## Common Workflows

### Add A Runtime API

1. Add the public declaration to `src/Pak.h`.
2. Implement in `src/PakReaderCore.cpp` (or `src/PakReaderCache.cpp` for
   cache-related runtime APIs).
3. Decide whether it is handle-based, path-based, or both.
4. Preserve thread-safety by capturing immutable read context before I/O.
5. Add or extend tests in `tests/PakRuntimeTests.cpp`.
6. Update README if the API is public and user-facing.

### Change The Archive Writer

1. Start from `Pakker::CreatePak()`.
2. Preserve filename normalization and validation.
3. Preserve power-of-two alignment.
4. Keep compression optional and per-file.
5. Re-run runtime tests because writer bugs usually surface at reader open/read.

### Change Compression Support

1. Start from `PakInternal::CompressBuffer()`/`DecompressBuffer()`
   (`src/PakCompression.h/.cpp`) -- the single dispatch point both
   `PakBuilder.cpp` and `PakReaderCore.cpp` call into. Do not call a vendored
   codec API directly from either of those files.
2. A new codec needs its own `PAK_FLAG_*_COMPRESSED` bit (`src/Pak.h`) and a
   branch in both functions. Flags are mutually exclusive; `IsCompressed()`
   already checks the union of all compression bits, so new callers using it
   don't need updating.
3. If the on-disk semantics of `PakEntry::flags` change in a way an older
   reader would silently misinterpret (rather than cleanly reject), that's an
   archive-format change -- see Archive Format's version-cutover precedent,
   don't rely on flag bits alone to signal a reader-compatibility boundary.
4. Keep the codec's vendored sources isolated (their own `src/vendor/<name>/`
   directory) and confined to `PakCompression.cpp`'s includes.
5. Add tests covering: round-trip, corruption caught by `deepVerify`/
   `VerifyEntry()`, and an archive mixing the new codec with existing ones.

### Change Decoded Cache Behavior

1. Start from `ReadEntryWithCache()`, `ShouldCacheDecoded()`, and cache key
   generation.
2. Keep the memory cache key cheap.
3. Keep the persistent key resistant to stale archive contents.
4. Update cache statistics consistently.
5. Add tests for both memory and persistent cache behavior.

### Change Platform I/O

1. Keep public API changes out of `PakPlatform` unless necessary.
2. Preserve `PAK_NO_MMAP`.
3. Make unsupported prefetch behavior advisory rather than fatal where possible.
4. Verify both mapped and streaming readers.

### Change Integrity Verification

1. Start from `ReadEntryToBuffer()` (on-read-path `verifyOnRead` check),
   `VerifyEntry()`, `HashEntrySourceBytes()`, and `Pakker::ValidatePak()`'s
   `deepVerify` loop. Persistent cache keys reuse the stored `contentHash`.
2. Keep hashing on-disk bytes (post-compression, post-encryption), not
   logical/decoded content -- this lets verification run without decrypting
   or decompressing first, and keeps `contentHash` consistent with what
   `HashEntrySourceBytes()` computes when verification is explicitly requested.
3. Never let a hash mismatch reach the decoded cache; check before storing.
4. `View()` stays unverified, always -- do not add a verification path to it.
5. Add tests for both the compressed and uncompressed entry paths, since they
   hash from different buffers (mapped/scratch vs. destination) before
   different mutation points (decrypt, then decompress).

### Change PakMount Layering

1. Start from `RebuildMergedIndexLocked()` for merge-order/override semantics
   and `PakMountHandle` for the layer+handle dispatch shape.
2. Keep the merge a full rebuild on `Mount()`/`MountReader()`/`Clear()` --
   incremental merge/unmerge is more complex for negligible benefit given how
   rarely mounting happens relative to `Find()` calls.
3. Keep `PakMount` a pure `PakReader` consumer: dispatch by snapshotting a
   layer's `shared_ptr<PakReader>` under `PakMount`'s own lock, then release
   before calling into it -- never hold both locks at once.
4. Do not add a second decoded-cache layer; per-layer caching is already
   available via `GetLayerReader(index)->SetCacheOptions(...)`.
5. If adding per-layer `Unmount()`, decide layer identity for
   `MountReader()`-mounted layers (which may carry no filename) before
   settling on an API shape.

### Change PakLooseOverlay Behavior

1. Start from `PakLooseOverlay::ResolveLooseFilePath()` -- the single
   traversal-guarded path-resolution point every method routes through.
2. Keep the filename-keyed API (no handles): the whole point of this class is
   that the loose/not-loose answer can change between calls as a file is
   saved, so a pre-resolved handle would just cache a stale answer.
3. Do not add a `View()`/zero-copy equivalent; a loose file read from disk
   has no mapping to back that contract. If a use case truly needs it, mmap
   the individual loose file rather than faking `PakView` over a heap buffer.
4. Keep it a pure `PakReader` consumer, same rule as `PakMount` -- do not make
   `PakReader` aware of loose overrides.
5. Add tests for: loose-file precedence, fallback to the wrapped reader, and
   path traversal rejection with a real file present at the resolved
   out-of-bounds location (a missing file at that path wouldn't distinguish
   "correctly rejected" from "just didn't happen to exist").

## Release Checklist

- Build Release with default options.
- Run CTest for default and `PAK_NO_MMAP` configurations.
- Run the benchmark if the change could affect hot-path read performance.
- Check README snippets against the current API.
- Check that `README.MD`, `HANDBOOK.md`, and `src/Pak.h` agree on thread-safety
  and cache semantics.
- Confirm `cmake --install` followed by `find_package(Pakker CONFIG)` from a
  separate consumer project still resolves and links `Pakker::Pakker`.
- Confirm no generated build files or temporary archives are staged.
