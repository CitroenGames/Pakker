# Pakker Handbook

This handbook is for contributors working on Pakker itself. The README explains
how to use the library; this file explains how the project is put together, what
runtime guarantees matter, and what to verify when changing it.

## Project Goal

Pakker is a small C++20 asset archive library aimed at game-engine runtime use.
Its main job is to build `.pak` archives offline, then serve assets at runtime
with predictable lookup cost, memory-mapped I/O when available, zero-copy views
for eligible files, and thread-safe concurrent reads.

The current archive format is v5-only.

## Repository Map

| Path | Purpose |
|------|---------|
| `src/Pak.h` | Public API, archive structures, runtime handles, cache options, `PakMount`, and thread-safety contract |
| `src/PakInternal.h` | Private implementation header: LZ4 size limit and FNV fingerprint/hash helpers shared across the files below |
| `src/PakCommon.cpp` | Shared archive-format contract: logging, path/filename validation, header and file-table I/O, encryption |
| `src/PakBuilder.cpp` | `Pakker` build-time API: create, extract, list, validate (incl. deep content-hash verification), and modify PAK files |
| `src/PakReaderCore.cpp` | `PakReader` lifecycle, handle lookup, core read dispatch, content-hash verification, and convenience wrappers |
| `src/PakReaderCache.cpp` | `PakReader` decoded-cache subsystem: memory LRU, persistent disk cache, cache-key generation, source-byte hashing |
| `src/PakMount.cpp` | `PakMount`: layered virtual filesystem composing multiple `PakReader` instances with override semantics |
| `src/PakPlatform.h` | Small platform abstraction for mmap, prefetch hints, and default cache directory discovery |
| `src/PakPlatform.cpp` | Windows/POSIX platform implementation and `PAK_NO_MMAP` fallback |
| `src/vendor/lz4.c`, `src/vendor/lz4.h` | Vendored LZ4 dependency used for per-file compression |
| `tests/PakRuntimeTests.cpp` | Assertion-based runtime regression tests registered through CTest |
| `benchmarks/PakRuntimeBenchmark.cpp` | Simple runtime benchmark for resolve, mapped view, and copied reads |
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

Pakker has three public roles:

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

The platform layer is intentionally narrow. `PakPlatform` owns file mapping,
unmapping, prefetch hints, and cache-directory discovery. Keep OS-specific code
there unless the public API truly needs to know about it.

## Archive Format

The current format is v5:

- Header magic is `PAK0`.
- Header version is `5`.
- The header stores file count, file-table offset, data alignment, and reserved
  fields.
- Each file-table entry stores UTF-8 path, data offset, original size,
  compressed size, flags, and a content-integrity hash (FNV-1a-64 of the
  on-disk, post-compression/post-encryption bytes).
- File data is written before the file table.
- File data offsets are padded to `PakOptions::alignment`.
- Bit `0x01` in entry flags means the on-disk data is LZ4-compressed.

Format invariants:

- Only v5 archives are accepted. v4 (no `contentHash` field) is rejected with
  no dual-format read path and no in-place upgrade tool -- rebuild from
  source with the current library.
- File names are normalized to forward slashes.
- Empty, invalid, too-long, duplicate-after-normalization, or traversal-like
  names must be rejected.
- Entry offsets and sizes must be validated against the archive size before
  runtime reads. This structural check (`ValidateEntry()`) stays O(1) per
  entry and does not read file content -- it must not become O(entry size),
  since it runs on every open/list/extract path. Content-hash verification is
  a separate, explicitly-invoked operation (see Integrity Verification).
- Alignment must be a power of two. `0` is treated as `1`.

Any archive-format change must update `PakInternal::PakHeader`,
`ReadPakHeader()`, `WritePakHeader()`, file-table I/O, validation, tests,
README, and this handbook.

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
encrypted entries, validate destination sizes, decompress with LZ4 when needed,
and use decoded caching when the entry is eligible.

`ReadFileZeroCopy()` is a convenience wrapper. It returns a mapped span for
eligible files and falls back to an owned buffer for compressed, encrypted, or
non-mapped reads.

## Integrity Verification

Every entry stores an FNV-1a-64 hash of its on-disk bytes (post-compression,
post-encryption), computed once at build time in `Pakker::CreatePak()`,
`CreatePakFromFolder()`, and `AddFileToPak()`. Verification is layered so the
default hot path pays nothing for it:

- `Pakker::ValidatePak(filename, deepVerify=false)` -- structural-only
  (`ValidateEntry()`), the existing fast default. `deepVerify=true`
  additionally re-hashes every entry's on-disk bytes -- O(archive size), meant
  for build/QA/patch-verification pipelines, not a hot path.
- `PakReader::VerifyEntry(handle)` -- re-hashes one entry's on-disk bytes via
  `HashEntrySourceBytes()` (the same helper the persistent cache uses for its
  own key derivation) and compares against the stored hash. Off the hot path
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

- In-memory decoded cache with an LRU budget.
- Persistent decoded cache when a writable cache directory is available.
- Source reads counted in `PakCacheStats`.
- Persistent cache invalidation using archive fingerprint plus source-byte hash.

Important constraints:

- Do not let `View()` return cache storage. It must mean mapped archive storage.
- Do not cache entries larger than `maxSingleEntryBytes`.
- Persistent cache paths must stay inside the resolved cache directory.
- Android and Web do not provide a default cache directory; callers should set
  `PakCacheOptions::persistentCacheDirectory` if they want persistent caching.
- Content-hash verification (when `verifyOnRead` is enabled) happens in
  `ReadEntryToBuffer()` before a decoded result is ever handed to
  `StoreMemoryCache()`/`StorePersistentCache()` in `ReadEntryWithCache()` -- a
  hash mismatch is never cached in either tier.

## Thread Safety And Lifetimes

`PakReader` is thread-safe for concurrent reads.

- `Find()`, `Resolve()`, `Info()`, `InfoByIndex()`, `View()`, `Read()`,
  `Load()`, `Prefetch()`, `VerifyEntry()`, and convenience wrappers may run
  concurrently.
- `Open()` and `Close()` take exclusive locks.
- The runtime table is immutable and shared with in-flight reads.
- `PakView` and `PakSpan` keep the mapped file alive through shared ownership,
  so a view/span can remain valid after `Close()`.
- The ifstream fallback serializes file I/O with `streamMutex_`.
- The decoded cache is protected by `cacheMutex_`.

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
- OS behavior belongs in `PakPlatform`.
- Compression behavior should stay isolated behind the vendored LZ4 calls.

Keep the hot runtime path allocation-conscious:

- Prefer handle-based APIs in new runtime examples.
- Avoid path normalization unless the input actually needs it.
- Avoid holding the reader shared lock during decompression or I/O.
- Keep immutable state shareable with in-flight reads.

Keep compatibility explicit:

- If a change breaks old archives, make the version boundary obvious.
- If a change only affects v5 internals, add tests that prove old v5 behavior
  still works.
- If adding a new public API, update `src/Pak.h`, `README.MD`, examples or tests
  as appropriate.

Known limitations (deliberate, revisitable scope cuts, not oversights):

- `PakMount` v1 has no per-layer `Unmount()`, only `Clear()` (drop every
  layer). Layer identity is ambiguous for `MountReader()`-mounted layers that
  may carry no filename, and the dominant mount pattern -- mount everything
  once at a load-screen boundary -- doesn't need selective removal.
- There is no in-place v4-to-v5 archive upgrade tool. Rebuild from source.

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
- Creating an archive with compression on.
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
- Corrupting a single byte in a compressed entry and an uncompressed entry,
  and confirming both `ValidatePak(deepVerify=true)` and `VerifyEntry()`
  catch it, while `ValidatePak(deepVerify=false)` does not.
- `PakOpenOptions::verifyOnRead=true` fails closed with
  `PakStatus::HashMismatch`; `verifyOnRead=false` (default) still reads
  corrupted bytes through unchanged.
- `View()` remains unverified by design, under any option.
- Old v4 archives (no `contentHash` field) are rejected by `Open()` and
  `ValidatePak()`.

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
   `VerifyEntry()`, `HashEntrySourceBytes()` (shared hashing helper, also used
   for persistent cache keys), and `Pakker::ValidatePak()`'s `deepVerify` loop.
2. Keep hashing on-disk bytes (post-compression, post-encryption), not
   logical/decoded content -- this lets verification run without decrypting
   or decompressing first, and keeps `contentHash` consistent with what
   `HashEntrySourceBytes()` already computes for cache-key purposes.
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
