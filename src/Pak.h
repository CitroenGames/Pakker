#ifndef PAK_H
#define PAK_H

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <string_view>
#include <fstream>
#include <functional>
#include <shared_mutex>
#include <mutex>

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
static constexpr size_t MAX_FILES_IN_PAK    = 1000000;
static constexpr std::string_view PAK_MAGIC = "PAK0";
static constexpr uint32_t PAK_VERSION_1     = 1;
static constexpr uint32_t PAK_VERSION_2     = 2; // compression support
static constexpr uint32_t PAK_VERSION_3     = 3; // alignment + mmap optimizations

#pragma pack(push, 1)
struct PakHeader {
    char magic[4] = { 'P', 'A', 'K', '0' };
    uint32_t version   = PAK_VERSION_3;
    uint32_t numFiles  = 0;
    uint64_t fileTableOffset = 0;
    uint32_t alignment = 0;         // data alignment in bytes (power of 2); 0 = unaligned (v1/v2)
    uint32_t reserved[3] = {0, 0, 0};
};
#pragma pack(pop)
static_assert(sizeof(PakHeader) == 36, "PakHeader must be 36 bytes with no padding");

struct PakEntry {
    std::string filename;
    uint64_t offset       = 0;
    uint64_t originalSize = 0;
    uint64_t compressedSize = 0; // == originalSize when uncompressed (v2+)
    uint8_t  flags        = 0;   // bit 0: compressed (v2+)

    PakEntry() = default;
    PakEntry(std::string name, uint64_t off, uint64_t origSz,
             uint64_t compSz = 0, uint8_t f = 0)
        : filename(std::move(name)), offset(off), originalSize(origSz),
          compressedSize(compSz == 0 ? origSz : compSz), flags(f) {}
};

static constexpr uint8_t PAK_FLAG_COMPRESSED = 0x01;

// Internal helpers shared by Pakker and PakReader
void Log(PakLogLevel level, const std::string& msg);

std::string NormalizePathSeparators(const std::string& path);
bool IsValidFilename(const std::string& filename);
uint64_t SafeStreamPos(std::streampos pos);
bool ValidateEntry(const PakEntry& entry, uint64_t pakFileSize);

bool ReadPakHeader(std::istream& stream, PakHeader& header);
bool ReadFileTable(std::istream& stream, uint32_t numFiles,
                   uint32_t version, std::vector<PakEntry>& entries);
bool WritePakHeader(std::ostream& stream, const PakHeader& header);
bool WriteFileTable(std::ostream& stream, const std::vector<PakEntry>& entries,
                    uint32_t version);

void EncryptDecrypt(std::vector<uint8_t>& data, const std::string& key);

// RAII guard for memory-mapped file. Shared between PakReader and PakSpan
// instances so the mapping stays alive until the last reference is released.
struct MappedFileGuard {
    PakPlatform::MappedFile mf{};
    ~MappedFileGuard() { PakPlatform::UnmapFile(mf); }
    MappedFileGuard() = default;
    MappedFileGuard(const MappedFileGuard&) = delete;
    MappedFileGuard& operator=(const MappedFileGuard&) = delete;
};

} // namespace PakInternal

// ---------------------------------------------------------------------------
// PakOptions -- controls PAK creation behavior
// ---------------------------------------------------------------------------

struct PakOptions {
    bool compress        = false;  // LZ4 per-file compression
    uint32_t alignment   = 16;    // data alignment in bytes (must be power of 2)
    uint32_t formatVersion = 3;   // 2 for legacy compat, 3 for aligned
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
    ~PakSpan() { if (ownsData && data) delete[] data; }

    PakSpan(PakSpan&& o) noexcept
        : data(o.data), size(o.size), ownsData(o.ownsData),
          mappingRef_(std::move(o.mappingRef_))
    {
        o.data = nullptr; o.size = 0; o.ownsData = false;
    }

    PakSpan& operator=(PakSpan&& o) noexcept {
        if (this != &o) {
            if (ownsData && data) delete[] data;
            data = o.data; size = o.size; ownsData = o.ownsData;
            mappingRef_ = std::move(o.mappingRef_);
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

    // Creates a PAK file with PakOptions for alignment and format control.
    bool CreatePak(const std::string& pakFilename,
                   const std::map<std::string, std::vector<uint8_t>>& files,
                   const PakOptions& options);

    // Legacy overload -- produces v2 format for backward compatibility.
    bool CreatePak(const std::string& pakFilename,
                   const std::map<std::string, std::vector<uint8_t>>& files,
                   bool compress = false);

    bool ExtractPak(const std::string& pakFilename,
                    const std::string& outputDir) const;

    bool ListPak(const std::string& pakFilename) const;

    std::vector<uint8_t> ReadFileFromPak(const std::string& pakFilename,
                                          const std::string& filename) const;

    std::shared_ptr<std::vector<uint8_t>> LoadFile(const std::string& pakFilename,
                                                    const std::string& filename) const;

    bool AddFileToPak(const std::string& pakFilename,
                      const std::string& filename,
                      const std::vector<uint8_t>& data,
                      bool compress = false);

    // Creates a PAK from a folder with PakOptions.
    bool CreatePakFromFolder(const std::string& pakFilename,
                             const std::string& folderPath,
                             const PakOptions& options);

    // Legacy overload -- produces v2 format.
    bool CreatePakFromFolder(const std::string& pakFilename,
                             const std::string& folderPath,
                             bool compress = false);

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

    bool ValidatePak(const std::string& pakFilename) const;

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
//   - All const methods (ReadFile, ReadFileZeroCopy, LoadFile, FileExists,
//     GetFileCount, GetFileInfo, ReadFiles, IsOpen, IsMapped) are safe to
//     call concurrently from multiple threads.
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
    void Close();
    bool IsOpen() const;

    // Single file read (allocates result buffer)
    std::vector<uint8_t> ReadFile(const std::string& filename) const;
    std::shared_ptr<std::vector<uint8_t>> LoadFile(const std::string& filename) const;

    // Zero-copy read: returns a view into mapped memory for uncompressed +
    // unencrypted files. For compressed/encrypted files, allocates a buffer.
    // The returned PakSpan keeps the underlying mapping alive via shared
    // ownership -- it remains valid even after Close() is called.
    // Thread-safe: may be called concurrently from multiple threads.
    PakSpan ReadFileZeroCopy(const std::string& filename) const;

    // Metadata (no I/O after Open)
    bool FileExists(const std::string& filename) const;
    uint32_t GetFileCount() const;

    struct FileInfo {
        std::string filename;
        uint64_t originalSize;
        uint64_t compressedSize;
        bool compressed;
        bool found;
    };
    FileInfo GetFileInfo(const std::string& filename) const;

    // Batch read -- reads multiple files. Uses parallel decompression with mmap.
    std::vector<std::pair<std::string, std::vector<uint8_t>>>
        ReadFiles(const std::vector<std::string>& filenames) const;

    // Query whether memory-mapped I/O is active
    bool IsMapped() const;

private:
    std::vector<uint8_t> ReadEntry(const PakInternal::PakEntry& entry,
        bool useMmap, uint64_t fileSize, const std::string& encryptionKey,
        const std::shared_ptr<PakInternal::MappedFileGuard>& guard) const;
    std::vector<uint8_t> ReadEntryFromMmap(const PakInternal::PakEntry& entry,
        uint64_t fileSize, const std::string& encryptionKey,
        const std::shared_ptr<PakInternal::MappedFileGuard>& guard) const;

    std::string encryptionKey_;
    mutable std::ifstream pakStream_;
    std::string pakFilename_;
    PakInternal::PakHeader header_{};
    uint64_t pakFileSize_ = 0;
    uint32_t formatVersion_ = 0;
    bool isOpen_ = false;

    // O(1) lookup: normalized filename -> entry
    std::unordered_map<std::string, PakInternal::PakEntry> fileTable_;

    // Memory-mapped I/O (shared ownership with PakSpan instances)
    std::shared_ptr<PakInternal::MappedFileGuard> mappedGuard_;
    bool useMmap_ = false;
    uint32_t alignment_ = 1;

    // Thread safety
    mutable std::shared_mutex mutex_;
    mutable std::mutex streamMutex_;
};

#endif // PAK_H
