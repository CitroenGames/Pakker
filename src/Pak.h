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

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

enum class PakLogLevel { Info, Warning, Error };
using PakLogCallback = void(*)(PakLogLevel level, const char* message);

// Set a global log callback. When null (default), all logging is suppressed.
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

struct PakHeader {
    char magic[4] = { 'P', 'A', 'K', '0' };
    uint32_t version   = PAK_VERSION_2;
    uint32_t numFiles  = 0;
    uint64_t fileTableOffset = 0;
    uint32_t reserved[4] = {0, 0, 0, 0};
};

struct PakEntry {
    std::string filename;
    uint64_t offset       = 0;
    uint64_t originalSize = 0;
    uint64_t compressedSize = 0; // == originalSize when uncompressed (v2)
    uint8_t  flags        = 0;   // bit 0: compressed (v2)

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

} // namespace PakInternal

// ---------------------------------------------------------------------------
// Pakker -- build-time API (create, extract, modify PAK files)
// ---------------------------------------------------------------------------

class Pakker {
public:
    // encryptionKey: pass empty string for no encryption (recommended for shipping)
    explicit Pakker(const std::string& encryptionKey = "");

    // Creates a PAK file. compress=true uses LZ4 per-file compression.
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
                      const std::vector<uint8_t>& data);

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
// NOT thread-safe: callers must synchronize access externally.
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

    // Single file read
    std::vector<uint8_t> ReadFile(const std::string& filename) const;
    std::shared_ptr<std::vector<uint8_t>> LoadFile(const std::string& filename) const;

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

    // Batch read -- reads multiple files, sorted by offset for sequential I/O
    std::vector<std::pair<std::string, std::vector<uint8_t>>>
        ReadFiles(const std::vector<std::string>& filenames) const;

private:
    std::vector<uint8_t> ReadEntry(const PakInternal::PakEntry& entry) const;

    std::string encryptionKey_;
    mutable std::ifstream pakStream_;
    PakInternal::PakHeader header_{};
    uint64_t pakFileSize_ = 0;
    uint32_t formatVersion_ = 0;
    bool isOpen_ = false;

    // O(1) lookup: normalized filename -> entry
    std::unordered_map<std::string, PakInternal::PakEntry> fileTable_;
};

#endif // PAK_H
