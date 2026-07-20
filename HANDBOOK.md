# Pakker Handbook

This handbook is for contributors working on Pakker itself. The README explains
how to use the library; this file explains how the project is put together, what
runtime guarantees matter, and what to verify when changing it.

## Project Goal

Pakker is a small C++20 asset archive library aimed at game-engine runtime use.
Its main job is to build `.pak` archives offline, then serve assets at runtime
with predictable lookup cost, memory-mapped I/O when available, zero-copy views
for eligible files, and thread-safe concurrent reads.

The current archive format is v4-only.

## Repository Map

| Path | Purpose |
|------|---------|
| `src/Pak.h` | Public API, archive structures, runtime handles, cache options, and thread-safety contract |
| `src/PakInternal.h` | Private implementation header: LZ4 size limit and FNV fingerprint/hash helpers shared across the files below |
| `src/PakCommon.cpp` | Shared archive-format contract: logging, path/filename validation, header and file-table I/O, encryption |
| `src/PakBuilder.cpp` | `Pakker` build-time API: create, extract, list, validate, and modify PAK files |
| `src/PakReaderCore.cpp` | `PakReader` lifecycle, handle lookup, core read dispatch, and convenience wrappers |
| `src/PakReaderCache.cpp` | `PakReader` decoded-cache subsystem: memory LRU, persistent disk cache, cache-key generation |
| `src/PakPlatform.h` | Small platform abstraction for mmap, prefetch hints, and default cache directory discovery |
| `src/PakPlatform.cpp` | Windows/POSIX platform implementation and `PAK_NO_MMAP` fallback |
| `src/vendor/lz4.c`, `src/vendor/lz4.h` | Vendored LZ4 dependency used for per-file compression |
| `tests/PakRuntimeTests.cpp` | Assertion-based runtime regression tests registered through CTest |
| `benchmarks/PakRuntimeBenchmark.cpp` | Simple runtime benchmark for resolve, mapped view, and copied reads |
| `example/main.cpp` | End-to-end demo using Pakker plus miniaudio |
| `CMakeLists.txt` | Static library, example, tests, benchmark, and feature options |
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

## Architecture

Pakker has two public roles:

- `Pakker` is the build-time writer and archive utility API. It creates,
  extracts, lists, validates, and modifies PAK files. It is not thread-safe.
- `PakReader` is the runtime read-only API. Open one archive, cache the file
  table, resolve asset paths into `PakFileHandle` values, and dispatch reads by
  handle. It is designed for concurrent asset loading.

The platform layer is intentionally narrow. `PakPlatform` owns file mapping,
unmapping, prefetch hints, and cache-directory discovery. Keep OS-specific code
there unless the public API truly needs to know about it.

## Archive Format

The current format is v4:

- Header magic is `PAK0`.
- Header version is `4`.
- The header stores file count, file-table offset, data alignment, and reserved
  fields.
- Each file-table entry stores UTF-8 path, data offset, original size,
  compressed size, and flags.
- File data is written before the file table.
- File data offsets are padded to `PakOptions::alignment`.
- Bit `0x01` in entry flags means the on-disk data is LZ4-compressed.

Format invariants:

- Only v4 archives are accepted.
- File names are normalized to forward slashes.
- Empty, invalid, too-long, duplicate-after-normalization, or traversal-like
  names must be rejected.
- Entry offsets and sizes must be validated against the archive size before
  runtime reads.
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

## Thread Safety And Lifetimes

`PakReader` is thread-safe for concurrent reads.

- `Find()`, `Resolve()`, `Info()`, `View()`, `Read()`, `Load()`, `Prefetch()`,
  and convenience wrappers may run concurrently.
- `Open()` and `Close()` take exclusive locks.
- The runtime table is immutable and shared with in-flight reads.
- `PakView` and `PakSpan` keep the mapped file alive through shared ownership,
  so a view/span can remain valid after `Close()`.
- The ifstream fallback serializes file I/O with `streamMutex_`.
- The decoded cache is protected by `cacheMutex_`.

Do not move a `PakReader` while other threads are using it.

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
- OS behavior belongs in `PakPlatform`.
- Compression behavior should stay isolated behind the vendored LZ4 calls.

Keep the hot runtime path allocation-conscious:

- Prefer handle-based APIs in new runtime examples.
- Avoid path normalization unless the input actually needs it.
- Avoid holding the reader shared lock during decompression or I/O.
- Keep immutable state shareable with in-flight reads.

Keep compatibility explicit:

- If a change breaks old archives, make the version boundary obvious.
- If a change only affects v4 internals, add tests that prove old v4 behavior
  still works.
- If adding a new public API, update `src/Pak.h`, `README.MD`, examples or tests
  as appropriate.

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

## Release Checklist

- Build Release with default options.
- Run CTest for default and `PAK_NO_MMAP` configurations.
- Run the benchmark if the change could affect hot-path read performance.
- Check README snippets against the current API.
- Check that `README.MD`, `HANDBOOK.md`, and `src/Pak.h` agree on thread-safety
  and cache semantics.
- Confirm no generated build files or temporary archives are staged.
