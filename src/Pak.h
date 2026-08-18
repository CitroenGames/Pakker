#ifndef PAK_H
#define PAK_H

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <list>
#include <memory>
#include <cstdint>
#include <string_view>
#include <fstream>
#include <functional>
#include <shared_mutex>
#include <mutex>
#include <atomic>
#include <span>

#include "PakPlatform.h"

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

enum class PakLogLevel { Info, Warning, Error };
using PakLogCallback = void(*)(PakLogLevel level, const char* message);

// Set a global log callback. When null (default), all logging is suppressed.
// Thread-safe: may be called from any thread at any time. The callback itself
// must be thread-safe if it can be invoked from multiple threads concurrently.
void PakSetLogCallback(PakLogCallback cb);

// ---------------------------------------------------------------------------
// Shared types & constants
// ---------------------------------------------------------------------------

namespace PakInternal {

static constexpr size_t MAX_FILENAME_LENGTH = 65535;
// A single shipping archive can hold a whole game's worth of assets, so this
// ceiling exists to reject absurd headers rather than to express a design
// limit. Handles are uint32, and the name blob is separately bounded below.
static constexpr size_t MAX_FILES_IN_PAK    = 8000000;
static constexpr uint64_t MAX_NAME_BLOB_SIZE = 1ull << 30; // 1 GiB
static constexpr std::string_view PAK_MAGIC = "PAK0";
static constexpr uint32_t PAK_VERSION_4     = 4; // rejected legacy format, no contentHash field
static constexpr uint32_t PAK_VERSION_5     = 5; // rejected legacy format, no Zstd flag bit
static constexpr uint32_t PAK_VERSION_6     = 6; // rejected legacy format, variable-length file table
static constexpr uint32_t PAK_VERSION_7     = 7; // v7-only format: fixed-size records + name blob

#pragma pack(push, 1)
struct PakHeader {
    char magic[4] = { 'P', 'A', 'K', '0' };
    uint32_t version   = PAK_VERSION_7;
    uint32_t numFiles  = 0;
    uint32_t alignment = 0;         // data alignment in bytes (power of 2)
    uint64_t fileTableOffset = 0;   // start of the fixed-size entry records
    uint64_t nameBlobOffset  = 0;   // start of the packed name bytes
    uint64_t nameBlobSize    = 0;
    uint64_t reserved[2] = {0, 0};
};
#pragma pack(pop)
static_assert(sizeof(PakHeader) == 56, "PakHeader must be 56 bytes with no padding");

// One file-table entry exactly as it appears on disk.
//
// Fixed size is the whole point. v6 stored a length-prefixed name inline with
// each entry, so opening an archive meant parsing entries one at a time and
// heap-allocating a std::string per file. A fixed-size record lets the entire
// table be read in one bulk I/O and consumed as a flat array, and lets the
// names travel together in a single contiguous blob.
#pragma pack(push, 1)
struct PakEntryRecord {
    uint64_t offset         = 0;
    uint64_t originalSize   = 0;
    uint64_t compressedSize = 0; // == originalSize when uncompressed
    uint64_t contentHash    = 0; // XXH64 of the on-disk (compressed+encrypted) bytes
    uint64_t pathHash       = 0; // XXH64 of the normalized name
    uint32_t nameOffset     = 0; // byte offset into the name blob
    uint16_t nameLength     = 0;
    uint8_t  flags          = 0; // bit 0: LZ4 compressed, bit 1: Zstd compressed
    uint8_t  reserved       = 0;
};
#pragma pack(pop)
static_assert(sizeof(PakEntryRecord) == 48, "PakEntryRecord must be 48 bytes with no padding");

// Build-time entry. Carries the name by value because the writer assembles
// entries before it knows the final blob layout; the reader never uses this.
struct PakEntry {
    std::string filename;
    uint64_t offset       = 0;
    uint64_t originalSize = 0;
    uint64_t compressedSize = 0; // == originalSize when uncompressed
    uint8_t  flags        = 0;   // bit 0: LZ4 compressed, bit 1: Zstd compressed (mutually exclusive)
    uint64_t contentHash   = 0;  // XXH64 of the on-disk (compressed+encrypted) bytes
    uint64_t pathHash      = 0;  // XXH64 of the normalized name

    PakEntry() = default;
    PakEntry(std::string name, uint64_t off, uint64_t origSz,
             uint64_t compSz = 0, uint8_t f = 0, uint64_t hash = 0, uint64_t pHash = 0)
        : filename(std::move(name)), offset(off), originalSize(origSz),
          compressedSize(compSz == 0 ? origSz : compSz), flags(f), contentHash(hash),
          pathHash(pHash) {}
};

// One slot of the reader's open-addressing name index, built at Open().
//
// Only a 32-bit tag of the path hash lives here; a tag match is confirmed
// against the full 64-bit pathHash in the entry record. That keeps the table
// at 8 bytes per slot instead of 16, which halves both its footprint and the
// number of cache lines a probe has to touch.
struct LookupSlot {
    uint32_t hashTag    = 0;
    uint32_t entryIndex = 0xFFFFFFFFu; // 0xFFFFFFFF == empty
};
static_assert(sizeof(LookupSlot) == 8, "LookupSlot must stay 8 bytes");

static constexpr uint8_t PAK_FLAG_LZ4_COMPRESSED  = 0x01;
static constexpr uint8_t PAK_FLAG_ZSTD_COMPRESSED = 0x02;

inline bool IsCompressed(uint8_t flags)
{
    return (flags & (PAK_FLAG_LZ4_COMPRESSED | PAK_FLAG_ZSTD_COMPRESSED)) != 0;
}

// Internal helpers shared by Pakker and PakReader
void Log(PakLogLevel level, const std::string& msg);

std::string NormalizePathSeparators(const std::string& path);
bool IsValidFilename(const std::string& filename);
uint64_t SafeStreamPos(std::ios& stream, std::streampos pos);
bool ValidateEntry(const PakEntry& entry, uint64_t pakFileSize);

bool ReadPakHeader(std::istream& stream, PakHeader& header);
bool WritePakHeader(std::ostream& stream, const PakHeader& header);

// Reads the v7 file table (records followed by the name blob) and expands it
// into name-carrying PakEntry values. Convenience for build-time and tooling
// paths; PakReader::Open deliberately does not use this, because materializing
// a std::string per entry is exactly the cost it exists to avoid.
bool ReadFileTable(std::istream& stream, const PakHeader& header,
                   std::vector<PakEntry>& entries);

// Writes the record array followed by the name blob, filling in
// header.nameBlobOffset/nameBlobSize. `tableOffset` is where the records
// begin, so the name offsets can be computed before anything is written.
bool WriteFileTable(std::ostream& stream, const std::vector<PakEntry>& entries,
                    PakHeader& header);

void EncryptDecrypt(std::vector<uint8_t>& data, const std::string& key);

// RAII guard for memory-mapped file. Shared between PakReader, PakView, and
// PakSpan instances so the mapping stays alive until the last reference is
// released.
struct MappedFileGuard {
    PakPlatform::MappedFile mf{};
    ~MappedFileGuard() { PakPlatform::UnmapFile(mf); }
    MappedFileGuard() = default;
    MappedFileGuard(const MappedFileGuard&) = delete;
    MappedFileGuard& operator=(const MappedFileGuard&) = delete;
};

// Stable per-thread index used to shard hot counters and cache buckets so
// independent threads touch independent cache lines. Indices are handed out
// on first use and never reused; callers must take it modulo their shard
// count.
size_t ThreadShardIndex() noexcept;

// XXH64 over an arbitrary buffer. Roughly an order of magnitude faster than
// the byte-at-a-time FNV-1a used for structural fingerprints, which matters
// wherever whole archives get hashed.
uint64_t HashBytesFast(const void* data, size_t size) noexcept;

// A counter incremented on the hot read path from many threads at once.
//
// A single std::atomic<uint64_t> looks cheap, but fetch_add is a
// read-modify-write: it must acquire the cache line exclusively, so N
// threads incrementing one counter serialize on one line. Spreading the
// increments across per-thread, cache-line-aligned slots keeps them
// core-local. The trade is that the value is only correct when summed, which
// is fine for a diagnostic read out by GetCacheStats().
class ShardedCounter {
public:
    static constexpr size_t Shards = 64;

    void Increment() noexcept
    {
        shards_[ThreadShardIndex() % Shards].value.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t Load() const noexcept
    {
        uint64_t total = 0;
        for (const auto& shard : shards_) {
            total += shard.value.load(std::memory_order_relaxed);
        }
        return total;
    }

    void Reset() noexcept
    {
        for (auto& shard : shards_) shard.value.store(0, std::memory_order_relaxed);
    }

    // Collapses an existing total into shard 0. Used when moving a reader.
    void Store(uint64_t value) noexcept
    {
        Reset();
        shards_[0].value.store(value, std::memory_order_relaxed);
    }

private:
    // Padding to a cache line is the entire point, so MSVC's "structure was
    // padded due to alignment specifier" diagnostic is noise here.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct alignas(64) Shard {
        std::atomic<uint64_t> value{0};
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    Shard shards_[Shards];
};

struct TransparentStringHash {
    using is_transparent = void;

    size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
    size_t operator()(const std::string& value) const noexcept {
        return (*this)(std::string_view(value));
    }
    size_t operator()(const char* value) const noexcept {
        return (*this)(std::string_view(value));
    }
};

struct TransparentStringEqual {
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
        return lhs == rhs;
    }
};

} // namespace PakInternal

// ---------------------------------------------------------------------------
// Runtime status and handle types
// ---------------------------------------------------------------------------

enum class PakStatus {
    Ok,
    NotOpen,
    NotFound,
    InvalidHandle,
    InvalidArgument,
    BufferTooSmall,
    Unsupported,
    CorruptArchive,
    IoError,
    DecompressionFailed,
    HashMismatch,       // on-disk bytes do not match the entry's stored content hash
};

const char* PakStatusToString(PakStatus status);

// Stable 64-bit hash of an asset path (XXH64). Lets an engine carry
// precomputed asset IDs instead of path strings, and is what the layered
// mount index is keyed on.
//
// The input must already be normalized -- forward slashes, no leading slash
// -- exactly as stored in the archive. Hashing is byte-exact and therefore
// case-sensitive, matching lookup behaviour elsewhere in the library.
uint64_t PakPathHash(std::string_view path) noexcept;

struct PakFileHandle {
    static constexpr uint32_t InvalidIndex = UINT32_MAX;

    uint32_t index = InvalidIndex;

    explicit operator bool() const { return index != InvalidIndex; }
    friend bool operator==(PakFileHandle a, PakFileHandle b) { return a.index == b.index; }
    friend bool operator!=(PakFileHandle a, PakFileHandle b) { return !(a == b); }
};

// The reader's runtime entry, and what Info() hands back.
//
// One array of these is the whole file table at runtime: there is no second
// parallel array of "real" entries behind it. filename points into the
// reader's contiguous name blob and is empty when the archive was opened with
// PakOpenOptions::loadNames disabled.
struct PakFileInfo {
    std::string_view filename;
    uint64_t originalSize = 0;
    uint64_t compressedSize = 0;
    uint64_t offset = 0;
    uint64_t pathHash = 0;     // XXH64 of the normalized name; see PakPathHash
    uint64_t contentHash = 0;  // XXH64 of the on-disk bytes
    uint8_t  flags = 0;
    bool compressed = false;
};

struct PakView {
    const uint8_t* data = nullptr;
    uint64_t size = 0;
    bool mapped = false;

private:
    friend class PakReader;
    std::shared_ptr<PakInternal::MappedFileGuard> mappingRef_;
};

// ---------------------------------------------------------------------------
// PakCompression -- per-file compression method selection
// ---------------------------------------------------------------------------

enum class PakCompression {
    None,
    LZ4,   // fast decode, ~2:1 ratio -- prefer for latency-sensitive hot-reload style reads
    Zstd,  // better ratio at similar decode cost -- prefer for install/patch size
};

// ---------------------------------------------------------------------------
// PakOptions -- controls PAK creation behavior
// ---------------------------------------------------------------------------

struct PakOptions {
    PakCompression compression = PakCompression::None;
    int zstdLevel        = 19;    // 1-19; only consulted when compression == Zstd
    uint32_t alignment   = 16;    // data alignment in bytes (must be power of 2)
};

// ---------------------------------------------------------------------------
// PakOpenOptions -- controls PakReader::Open() runtime behavior
// ---------------------------------------------------------------------------

struct PakOpenOptions {
    // When true, Read()/Load() re-hash each entry's on-disk bytes against its
    // stored content hash and fail closed with PakStatus::HashMismatch on a
    // mismatch instead of returning corrupted data. Off by default: it forces
    // a full read+hash pass even on the mmap zero-copy path underneath
    // Read()/Load(), so it costs real throughput. Prefer PakReader::VerifyEntry()
    // for off-hot-path verification (QA sweeps, "verify game files" flows).
    // View() is never verified, even when this is enabled -- see View()'s doc
    // comment below.
    bool verifyOnRead = false;

    // When false, the archive's name blob is not read into memory. Lookups
    // still work -- Find() hashes the path and matches on the 64-bit path
    // hash -- but the name is no longer available to confirm the match, so a
    // hash collision would resolve to the wrong entry instead of failing, and
    // PakFileInfo::filename, ListFiles(), and GetFileInfo() report nothing.
    //
    // Worth it only where the saving is real: names run around 50 bytes per
    // entry, so a shipping build that resolves everything through precomputed
    // IDs can drop tens of megabytes across a full mount. Leave it on for
    // tools, editors, and anything that reports asset paths to a human.
    bool loadNames = true;
};

// ---------------------------------------------------------------------------
// PakCacheOptions -- runtime decoded asset cache policy
// ---------------------------------------------------------------------------

struct PakCacheOptions {
    bool enabled = true;
    bool memoryCacheEnabled = true;
    bool persistentCacheEnabled = true;
    uint64_t memoryBudgetBytes = 64ull * 1024ull * 1024ull;
    uint64_t persistentBudgetBytes = 512ull * 1024ull * 1024ull;
    uint64_t maxSingleEntryBytes = 64ull * 1024ull * 1024ull;

    // Empty means use PakPlatform::GetDefaultCacheDirectory(). Android and Web
    // return no default, so apps should provide their own writable cache path.
    std::string persistentCacheDirectory;
};

struct PakCacheStats {
    uint64_t memoryHits = 0;
    uint64_t memoryMisses = 0;
    uint64_t persistentHits = 0;
    uint64_t persistentMisses = 0;
    uint64_t sourceReads = 0;
    uint64_t memoryStores = 0;
    uint64_t persistentStores = 0;
    uint64_t memoryEvictions = 0;
    uint64_t persistentEvictions = 0;
    uint64_t memoryBytes = 0;
    uint64_t persistentBytes = 0;
};

// ---------------------------------------------------------------------------
// PakSpan -- non-owning or owning view into asset data
//
// When PakReader uses memory-mapped I/O and the file is uncompressed +
// unencrypted, PakSpan points directly into the mapped region (ownsData=false,
// zero-copy). Otherwise it allocates and owns a buffer (ownsData=true).
//
// Lifetime: non-owning spans keep the underlying memory mapping alive via
// shared ownership. They remain valid even after the originating PakReader
// has been closed or destroyed. The mapping is unmapped when the last
// PakSpan referencing it is destroyed.
// ---------------------------------------------------------------------------

struct PakSpan {
    const uint8_t* data = nullptr;
    uint64_t size = 0;
    bool ownsData = false;

    explicit operator bool() const { return data != nullptr && size > 0; }

    PakSpan() = default;
    ~PakSpan() { if (ownsData && data && !cachedDataRef_) delete[] data; }

    PakSpan(PakSpan&& o) noexcept
        : data(o.data), size(o.size), ownsData(o.ownsData),
          mappingRef_(std::move(o.mappingRef_)),
          cachedDataRef_(std::move(o.cachedDataRef_))
    {
        o.data = nullptr; o.size = 0; o.ownsData = false;
    }

    PakSpan& operator=(PakSpan&& o) noexcept {
        if (this != &o) {
            if (ownsData && data && !cachedDataRef_) delete[] data;
            data = o.data; size = o.size; ownsData = o.ownsData;
            mappingRef_ = std::move(o.mappingRef_);
            cachedDataRef_ = std::move(o.cachedDataRef_);
            o.data = nullptr; o.size = 0; o.ownsData = false;
        }
        return *this;
    }

    PakSpan(const PakSpan&) = delete;
    PakSpan& operator=(const PakSpan&) = delete;

private:
    friend class PakReader;
    // Keeps the mapped file alive for non-owning spans
    std::shared_ptr<PakInternal::MappedFileGuard> mappingRef_;
    std::shared_ptr<const std::vector<uint8_t>> cachedDataRef_;
};

// ---------------------------------------------------------------------------
// Pakker -- build-time API (create, extract, modify PAK files)
//
// Thread safety: Pakker is NOT thread-safe. It is designed for single-threaded
// build-time use. If you need to create multiple PAK files concurrently, use
// separate Pakker instances (each instance has no shared mutable state).
// ---------------------------------------------------------------------------

class Pakker {
public:
    // encryptionKey: pass empty string for no encryption (recommended for shipping)
    explicit Pakker(const std::string& encryptionKey = "");

    // Creates a v6 PAK file with PakOptions for alignment and compression control.
    bool CreatePak(const std::string& pakFilename,
                   const std::map<std::string, std::vector<uint8_t>>& files,
                   const PakOptions& options);

    bool ExtractPak(const std::string& pakFilename,
                    const std::string& outputDir) const;

    bool ListPak(const std::string& pakFilename) const;

    std::vector<std::string> ListFiles(const std::string& pakFilename) const;
    std::vector<std::string> ListFilesWithPrefix(const std::string& pakFilename,
                                                 const std::string& prefix) const;

    std::vector<uint8_t> ReadFileFromPak(const std::string& pakFilename,
                                          const std::string& filename) const;

    std::shared_ptr<std::vector<uint8_t>> LoadFile(const std::string& pakFilename,
                                                    const std::string& filename) const;

    bool AddFileToPak(const std::string& pakFilename,
                      const std::string& filename,
                      const std::vector<uint8_t>& data,
                      PakCompression compression = PakCompression::None,
                      int zstdLevel = 19);

    // Creates a v6 PAK from a folder with PakOptions.
    bool CreatePakFromFolder(const std::string& pakFilename,
                             const std::string& folderPath,
                             const PakOptions& options);

    uint32_t GetFileCount(const std::string& pakFilename) const;
    bool FileExists(const std::string& pakFilename, const std::string& filename) const;

    struct FileInfo {
        std::string filename;
        uint64_t size;
        bool found;
    };
    FileInfo GetFileInfo(const std::string& pakFilename,
                         const std::string& filename) const;

    bool ExtractSingleFile(const std::string& pakFilename,
                           const std::string& filename,
                           const std::string& outputPath) const;

    // Structural validation is always performed (header, bounds, filenames).
    // deepVerify additionally re-hashes every entry's on-disk bytes against
    // its stored content hash -- O(archive size), off by default so routine
    // validation stays fast on multi-GB archives.
    bool ValidatePak(const std::string& pakFilename, bool deepVerify = false) const;

private:
    bool WriteFile(const std::string& filename,
                   const std::vector<uint8_t>& buffer) const;

    std::string encryptionKey_;
};

// ---------------------------------------------------------------------------
// PakReader -- runtime read-only API for shipping builds
//
// Open a PAK once, keep the file table cached in memory, serve reads from the
// persistent handle with O(1) filename lookup.
//
// Thread safety:
//   - Open() and Close() take exclusive locks. Do not call them concurrently
//     with any other method on the same PakReader instance.
//   - All const methods, including Find/View/Read/Load/Prefetch and
//     convenience wrappers, are safe to call concurrently from multiple threads.
//   - Moving a PakReader acquires exclusive locks on the involved instances.
//     Do not move a PakReader while other threads are reading from it.
//   - PakReader is non-copyable, movable.
//
// Memory-mapped I/O:
//   When available (the default), reads are served from memory-mapped pages.
//   Multiple threads can read in parallel without contention.
//   When mmap is unavailable (PAK_NO_MMAP or OS failure), reads fall back
//   to ifstream access serialized by an internal mutex.
// ---------------------------------------------------------------------------

class PakReader {
public:
    // encryptionKey: pass empty string for no encryption (default, fastest)
    explicit PakReader(const std::string& encryptionKey = "");
    ~PakReader();

    // Non-copyable, movable
    PakReader(const PakReader&) = delete;
    PakReader& operator=(const PakReader&) = delete;
    PakReader(PakReader&& other) noexcept;
    PakReader& operator=(PakReader&& other) noexcept;

    // Lifecycle
    bool Open(const std::string& pakFilename);
    bool Open(const std::string& pakFilename, const PakOpenOptions& options);
    void Close();
    bool IsOpen() const;

    // Engine runtime API: resolve once, cache handles, and avoid per-read path work.
    PakFileHandle Find(std::string_view filename) const;

    // Lookup by precomputed PakPathHash(), skipping path hashing entirely.
    // The intended shipping pattern: bake asset IDs at build time, carry
    // those, and never touch a path string at runtime.
    //
    // Unlike Find(), this cannot confirm the match against the stored name,
    // so a 64-bit hash collision resolves to the wrong entry rather than
    // failing. With ~10^6 assets that probability is about 3e-8.
    PakFileHandle FindByHash(uint64_t pathHash) const;

    size_t Resolve(std::span<const std::string_view> filenames,
                   std::span<PakFileHandle> handles) const;
    const PakFileInfo* Info(PakFileHandle handle) const;
    // Returns metadata for the file at [0, GetFileCount()), independent of
    // FindInTable path normalization. Intended for enumeration/composition
    // layers (e.g. a multi-archive VFS) that need to iterate all entries.
    // Returns nullptr if index is out of range or the reader is not open.
    const PakFileInfo* InfoByIndex(uint32_t index) const;

    // Returns a zero-copy pointer into the mapping. The returned PakView is
    // documented to stay valid after Close(), so it takes a reference count on
    // the mapping -- the one atomic read-modify-write left anywhere on the
    // read path, and therefore the one call here that does not scale linearly
    // with thread count when hammered in a tight loop.
    //
    // That is the intended shape of the API rather than a limitation to work
    // around: a view is meant to be taken once when an asset loads and then
    // held for that asset's lifetime, not re-taken per access. Read(), which
    // is what per-frame streaming actually calls, takes no reference count.
    PakStatus View(PakFileHandle handle, PakView& outView) const;
    PakStatus Read(PakFileHandle handle, std::span<uint8_t> destination,
                   uint64_t* bytesWritten = nullptr) const;
    PakStatus Load(PakFileHandle handle, std::vector<uint8_t>& outData) const;
    PakStatus Prefetch(PakFileHandle handle) const;

    // Partial read into decoded/original-content offset space, for large
    // single assets (video/audio) that shouldn't be fully materialized just
    // to read a slice. Supported for uncompressed entries (mapped or
    // streamed, encrypted or not -- XOR-with-repeating-key is range-safe).
    // Compressed entries (LZ4 or Zstd) return PakStatus::Unsupported: this
    // archive format's compressed frames have no internal chunk index, so a
    // true seekable-compressed-range-read would need a block-compression
    // format change, which is out of scope here. Engines needing partial
    // reads on large compressed-in-codec media (already-compressed video/
    // audio) should store those entries uncompressed at the archive level.
    PakStatus ReadRange(PakFileHandle handle, uint64_t rangeOffset,
                        std::span<uint8_t> destination, uint64_t* bytesWritten = nullptr) const;

    // Off-hot-path integrity check: re-hashes the entry's on-disk bytes and
    // compares against its stored content hash, without decompressing or
    // decrypting. Intended for QA sweeps / "verify game files" flows, not
    // per-read hot paths -- see PakOpenOptions::verifyOnRead for a fail-closed
    // on-read-path alternative.
    PakStatus VerifyEntry(PakFileHandle handle) const;

    // Convenience wrappers. Prefer the handle API in runtime engine code.
    std::vector<uint8_t> ReadFile(const std::string& filename) const;
    std::shared_ptr<std::vector<uint8_t>> LoadFile(const std::string& filename) const;

    // Zero-copy read: returns a view into mapped memory for uncompressed +
    // unencrypted files. For compressed/encrypted files, allocates a buffer.
    // The returned PakSpan keeps the underlying mapping alive via shared
    // ownership -- it remains valid even after Close() is called.
    // Thread-safe: may be called concurrently from multiple threads.
    PakSpan ReadFileZeroCopy(const std::string& filename) const;

    // Metadata (no I/O after Open)
    bool FileExists(std::string_view filename) const;
    uint32_t GetFileCount() const;

    // Enumeration, mirroring PakMount's ListFiles()/ListFilesWithPrefix()
    // for the single-archive case. Built from the cached file table -- no I/O.
    std::vector<std::string> ListFiles() const;
    std::vector<std::string> ListFilesWithPrefix(const std::string& prefix) const;

    struct FileInfo {
        std::string filename;
        uint64_t originalSize;
        uint64_t compressedSize;
        bool compressed;
        bool found;
    };
    FileInfo GetFileInfo(const std::string& filename) const;

    // Batch convenience wrapper. Prefer Resolve() and feed handles to
    // the engine job system for parallel work.
    std::vector<std::pair<std::string, std::vector<uint8_t>>>
        ReadFiles(const std::vector<std::string>& filenames) const;

    // Query whether memory-mapped I/O is active
    bool IsMapped() const;

    // Runtime decoded cache control. Caching is enabled by default with
    // conservative budgets; View() remains mmap-only and does not return cache
    // storage.
    void SetCacheOptions(const PakCacheOptions& options);
    PakCacheOptions GetCacheOptions() const;
    PakCacheStats GetCacheStats() const;
    void ClearMemoryCache();
    bool ClearPersistentCache();
    bool ClearCache();

private:
    struct CacheKey {
        uint64_t high = 0;
        uint64_t low = 0;

        friend bool operator==(CacheKey a, CacheKey b) {
            return a.high == b.high && a.low == b.low;
        }
    };

    struct CacheKeyHash {
        size_t operator()(CacheKey key) const noexcept {
            uint64_t mixed = key.high ^ (key.low + 0x9e3779b97f4a7c15ull +
                                         (key.high << 6) + (key.high >> 2));
            return static_cast<size_t>(mixed);
        }
    };

    using CacheLru = std::list<CacheKey>;

    struct DecodedCacheEntry {
        std::shared_ptr<const std::vector<uint8_t>> data;
        uint64_t size = 0;
        CacheLru::iterator lruPosition;
    };

    // The whole file table, in three allocations regardless of entry count.
    //
    // v6 held a std::string per entry inside PakEntry, a second copy inside a
    // node-based unordered_map, plus a separate infos array -- roughly four
    // allocations and ~218 resident bytes per file, and an Open() that
    // touched every one of them. Here the records arrive as one bulk read,
    // the names as a second, and the lookup table is built in place over
    // hashes the archive already stores.
    struct RuntimeTable {
        std::vector<PakFileInfo> infos;
        std::vector<char> nameBlob;            // empty when names are not resident
        std::vector<PakInternal::LookupSlot> lookup;
        uint64_t lookupMask = 0;
        bool namesLoaded = false;
        // Kept in the immutable snapshot so in-flight prefetches do not need
        // to copy the archive path on every handle operation.
        std::string pakFilename;
    };

    // Immutable read-path state, published as a unit by Open() and retired
    // by Close(). Everything a read needs lives here, so the whole hot path
    // reaches it through a single acquire load of snapshot_ -- no lock, and
    // no reference-count traffic.
    //
    // That distinction is the entire point. A shared_lock acquire/release
    // pair and a shared_ptr copy/destroy pair are atomic read-modify-writes,
    // and an atomic RMW must take the cache line exclusively, so every
    // concurrent reader invalidates every other reader's copy of that line.
    // Under a streaming workload fanned across worker threads that turns into
    // pure cache-line ping-pong and aggregate throughput *falls* as threads
    // are added. A plain acquire load never writes, so the line stays Shared
    // in every core and reads scale.
    //
    // Lifetime: snapshotOwner_ owns the snapshot; Close() unpublishes it and
    // then frees it. Callers must not run Close() concurrently with reads --
    // already required by this class's documented threading contract. Values
    // already handed out stay valid regardless, because PakView and PakSpan
    // hold their own shared_ptr to the mapping guard.
    struct ReadSnapshot {
        RuntimeTable table;
        std::shared_ptr<PakInternal::MappedFileGuard> guard;
        const PakInternal::MappedFileGuard* guardPtr = nullptr;
        bool useMmap = false;
        bool verifyOnRead = false;
        uint64_t fileSize = 0;
        uint64_t archiveFingerprint = 0;
    };

    // nullptr when the reader is closed.
    const ReadSnapshot* AcquireSnapshot() const noexcept
    {
        return snapshot_.load(std::memory_order_acquire);
    }

    static bool NeedsPathNormalization(std::string_view path);
    static std::string NormalizePath(std::string_view path);
    static PakFileHandle FindInTable(const RuntimeTable& table, std::string_view filename);
    // Probes the lookup table by path hash. When `verifyName` is non-empty and
    // names are resident, the candidate's name must match it exactly.
    static PakFileHandle FindByHashInTable(const RuntimeTable& table, uint64_t pathHash,
                                           std::string_view verifyName);
    // Returns false if two entries share a path hash, which makes the archive
    // unusable as written.
    static bool BuildLookupTable(RuntimeTable& table);

    // Tears down all open state. Caller must hold mutex_ exclusively. Takes
    // and releases cacheMutex_ internally, so callers must not already hold it.
    void CloseLocked();

    // Resolves a handle against the published snapshot. Returns nullptr and
    // sets outStatus when the reader is closed or the handle is out of range.
    const ReadSnapshot* ResolveHandle(PakFileHandle handle, PakStatus& outStatus) const noexcept;

    PakStatus ValidateReadRequest(const PakFileInfo& entry,
        uint64_t destinationSize, const ReadSnapshot& snapshot) const;
    PakStatus ReadEntryToBuffer(const PakFileInfo& entry,
        std::span<uint8_t> destination, uint64_t* bytesWritten,
        const ReadSnapshot& snapshot) const;
    PakStatus ReadEntryWithCache(PakFileHandle handle, const PakFileInfo& entry,
        std::span<uint8_t> destination, uint64_t* bytesWritten,
        const ReadSnapshot& snapshot) const;

    bool ShouldCacheDecoded(const PakFileInfo& entry,
        const ReadSnapshot& snapshot, const PakCacheOptions& options) const;
    static CacheKey MakeMemoryCacheKey(PakFileHandle handle,
        const ReadSnapshot& snapshot) noexcept;
    CacheKey MakePersistentCacheKey(PakFileHandle handle,
        const PakFileInfo& entry, const ReadSnapshot& snapshot) const;
    PakStatus HashEntrySourceBytes(const PakFileInfo& entry,
        const ReadSnapshot& snapshot, uint64_t& outHash) const;
    bool TryGetMemoryCache(CacheKey key,
        std::shared_ptr<const std::vector<uint8_t>>& outData) const;
    void StoreMemoryCache(CacheKey key,
        std::shared_ptr<const std::vector<uint8_t>> data) const;
    bool TryLoadPersistentCache(CacheKey key,
        std::shared_ptr<const std::vector<uint8_t>>& outData) const;
    void StorePersistentCache(CacheKey key,
        const std::vector<uint8_t>& data) const;
    void ResolvePersistentCacheDirectoryLocked() const;
    void TrimAllMemoryShards() const;
    void TrimPersistentCache() const;
    std::string PersistentCachePathLocked(CacheKey key) const;
    bool HasPersistentCacheDirectoryLocked() const;

    std::string encryptionKey_;
    mutable std::ifstream pakStream_;
    std::string pakFilename_;
    PakInternal::PakHeader header_{};
    uint32_t alignment_ = 1;

    // Owns the published snapshot; only ever touched under mutex_.
    std::shared_ptr<const ReadSnapshot> snapshotOwner_;
    // The hot path's only view of reader state. See ReadSnapshot above.
    std::atomic<const ReadSnapshot*> snapshot_{nullptr};

    // Thread safety. mutex_ now guards Open()/Close()/move and the cold-path
    // members above only -- reads never take it.
    mutable std::shared_mutex mutex_;
    mutable std::mutex streamMutex_;
    // Guards the cache *policy* and the persistent-cache bookkeeping below.
    // It is deliberately not on the decoded-read path any more: it used to be
    // taken once per compressed read merely to copy the options struct, and
    // since that struct holds a std::string, every such read also allocated.
    mutable std::mutex cacheMutex_;

    // Published policy. Readers take the atomic; writers swap it under
    // cacheMutex_. Same publish-once discipline as ReadSnapshot.
    std::shared_ptr<const PakCacheOptions> cacheOptionsOwner_;
    mutable std::atomic<const PakCacheOptions*> cacheOptions_{nullptr};

    const PakCacheOptions& CacheOptions() const noexcept
    {
        const PakCacheOptions* options = cacheOptions_.load(std::memory_order_acquire);
        return options ? *options : kDefaultCacheOptions;
    }
    static const PakCacheOptions kDefaultCacheOptions;

    // Persistent-cache stats only; the memory cache keeps its own per shard.
    mutable PakCacheStats cacheStats_{};
    // Source reads include the uncached mmap fast path, so this counter is
    // bumped by literally every read. A single atomic counter is still a
    // contended RMW on one cache line, which was enough to cap read scaling
    // on its own -- so it is sharded per thread and summed only when
    // GetCacheStats() asks.
    mutable PakInternal::ShardedCounter sourceReads_;
    // The decoded memory cache, split across independently-locked shards.
    //
    // One mutex over one map and one LRU list meant every cache hit from every
    // streaming worker serialized on the same lock, so hit throughput went
    // *down* as threads were added. Sharding by cache key makes unrelated
    // entries independent; each shard keeps its own budget slice, LRU, and
    // counters, and the counters are summed only when GetCacheStats() asks.
    static constexpr size_t kMemoryCacheShards = 16;

    struct MemoryCacheShard {
        mutable std::mutex mutex;
        CacheLru lru;
        std::unordered_map<CacheKey, DecodedCacheEntry, CacheKeyHash> entries;
        uint64_t bytes = 0;
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t stores = 0;
        uint64_t evictions = 0;
    };

    mutable MemoryCacheShard memoryShards_[kMemoryCacheShards];

    // Sharding only pays off once each shard still holds a useful number of
    // entries. Splitting a small budget 16 ways would leave slices too small
    // to admit anything, so the shard count scales with the budget and
    // collapses to 1 -- exactly the old single-lock, exact-LRU behavior --
    // for caches below this threshold.
    static constexpr uint64_t kMinBytesPerShard = 4ull * 1024 * 1024;

    static size_t ActiveShardCount(const PakCacheOptions& options) noexcept
    {
        const uint64_t shards = options.memoryBudgetBytes / kMinBytesPerShard;
        if (shards <= 1) return 1;
        return shards >= kMemoryCacheShards ? kMemoryCacheShards
                                            : static_cast<size_t>(shards);
    }
    static size_t ShardIndexFor(CacheKey key, size_t shardCount) noexcept
    {
        return static_cast<size_t>(CacheKeyHash{}(key)) % shardCount;
    }
    // Each active shard gets an equal slice of the overall budget.
    static uint64_t ShardBudget(const PakCacheOptions& options) noexcept
    {
        return options.memoryBudgetBytes / ActiveShardCount(options);
    }
    void TrimShardLocked(MemoryCacheShard& shard, uint64_t shardBudget) const;
    void AdoptMemoryShards(PakReader& other) const;
    mutable std::string effectivePersistentCacheDirectory_;
};

// ---------------------------------------------------------------------------
// PakMountHandle -- runtime handle into a PakMount's composed namespace
//
// Captures both the winning layer and that layer's own PakFileHandle so
// reads dispatch straight to the owning PakReader with no re-lookup.
// ---------------------------------------------------------------------------

struct PakMountHandle {
    static constexpr uint32_t InvalidLayer = UINT32_MAX;

    uint32_t layerIndex = InvalidLayer;
    PakFileHandle fileHandle{};

    explicit operator bool() const {
        return layerIndex != InvalidLayer && static_cast<bool>(fileHandle);
    }
    friend bool operator==(PakMountHandle a, PakMountHandle b) {
        return a.layerIndex == b.layerIndex && a.fileHandle == b.fileHandle;
    }
    friend bool operator!=(PakMountHandle a, PakMountHandle b) { return !(a == b); }
};

// ---------------------------------------------------------------------------
// PakMount -- layered virtual filesystem over multiple PakReader archives
//
// Composes multiple already-open (or opened-on-mount) PakReader instances
// into one logical asset namespace with override semantics: later-mounted
// layers take priority over earlier ones for files that exist in more than
// one layer. This mirrors "install base, then apply patch/DLC on top."
//
// PakMount does not read archive bytes itself and does not implement its own
// decoded cache -- it dispatches to the winning layer's PakReader, whose own
// cache (see PakCacheOptions, PakReader::SetCacheOptions) applies
// transparently. Use GetLayerReader() to tune cache budgets per layer.
//
// v1 has no per-layer Unmount(): only Clear() (drop every layer). The
// dominant mount pattern -- mount everything once at a load-screen boundary
// -- doesn't need selective removal, and layer identity is ambiguous for
// MountReader()-mounted layers that may carry no filename. This is a
// deliberate, revisitable scope cut, not an oversight.
//
// Thread safety:
//   - Mount(), MountReader(), and Clear() take an exclusive lock and rebuild
//     the merged lookup index. Do not call them concurrently with any other
//     PakMount method on the same instance -- same rule as PakReader's
//     Open()/Close().
//   - Find(), Resolve(), Info(), View(), Read(), Load(), Prefetch(),
//     ListFiles(), ListFilesWithPrefix(), FileExists(), GetFileCount(),
//     LayerCount(), GetLayerReader(), and convenience wrappers are safe to
//     call concurrently from multiple threads, and safe to call concurrently
//     with reads issued directly against a shared_ptr<PakReader> an engine
//     also holds outside the mount (e.g. one obtained via GetLayerReader()
//     or passed into MountReader()).
//   - PakMount is non-copyable, movable. Moving a PakMount acquires an
//     exclusive lock on the instance being moved from.
//   - PakMountHandle values remain valid as long as the layer they reference
//     is still mounted; do not use a handle resolved before a Clear() call.
// ---------------------------------------------------------------------------

class PakMount {
public:
    PakMount() = default;
    ~PakMount() = default;

    PakMount(const PakMount&) = delete;
    PakMount& operator=(const PakMount&) = delete;
    PakMount(PakMount&& other) noexcept;
    PakMount& operator=(PakMount&& other) noexcept;

    // Opens a new PakReader(encryptionKey) on pakFilename and mounts it as
    // the highest-priority layer so far. Returns false (mounting nothing) if
    // Open() fails.
    bool Mount(const std::string& pakFilename, const std::string& encryptionKey = "");

    // Mounts an externally-owned/managed reader as the highest-priority
    // layer so far. `reader` must already be open (IsOpen() == true), or
    // this returns false. Ownership is shared -- the caller may keep using
    // `reader` directly (e.g. for per-layer cache tuning) concurrently with
    // PakMount reads.
    bool MountReader(std::shared_ptr<PakReader> reader);

    // Unmounts every layer and clears the merged index.
    void Clear();

    size_t LayerCount() const;

    // Layer 0 is the first-mounted (lowest-priority) layer; LayerCount()-1
    // is the most-recently-mounted (highest-priority) layer. Returns nullptr
    // if layerIndex is out of range.
    std::shared_ptr<PakReader> GetLayerReader(size_t layerIndex) const;

    // Engine runtime API: same ergonomics as PakReader -- resolve once,
    // cache the handle, dispatch reads without repeated path lookups.
    PakMountHandle Find(std::string_view filename) const;
    size_t Resolve(std::span<const std::string_view> filenames,
                   std::span<PakMountHandle> handles) const;
    const PakFileInfo* Info(PakMountHandle handle) const;
    PakStatus View(PakMountHandle handle, PakView& outView) const;
    PakStatus Read(PakMountHandle handle, std::span<uint8_t> destination,
                   uint64_t* bytesWritten = nullptr) const;
    PakStatus Load(PakMountHandle handle, std::vector<uint8_t>& outData) const;
    PakStatus Prefetch(PakMountHandle handle) const;

    // Convenience wrappers. Prefer the handle API in runtime engine code.
    std::vector<uint8_t> ReadFile(const std::string& filename) const;
    std::shared_ptr<std::vector<uint8_t>> LoadFile(const std::string& filename) const;
    PakSpan ReadFileZeroCopy(const std::string& filename) const;

    bool FileExists(std::string_view filename) const;
    uint32_t GetFileCount() const; // size of the deduplicated merged namespace

    // VFS-aware enumeration: deduplicated by name, highest-priority layer
    // wins on conflicts. Built from the same merged index as Find().
    std::vector<std::string> ListFiles() const;
    std::vector<std::string> ListFilesWithPrefix(const std::string& prefix) const;

private:
    // One slot of the merged open-addressing table.
    //
    // Keyed on the path hash rather than on the path itself. The merged
    // namespace of a large layered install runs to hundreds of thousands of
    // names, and the previous design stored a std::string copy of every one
    // of them in a node-based map *and* re-sorted a second full copy on every
    // Mount() -- so mounting N layers did O(N^2) name work and allocated per
    // entry per mount. A flat power-of-two slot array is memcpy-cheap to copy
    // forward, so each mount overlays only its own layer.
    struct MergedSlot {
        uint64_t pathHash = 0;
        uint32_t layerIndex = PakMountHandle::InvalidLayer;
        PakFileHandle fileHandle{};

        bool Empty() const { return layerIndex == PakMountHandle::InvalidLayer; }
    };

    // Immutable once published; see PakReader::ReadSnapshot for why the hot
    // path reaches this through a plain atomic load rather than a lock plus a
    // shared_ptr copy. layerReaders holds raw pointers whose lifetime is
    // owned by layers_ below, which is sound because mounting may not race
    // with reads (see the threading contract above).
    struct MergedIndex {
        std::vector<MergedSlot> slots;  // power-of-two sized, or empty
        std::vector<PakReader*> layerReaders;
        uint64_t mask = 0;
        uint32_t count = 0;
    };

    static void GrowIndex(MergedIndex& index, uint32_t additionalEntries);
    // Returns the slot's previous occupant, or an Empty() slot when the
    // insert filled a free slot.
    static MergedSlot InsertSlot(MergedIndex& index, uint64_t pathHash,
                                 uint32_t layerIndex, PakFileHandle fileHandle);
    static void OverlayLayer(MergedIndex& index, uint32_t layerIndex, PakReader& reader);
    static const MergedSlot* ProbeIndex(const MergedIndex& index, uint64_t pathHash);
    static PakMountHandle FindInIndex(const MergedIndex& index, std::string_view filename);

    void PublishIndexLocked(std::shared_ptr<const MergedIndex> index);

    const MergedIndex* AcquireIndex() const noexcept
    {
        return index_.load(std::memory_order_acquire);
    }

    mutable std::shared_mutex mutex_;
    std::vector<std::shared_ptr<PakReader>> layers_; // index 0 = lowest priority
    std::shared_ptr<const MergedIndex> indexOwner_;
    std::atomic<const MergedIndex*> index_{nullptr};
};

#endif // PAK_H
