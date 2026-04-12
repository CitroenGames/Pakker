#include "Pak.h"
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cctype>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_set>
#include <thread>

#include "vendor/lz4.h"

namespace fs = std::filesystem;

// ===========================================================================
// Global logging
// ===========================================================================

static std::atomic<PakLogCallback> g_logCallback{nullptr};

void PakSetLogCallback(PakLogCallback cb) { g_logCallback.store(cb, std::memory_order_release); }

namespace PakInternal {

void Log(PakLogLevel level, const std::string& msg)
{
    auto cb = g_logCallback.load(std::memory_order_acquire);
    if (cb) {
        cb(level, msg.c_str());
    }
}

// ===========================================================================
// Shared utility functions
// ===========================================================================

std::string NormalizePathSeparators(const std::string& path)
{
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    auto firstNonSlash = normalized.find_first_not_of('/');
    if (firstNonSlash == std::string::npos) {
        normalized.clear();
    } else if (firstNonSlash > 0) {
        normalized.erase(0, firstNonSlash);
    }
    return normalized;
}

bool IsValidFilename(const std::string& filename)
{
    if (filename.empty() || filename.length() > MAX_FILENAME_LENGTH) return false;

    // Single pass: check for invalid characters, null bytes, and ".." sequences
    static constexpr std::string_view invalidChars = "<>:\"|?*";
    char prev = 0;
    for (char c : filename) {
        if (c == '\0') return false;
        if (invalidChars.find(c) != std::string_view::npos) return false;
        if (c == '.' && prev == '.') return false;
        prev = c;
    }
    return true;
}

uint64_t SafeStreamPos(std::streampos pos)
{
    if (pos == std::streampos(-1)) return 0;
    return static_cast<uint64_t>(pos);
}

bool ValidateEntry(const PakEntry& entry, uint64_t pakFileSize)
{
    uint64_t diskSize = entry.compressedSize > 0 ? entry.compressedSize : entry.originalSize;
    if (entry.offset > pakFileSize ||
        diskSize > pakFileSize ||
        diskSize > pakFileSize - entry.offset) {  // overflow-safe comparison
        return false;
    }
    return IsValidFilename(entry.filename);
}

// ---------------------------------------------------------------------------
// Header I/O
// ---------------------------------------------------------------------------

bool ReadPakHeader(std::istream& stream, PakHeader& header)
{
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!stream) {
        Log(PakLogLevel::Error, "ReadPakHeader: Failed to read PAK header.");
        return false;
    }
    if (std::memcmp(header.magic, PAK_MAGIC.data(), 4) != 0) {
        Log(PakLogLevel::Error, "ReadPakHeader: Invalid magic number.");
        return false;
    }
    if (header.version != PAK_VERSION_1 &&
        header.version != PAK_VERSION_2 &&
        header.version != PAK_VERSION_3) {
        Log(PakLogLevel::Error, "ReadPakHeader: Unsupported PAK version: " + std::to_string(header.version));
        return false;
    }
    if (header.numFiles > MAX_FILES_IN_PAK) {
        Log(PakLogLevel::Error, "ReadPakHeader: Too many files in PAK: " + std::to_string(header.numFiles));
        return false;
    }
    // For v1/v2, the alignment field was part of reserved[] and is 0.
    // Treat 0 as unaligned (alignment = 1).
    return true;
}

bool WritePakHeader(std::ostream& stream, const PakHeader& header)
{
    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!stream) {
        Log(PakLogLevel::Error, "WritePakHeader: Failed to write header.");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// File table I/O -- version-aware
// ---------------------------------------------------------------------------

bool ReadFileTable(std::istream& stream, uint32_t numFiles,
                   uint32_t version, std::vector<PakEntry>& entries)
{
    entries.clear();
    entries.reserve(numFiles);

    for (uint32_t i = 0; i < numFiles; ++i) {
        uint16_t nameLength;
        stream.read(reinterpret_cast<char*>(&nameLength), sizeof(nameLength));
        if (!stream || nameLength == 0 || nameLength > MAX_FILENAME_LENGTH) {
            Log(PakLogLevel::Error, "ReadFileTable: Invalid filename length: " + std::to_string(nameLength));
            return false;
        }

        std::string filename(nameLength, '\0');
        stream.read(&filename[0], nameLength);
        if (!stream) {
            Log(PakLogLevel::Error, "ReadFileTable: Failed to read filename.");
            return false;
        }

        uint64_t offset, originalSize;
        stream.read(reinterpret_cast<char*>(&offset), sizeof(offset));
        stream.read(reinterpret_cast<char*>(&originalSize), sizeof(originalSize));
        if (!stream) {
            Log(PakLogLevel::Error, "ReadFileTable: Failed to read file entry for: " + filename);
            return false;
        }

        uint64_t compressedSize = originalSize;
        uint8_t flags = 0;

        if (version >= PAK_VERSION_2) {
            stream.read(reinterpret_cast<char*>(&compressedSize), sizeof(compressedSize));
            stream.read(reinterpret_cast<char*>(&flags), sizeof(flags));
            if (!stream) {
                Log(PakLogLevel::Error, "ReadFileTable: Failed to read v2 fields for: " + filename);
                return false;
            }
        }

        PakEntry entry(std::move(filename), offset, originalSize, compressedSize, flags);
        if (!IsValidFilename(entry.filename)) {
            Log(PakLogLevel::Error, "ReadFileTable: Invalid filename: " + entry.filename);
            return false;
        }
        entries.emplace_back(std::move(entry));
    }
    return true;
}

bool WriteFileTable(std::ostream& stream, const std::vector<PakEntry>& entries,
                    uint32_t version)
{
    for (const auto& entry : entries) {
        if (entry.filename.length() > MAX_FILENAME_LENGTH) {
            Log(PakLogLevel::Error, "WriteFileTable: Filename too long: " + entry.filename);
            return false;
        }

        uint16_t nameLength = static_cast<uint16_t>(entry.filename.size());
        stream.write(reinterpret_cast<const char*>(&nameLength), sizeof(nameLength));
        stream.write(entry.filename.data(), nameLength);
        stream.write(reinterpret_cast<const char*>(&entry.offset), sizeof(entry.offset));
        stream.write(reinterpret_cast<const char*>(&entry.originalSize), sizeof(entry.originalSize));

        if (version >= PAK_VERSION_2) {
            stream.write(reinterpret_cast<const char*>(&entry.compressedSize), sizeof(entry.compressedSize));
            stream.write(reinterpret_cast<const char*>(&entry.flags), sizeof(entry.flags));
        }

        if (!stream) {
            Log(PakLogLevel::Error, "WriteFileTable: Failed to write file entry for: " + entry.filename);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Encryption
// ---------------------------------------------------------------------------

void EncryptDecrypt(std::vector<uint8_t>& data, const std::string& key)
{
    if (data.empty() || key.empty()) return;
    const size_t keyLength = key.length();
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] ^= static_cast<uint8_t>(key[i % keyLength]);
    }
}

} // namespace PakInternal

using namespace PakInternal;

// Maximum input size for LZ4 functions (from LZ4_MAX_INPUT_SIZE)
static constexpr uint64_t LZ4_MAX_SAFE_SIZE = 0x7E000000;

// Returns true for file extensions known to be already compressed.
// Skipping LZ4 on these avoids wasted CPU time (the result is always discarded).
static bool IsLikelyPreCompressed(const std::string& filename)
{
    auto dotPos = filename.rfind('.');
    if (dotPos == std::string::npos) return false;

    // Lowercase extension into a stack buffer to avoid heap allocation.
    // All known extensions are <= 5 chars (e.g. ".jpeg", ".flac", ".webm").
    constexpr size_t kMaxExtLen = 8;
    size_t extLen = filename.size() - dotPos;
    if (extLen > kMaxExtLen) return false;

    char buf[kMaxExtLen + 1];
    for (size_t i = 0; i < extLen; ++i) {
        buf[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(filename[dotPos + i])));
    }
    buf[extLen] = '\0';
    std::string_view ext(buf, extLen);

    static const std::unordered_set<std::string_view> compressedExts = {
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".avif",
        ".mp3", ".ogg", ".flac", ".aac", ".wma", ".opus",
        ".mp4", ".webm", ".mkv",
        ".zip", ".gz", ".7z", ".rar", ".bz2", ".xz", ".zst",
        ".pak", ".lz4",
    };

    return compressedExts.count(ext) > 0;
}

// Helper: write zero-padding bytes to align stream position
static bool WritePadding(std::ostream& stream, uint32_t alignment)
{
    if (alignment <= 1) return true;
    uint64_t cur = SafeStreamPos(stream.tellp());
    uint64_t aligned = (cur + alignment - 1) & ~(static_cast<uint64_t>(alignment) - 1);
    uint64_t pad = aligned - cur;
    if (pad > 0) {
        static const uint8_t zeros[65536] = {};
        while (pad > 0) {
            uint64_t chunk = pad > sizeof(zeros) ? sizeof(zeros) : pad;
            stream.write(reinterpret_cast<const char*>(zeros),
                        static_cast<std::streamsize>(chunk));
            if (!stream) return false;
            pad -= chunk;
        }
    }
    return true;
}

// ===========================================================================
// Pakker -- build-time API
// ===========================================================================

Pakker::Pakker(const std::string& encryptionKey)
    : encryptionKey_(encryptionKey)
{
}

bool Pakker::WriteFile(const std::string& filename, const std::vector<uint8_t>& buffer) const
{
    std::ofstream fileStream(filename, std::ios::binary);
    if (!fileStream) {
        Log(PakLogLevel::Error, "WriteFile: Unable to create file: " + filename);
        return false;
    }
    if (!buffer.empty()) {
        fileStream.write(reinterpret_cast<const char*>(buffer.data()),
                         static_cast<std::streamsize>(buffer.size()));
        if (!fileStream) {
            Log(PakLogLevel::Error, "WriteFile: Failed to write data to file: " + filename);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// CreatePak -- PakOptions overload (primary implementation)
// ---------------------------------------------------------------------------

bool Pakker::CreatePak(const std::string& pakFilename,
                       const std::map<std::string, std::vector<uint8_t>>& files,
                       const PakOptions& options)
{
    if (files.size() > MAX_FILES_IN_PAK) {
        Log(PakLogLevel::Error, "CreatePak: Too many files to pack: " + std::to_string(files.size()));
        return false;
    }

    uint32_t version = options.formatVersion;
    if (version < PAK_VERSION_2) version = PAK_VERSION_2;
    if (version > PAK_VERSION_3) version = PAK_VERSION_3;

    uint32_t alignment = (version >= PAK_VERSION_3) ? options.alignment : 1;
    // Ensure alignment is a power of 2
    if (alignment == 0) alignment = 1;
    if ((alignment & (alignment - 1)) != 0) {
        Log(PakLogLevel::Error, "CreatePak: Alignment must be a power of 2.");
        return false;
    }

    std::ofstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        Log(PakLogLevel::Error, "CreatePak: Unable to create pak file: " + pakFilename);
        return false;
    }

    PakHeader header;
    header.version = version;
    header.numFiles = static_cast<uint32_t>(files.size());
    header.fileTableOffset = 0;
    header.alignment = (version >= PAK_VERSION_3) ? alignment : 0;

    if (!WritePakHeader(pakStream, header)) return false;

    std::vector<PakEntry> entries;
    entries.reserve(files.size());

    std::vector<uint8_t> compressBuffer;
    std::vector<uint8_t> encryptBuffer;
    std::unordered_set<std::string> seenNames;

    for (const auto& [filename, data] : files) {
        std::string normalizedFilename = NormalizePathSeparators(filename);
        if (!IsValidFilename(normalizedFilename)) {
            Log(PakLogLevel::Error, "CreatePak: Invalid filename: " + filename);
            return false;
        }
        if (!seenNames.insert(normalizedFilename).second) {
            Log(PakLogLevel::Error, "CreatePak: Duplicate filename after normalization: " + normalizedFilename);
            return false;
        }

        // Align file data offset
        if (!WritePadding(pakStream, alignment)) {
            Log(PakLogLevel::Error, "CreatePak: Failed to write alignment padding.");
            return false;
        }

        const uint8_t* writePtr = data.data();
        size_t writeSize = data.size();
        uint8_t flags = 0;
        uint64_t originalSize = data.size();

        if (options.compress && !data.empty() && !IsLikelyPreCompressed(normalizedFilename)) {
            if (data.size() > LZ4_MAX_SAFE_SIZE) {
                Log(PakLogLevel::Warning, "CreatePak: File too large for LZ4, storing uncompressed: " + normalizedFilename);
            } else {
                int maxCompressed = LZ4_compressBound(static_cast<int>(data.size()));
                compressBuffer.resize(maxCompressed);
                int compressedSize = LZ4_compress_default(
                    reinterpret_cast<const char*>(data.data()),
                    reinterpret_cast<char*>(compressBuffer.data()),
                    static_cast<int>(data.size()),
                    maxCompressed);

                if (compressedSize > 0 &&
                    static_cast<uint64_t>(compressedSize) < originalSize) {
                    compressBuffer.resize(compressedSize);
                    writePtr = compressBuffer.data();
                    writeSize = compressedSize;
                    flags = PAK_FLAG_COMPRESSED;
                }
            }
        }

        // Encrypt in-place on whichever buffer writePtr already points to
        if (!encryptionKey_.empty()) {
            if (writePtr == data.data()) {
                // Data is from the const map value; copy into compressBuffer to mutate
                compressBuffer.assign(data.begin(), data.end());
                EncryptDecrypt(compressBuffer, encryptionKey_);
                writePtr = compressBuffer.data();
                writeSize = compressBuffer.size();
            } else {
                // writePtr points to compressBuffer (compression happened); encrypt in-place
                EncryptDecrypt(compressBuffer, encryptionKey_);
            }
        }

        uint64_t currentOffset = SafeStreamPos(pakStream.tellp());
        if (!pakStream) {
            Log(PakLogLevel::Error, "CreatePak: Failed to get stream position.");
            return false;
        }
        entries.emplace_back(normalizedFilename, currentOffset, originalSize,
                             static_cast<uint64_t>(writeSize), flags);

        pakStream.write(reinterpret_cast<const char*>(writePtr),
                       static_cast<std::streamsize>(writeSize));
        if (!pakStream) {
            Log(PakLogLevel::Error, "CreatePak: Failed to write data for file: " + filename);
            return false;
        }
    }

    header.fileTableOffset = SafeStreamPos(pakStream.tellp());
    if (!pakStream) {
        Log(PakLogLevel::Error, "CreatePak: Failed to get file table offset.");
        return false;
    }
    if (!WriteFileTable(pakStream, entries, version)) return false;

    pakStream.seekp(0, std::ios::beg);
    if (!pakStream) {
        Log(PakLogLevel::Error, "CreatePak: Failed to seek to header.");
        return false;
    }
    if (!WritePakHeader(pakStream, header)) return false;

    pakStream.close();
    Log(PakLogLevel::Info, "CreatePak: PAK file '" + pakFilename + "' created successfully.");
    return true;
}

// Legacy overload: produces v2 for backward compatibility
bool Pakker::CreatePak(const std::string& pakFilename,
                       const std::map<std::string, std::vector<uint8_t>>& files,
                       bool compress)
{
    PakOptions opts;
    opts.compress = compress;
    opts.formatVersion = PAK_VERSION_2;
    opts.alignment = 1;
    return CreatePak(pakFilename, files, opts);
}

bool Pakker::ExtractPak(const std::string& pakFilename, const std::string& outputDir) const
{
    std::ifstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        Log(PakLogLevel::Error, "ExtractPak: Unable to open pak file: " + pakFilename);
        return false;
    }

    PakHeader header;
    if (!ReadPakHeader(pakStream, header)) return false;

    pakStream.seekg(0, std::ios::end);
    uint64_t pakFileSize = SafeStreamPos(pakStream.tellg());

    pakStream.seekg(header.fileTableOffset, std::ios::beg);
    if (!pakStream) {
        Log(PakLogLevel::Error, "ExtractPak: Failed to seek to fileTableOffset.");
        return false;
    }

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header.numFiles, header.version, entries)) return false;

    for (const auto& entry : entries) {
        if (!ValidateEntry(entry, pakFileSize)) {
            Log(PakLogLevel::Error, "ExtractPak: Invalid entry detected: " + entry.filename);
            return false;
        }

        pakStream.seekg(entry.offset, std::ios::beg);
        if (!pakStream) {
            Log(PakLogLevel::Error, "ExtractPak: Failed to seek to offset for file: " + entry.filename);
            return false;
        }

        uint64_t diskSize = entry.compressedSize;
        std::vector<uint8_t> fileData(diskSize);
        pakStream.read(reinterpret_cast<char*>(fileData.data()),
                      static_cast<std::streamsize>(diskSize));
        if (!pakStream) {
            Log(PakLogLevel::Error, "ExtractPak: Failed to read data for file: " + entry.filename);
            return false;
        }

        EncryptDecrypt(fileData, encryptionKey_);

        if (entry.flags & PAK_FLAG_COMPRESSED) {
            if (entry.originalSize > LZ4_MAX_SAFE_SIZE || fileData.size() > LZ4_MAX_SAFE_SIZE) {
                Log(PakLogLevel::Error, "ExtractPak: Entry exceeds LZ4 size limit: " + entry.filename);
                return false;
            }
            std::vector<uint8_t> decompressed(entry.originalSize);
            int result = LZ4_decompress_safe(
                reinterpret_cast<const char*>(fileData.data()),
                reinterpret_cast<char*>(decompressed.data()),
                static_cast<int>(fileData.size()),
                static_cast<int>(entry.originalSize));
            if (result < 0 || static_cast<uint64_t>(result) != entry.originalSize) {
                Log(PakLogLevel::Error, "ExtractPak: Decompression failed for: " + entry.filename);
                return false;
            }
            fileData = std::move(decompressed);
        }

        std::string outputPath = (fs::path(outputDir) / entry.filename).string();
        fs::path sanitizedOutputPath = fs::weakly_canonical(fs::path(outputPath));
        fs::path sanitizedOutputDir = fs::weakly_canonical(fs::path(outputDir));
        auto rel = fs::relative(sanitizedOutputPath, sanitizedOutputDir);
        std::string relStr = rel.string();
        if (relStr.empty() || relStr.starts_with("..")) {
            Log(PakLogLevel::Error, "ExtractPak: Detected path traversal: " + entry.filename);
            return false;
        }

        fs::create_directories(fs::path(outputPath).parent_path());
        if (!WriteFile(outputPath, fileData)) return false;
    }

    Log(PakLogLevel::Info, "ExtractPak: All files extracted successfully to '" + outputDir + "'.");
    return true;
}

bool Pakker::ListPak(const std::string& pakFilename) const
{
    std::ifstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        Log(PakLogLevel::Error, "ListPak: Unable to open pak file: " + pakFilename);
        return false;
    }

    PakHeader header;
    if (!ReadPakHeader(pakStream, header)) return false;

    pakStream.seekg(header.fileTableOffset, std::ios::beg);
    if (!pakStream) {
        Log(PakLogLevel::Error, "ListPak: Failed to seek to fileTableOffset.");
        return false;
    }

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header.numFiles, header.version, entries)) return false;

    Log(PakLogLevel::Info, "ListPak: Contents of '" + pakFilename + "':");
    for (const auto& entry : entries) {
        std::ostringstream oss;
        oss << " - " << entry.filename
            << " (Offset: " << entry.offset
            << ", Size: " << entry.originalSize << " bytes";
        if (entry.flags & PAK_FLAG_COMPRESSED) {
            oss << ", Compressed: " << entry.compressedSize << " bytes";
        }
        oss << ")";
        Log(PakLogLevel::Info, oss.str());
    }
    return true;
}

std::vector<uint8_t> Pakker::ReadFileFromPak(const std::string& pakFilename,
                                              const std::string& filename) const
{
    std::ifstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        Log(PakLogLevel::Error, "ReadFileFromPak: Unable to open pak file: " + pakFilename);
        return {};
    }

    PakHeader header;
    if (!ReadPakHeader(pakStream, header)) return {};

    pakStream.seekg(0, std::ios::end);
    uint64_t pakFileSize = SafeStreamPos(pakStream.tellg());

    pakStream.seekg(header.fileTableOffset, std::ios::beg);
    if (!pakStream) return {};

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header.numFiles, header.version, entries)) return {};

    std::string normalizedFilename = NormalizePathSeparators(filename);
    for (const auto& entry : entries) {
        if (entry.filename == normalizedFilename) {
            if (!ValidateEntry(entry, pakFileSize)) return {};

            pakStream.seekg(entry.offset, std::ios::beg);
            if (!pakStream) return {};

            uint64_t diskSize = entry.compressedSize;
            std::vector<uint8_t> fileData(diskSize);
            pakStream.read(reinterpret_cast<char*>(fileData.data()),
                          static_cast<std::streamsize>(diskSize));
            if (!pakStream) return {};

            EncryptDecrypt(fileData, encryptionKey_);

            if (entry.flags & PAK_FLAG_COMPRESSED) {
                if (entry.originalSize > LZ4_MAX_SAFE_SIZE || fileData.size() > LZ4_MAX_SAFE_SIZE) {
                    Log(PakLogLevel::Error, "ReadFileFromPak: Entry exceeds LZ4 size limit: " + entry.filename);
                    return {};
                }
                std::vector<uint8_t> decompressed(entry.originalSize);
                int result = LZ4_decompress_safe(
                    reinterpret_cast<const char*>(fileData.data()),
                    reinterpret_cast<char*>(decompressed.data()),
                    static_cast<int>(fileData.size()),
                    static_cast<int>(entry.originalSize));
                if (result < 0 || static_cast<uint64_t>(result) != entry.originalSize) {
                    Log(PakLogLevel::Error, "ReadFileFromPak: Decompression failed for: " + entry.filename);
                    return {};
                }
                return decompressed;
            }
            return fileData;
        }
    }

    Log(PakLogLevel::Error, "ReadFileFromPak: File not found in pak: " + filename);
    return {};
}

std::shared_ptr<std::vector<uint8_t>> Pakker::LoadFile(const std::string& pakFilename,
                                                        const std::string& filename) const
{
    if (!FileExists(pakFilename, filename)) return nullptr;
    auto data = ReadFileFromPak(pakFilename, filename);
    return std::make_shared<std::vector<uint8_t>>(std::move(data));
}

bool Pakker::AddFileToPak(const std::string& pakFilename,
                          const std::string& filename,
                          const std::vector<uint8_t>& data,
                          bool compress)
{
    std::string normalizedFilename = NormalizePathSeparators(filename);
    if (!IsValidFilename(normalizedFilename)) {
        Log(PakLogLevel::Error, "AddFileToPak: Invalid filename: " + filename);
        return false;
    }

    std::fstream pakStream(pakFilename, std::ios::in | std::ios::out | std::ios::binary);
    if (!pakStream) {
        Log(PakLogLevel::Error, "AddFileToPak: Unable to open pak file: " + pakFilename);
        return false;
    }

    PakHeader header;
    if (!ReadPakHeader(pakStream, header)) return false;

    uint32_t alignment = (header.alignment > 0) ? header.alignment : 1;

    pakStream.seekg(header.fileTableOffset, std::ios::beg);
    if (!pakStream) return false;

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header.numFiles, header.version, entries)) return false;

    auto it = std::find_if(entries.begin(), entries.end(), [&](const PakEntry& e) {
        return e.filename == normalizedFilename;
    });
    if (it != entries.end()) {
        Log(PakLogLevel::Error, "AddFileToPak: File already exists in pak: " + filename);
        return false;
    }
    if (header.numFiles + 1 > MAX_FILES_IN_PAK) {
        Log(PakLogLevel::Error, "AddFileToPak: Would exceed maximum file count.");
        return false;
    }

    pakStream.seekp(header.fileTableOffset, std::ios::beg);
    if (!pakStream) return false;

    // Write alignment padding
    if (!WritePadding(pakStream, alignment)) {
        Log(PakLogLevel::Error, "AddFileToPak: Failed to write alignment padding.");
        return false;
    }

    uint64_t newOffset = SafeStreamPos(pakStream.tellp());
    if (!pakStream) {
        Log(PakLogLevel::Error, "AddFileToPak: Failed to get stream position.");
        return false;
    }

    const uint8_t* writePtr = data.data();
    size_t writeSize = data.size();
    uint8_t flags = 0;
    uint64_t originalSize = data.size();
    std::vector<uint8_t> compressBuffer;

    // Compress if requested and the file is compressible
    if (compress && !data.empty() && !IsLikelyPreCompressed(normalizedFilename)
        && header.version >= PAK_VERSION_2) {
        if (data.size() > LZ4_MAX_SAFE_SIZE) {
            Log(PakLogLevel::Warning, "AddFileToPak: File too large for LZ4, storing uncompressed: " + normalizedFilename);
        } else {
            int maxCompressed = LZ4_compressBound(static_cast<int>(data.size()));
            compressBuffer.resize(maxCompressed);
            int compressedSize = LZ4_compress_default(
                reinterpret_cast<const char*>(data.data()),
                reinterpret_cast<char*>(compressBuffer.data()),
                static_cast<int>(data.size()),
                maxCompressed);

            if (compressedSize > 0 &&
                static_cast<uint64_t>(compressedSize) < originalSize) {
                compressBuffer.resize(compressedSize);
                writePtr = compressBuffer.data();
                writeSize = compressedSize;
                flags = PAK_FLAG_COMPRESSED;
            }
        }
    }

    // Encrypt
    std::vector<uint8_t> encryptBuffer;
    if (!encryptionKey_.empty()) {
        encryptBuffer.assign(writePtr, writePtr + writeSize);
        EncryptDecrypt(encryptBuffer, encryptionKey_);
        writePtr = encryptBuffer.data();
        writeSize = encryptBuffer.size();
    }

    pakStream.write(reinterpret_cast<const char*>(writePtr),
                   static_cast<std::streamsize>(writeSize));
    if (!pakStream) return false;

    entries.emplace_back(normalizedFilename, newOffset, originalSize,
                         static_cast<uint64_t>(writeSize), flags);
    header.numFiles += 1;
    header.fileTableOffset = SafeStreamPos(pakStream.tellp());
    if (!pakStream) {
        Log(PakLogLevel::Error, "AddFileToPak: Failed to get file table offset.");
        return false;
    }

    pakStream.seekp(0, std::ios::beg);
    if (!WritePakHeader(pakStream, header)) return false;

    pakStream.seekp(header.fileTableOffset, std::ios::beg);
    if (!WriteFileTable(pakStream, entries, header.version)) return false;

    // Truncate any leftover bytes from the old file table
    uint64_t finalSize = SafeStreamPos(pakStream.tellp());
    pakStream.close();
    try {
        fs::resize_file(pakFilename, finalSize);
    } catch (const fs::filesystem_error&) {
        // Non-fatal: file is functionally correct, just may have trailing bytes
    }

    Log(PakLogLevel::Info, "AddFileToPak: File '" + filename + "' added successfully.");
    return true;
}

// ---------------------------------------------------------------------------
// CreatePakFromFolder -- PakOptions overload (primary implementation)
// ---------------------------------------------------------------------------

bool Pakker::CreatePakFromFolder(const std::string& pakFilename,
                                 const std::string& folderPath,
                                 const PakOptions& options)
{
    // Collect file paths first without loading contents into memory
    std::vector<std::pair<std::string, fs::path>> filePaths; // (normalized name, disk path)
    try {
        for (const auto& entry : fs::recursive_directory_iterator(folderPath)) {
            if (fs::is_regular_file(entry.path())) {
                std::string relativePath = fs::relative(entry.path(), folderPath).string();
                relativePath = NormalizePathSeparators(relativePath);
                if (!IsValidFilename(relativePath)) {
                    Log(PakLogLevel::Warning, "CreatePakFromFolder: Skipping invalid filename: " + relativePath);
                    continue;
                }
                filePaths.emplace_back(relativePath, entry.path());
            }
        }
    } catch (const fs::filesystem_error& e) {
        Log(PakLogLevel::Error, std::string("CreatePakFromFolder: Filesystem error: ") + e.what());
        return false;
    }

    // Sort for deterministic output
    std::sort(filePaths.begin(), filePaths.end());

    if (filePaths.size() > MAX_FILES_IN_PAK) {
        Log(PakLogLevel::Error, "CreatePakFromFolder: Too many files to pack: " + std::to_string(filePaths.size()));
        return false;
    }

    uint32_t version = options.formatVersion;
    if (version < PAK_VERSION_2) version = PAK_VERSION_2;
    if (version > PAK_VERSION_3) version = PAK_VERSION_3;

    uint32_t alignment = (version >= PAK_VERSION_3) ? options.alignment : 1;
    if (alignment == 0) alignment = 1;
    if ((alignment & (alignment - 1)) != 0) {
        Log(PakLogLevel::Error, "CreatePakFromFolder: Alignment must be a power of 2.");
        return false;
    }

    std::ofstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        Log(PakLogLevel::Error, "CreatePakFromFolder: Unable to create pak file: " + pakFilename);
        return false;
    }

    PakHeader header;
    header.version = version;
    header.numFiles = static_cast<uint32_t>(filePaths.size());
    header.fileTableOffset = 0;
    header.alignment = (version >= PAK_VERSION_3) ? alignment : 0;

    if (!WritePakHeader(pakStream, header)) return false;

    std::vector<PakEntry> entries;
    entries.reserve(filePaths.size());

    std::vector<uint8_t> compressBuffer;

    // Stream each file from disk one at a time to avoid loading everything into memory
    for (const auto& [normalizedName, diskPath] : filePaths) {
        // Bulk read: open at end to get size, then read in one call
        std::ifstream file(diskPath, std::ios::binary | std::ios::ate);
        if (!file) {
            Log(PakLogLevel::Warning, "CreatePakFromFolder: Failed to open file: " + diskPath.string());
            continue;
        }
        auto fileSize = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> data(static_cast<size_t>(fileSize));
        if (fileSize > 0) {
            file.read(reinterpret_cast<char*>(data.data()), fileSize);
            if (!file) {
                Log(PakLogLevel::Warning, "CreatePakFromFolder: Failed to read file: " + diskPath.string());
                continue;
            }
        }
        file.close();

        // Align file data offset
        if (!WritePadding(pakStream, alignment)) {
            Log(PakLogLevel::Error, "CreatePakFromFolder: Failed to write alignment padding.");
            return false;
        }

        const uint8_t* writePtr = data.data();
        size_t writeSize = data.size();
        uint8_t flags = 0;
        uint64_t originalSize = data.size();

        if (options.compress && !data.empty() && !IsLikelyPreCompressed(normalizedName)) {
            if (data.size() > LZ4_MAX_SAFE_SIZE) {
                Log(PakLogLevel::Warning, "CreatePakFromFolder: File too large for LZ4, storing uncompressed: " + normalizedName);
            } else {
                int maxCompressed = LZ4_compressBound(static_cast<int>(data.size()));
                compressBuffer.resize(maxCompressed);
                int compressedSize = LZ4_compress_default(
                    reinterpret_cast<const char*>(data.data()),
                    reinterpret_cast<char*>(compressBuffer.data()),
                    static_cast<int>(data.size()),
                    maxCompressed);

                if (compressedSize > 0 &&
                    static_cast<uint64_t>(compressedSize) < originalSize) {
                    compressBuffer.resize(compressedSize);
                    writePtr = compressBuffer.data();
                    writeSize = compressedSize;
                    flags = PAK_FLAG_COMPRESSED;
                }
            }
        }

        // Encrypt in-place on the buffer writePtr already points to
        if (!encryptionKey_.empty()) {
            if (writePtr == data.data()) {
                EncryptDecrypt(data, encryptionKey_);
            } else {
                EncryptDecrypt(compressBuffer, encryptionKey_);
            }
            // writePtr remains valid -- EncryptDecrypt does not resize
        }

        uint64_t currentOffset = SafeStreamPos(pakStream.tellp());
        if (!pakStream) {
            Log(PakLogLevel::Error, "CreatePakFromFolder: Failed to get stream position.");
            return false;
        }
        entries.emplace_back(normalizedName, currentOffset, originalSize,
                             static_cast<uint64_t>(writeSize), flags);

        pakStream.write(reinterpret_cast<const char*>(writePtr),
                       static_cast<std::streamsize>(writeSize));
        if (!pakStream) {
            Log(PakLogLevel::Error, "CreatePakFromFolder: Failed to write data for file: " + normalizedName);
            return false;
        }
    }

    header.fileTableOffset = SafeStreamPos(pakStream.tellp());
    if (!pakStream) {
        Log(PakLogLevel::Error, "CreatePakFromFolder: Failed to get file table offset.");
        return false;
    }
    header.numFiles = static_cast<uint32_t>(entries.size());
    if (!WriteFileTable(pakStream, entries, version)) return false;

    pakStream.seekp(0, std::ios::beg);
    if (!pakStream) {
        Log(PakLogLevel::Error, "CreatePakFromFolder: Failed to seek to header.");
        return false;
    }
    if (!WritePakHeader(pakStream, header)) return false;

    pakStream.close();
    Log(PakLogLevel::Info, "CreatePakFromFolder: PAK file '" + pakFilename + "' created successfully.");
    return true;
}

// Legacy overload: produces v2 for backward compatibility
bool Pakker::CreatePakFromFolder(const std::string& pakFilename,
                                 const std::string& folderPath,
                                 bool compress)
{
    PakOptions opts;
    opts.compress = compress;
    opts.formatVersion = PAK_VERSION_2;
    opts.alignment = 1;
    return CreatePakFromFolder(pakFilename, folderPath, opts);
}

uint32_t Pakker::GetFileCount(const std::string& pakFilename) const
{
    std::ifstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) return 0;
    PakHeader header;
    if (!ReadPakHeader(pakStream, header)) return 0;
    return header.numFiles;
}

bool Pakker::FileExists(const std::string& pakFilename, const std::string& filename) const
{
    return GetFileInfo(pakFilename, filename).found;
}

Pakker::FileInfo Pakker::GetFileInfo(const std::string& pakFilename,
                                     const std::string& filename) const
{
    FileInfo info{};
    info.found = false;

    std::ifstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) return info;

    PakHeader header;
    if (!ReadPakHeader(pakStream, header)) return info;

    pakStream.seekg(header.fileTableOffset, std::ios::beg);
    if (!pakStream) return info;

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header.numFiles, header.version, entries)) return info;

    std::string normalizedFilename = NormalizePathSeparators(filename);
    auto it = std::find_if(entries.begin(), entries.end(),
                          [&](const PakEntry& e) { return e.filename == normalizedFilename; });
    if (it != entries.end()) {
        info.filename = it->filename;
        info.size = it->originalSize;
        info.found = true;
    }
    return info;
}

bool Pakker::ExtractSingleFile(const std::string& pakFilename,
                               const std::string& filename,
                               const std::string& outputPath) const
{
    // ReadFileFromPak returns empty vector for both "not found" and "0-byte file".
    // Use FileExists to distinguish the two cases.
    if (!FileExists(pakFilename, filename)) {
        Log(PakLogLevel::Error, "ExtractSingleFile: File not found in pak: " + filename);
        return false;
    }
    std::vector<uint8_t> fileData = ReadFileFromPak(pakFilename, filename);
    fs::create_directories(fs::path(outputPath).parent_path());
    return WriteFile(outputPath, fileData);
}

bool Pakker::ValidatePak(const std::string& pakFilename) const
{
    std::ifstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        Log(PakLogLevel::Error, "ValidatePak: Unable to open pak file: " + pakFilename);
        return false;
    }

    pakStream.seekg(0, std::ios::end);
    uint64_t pakFileSize = SafeStreamPos(pakStream.tellg());
    pakStream.seekg(0, std::ios::beg);

    PakHeader header;
    if (!ReadPakHeader(pakStream, header)) return false;

    if (header.fileTableOffset >= pakFileSize) {
        Log(PakLogLevel::Error, "ValidatePak: Invalid file table offset.");
        return false;
    }

    pakStream.seekg(header.fileTableOffset, std::ios::beg);
    if (!pakStream) return false;

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header.numFiles, header.version, entries)) return false;

    for (const auto& entry : entries) {
        if (!ValidateEntry(entry, pakFileSize)) {
            Log(PakLogLevel::Error, "ValidatePak: Invalid entry: " + entry.filename);
            return false;
        }
    }

    Log(PakLogLevel::Info, "ValidatePak: PAK file '" + pakFilename + "' is valid.");
    return true;
}

// ===========================================================================
// PakReader -- runtime read-only API for shipping builds
// ===========================================================================

PakReader::PakReader(const std::string& encryptionKey)
    : encryptionKey_(encryptionKey)
{
}

PakReader::~PakReader()
{
    Close();
}

PakReader::PakReader(PakReader&& other) noexcept
    : encryptionKey_()
      // mutex_ and streamMutex_ are default-constructed (non-movable)
{
    std::unique_lock lock(other.mutex_);

    encryptionKey_ = std::move(other.encryptionKey_);
    pakStream_ = std::move(other.pakStream_);
    pakFilename_ = std::move(other.pakFilename_);
    header_ = other.header_;
    pakFileSize_ = other.pakFileSize_;
    formatVersion_ = other.formatVersion_;
    isOpen_ = other.isOpen_;
    fileTable_ = std::move(other.fileTable_);
    mappedGuard_ = std::move(other.mappedGuard_);
    useMmap_ = other.useMmap_;
    alignment_ = other.alignment_;

    other.isOpen_ = false;
    other.pakFileSize_ = 0;
    other.formatVersion_ = 0;
    other.useMmap_ = false;
    other.alignment_ = 1;
}

PakReader& PakReader::operator=(PakReader&& other) noexcept
{
    if (this != &other) {
        // Lock both mutexes in address order to prevent deadlock
        std::unique_lock<std::shared_mutex> lk1, lk2;
        if (this < &other) {
            lk1 = std::unique_lock(mutex_);
            lk2 = std::unique_lock(other.mutex_);
        } else {
            lk2 = std::unique_lock(other.mutex_);
            lk1 = std::unique_lock(mutex_);
        }

        // Close current state (inline, we already hold our lock)
        if (isOpen_) {
            mappedGuard_.reset();
            useMmap_ = false;
            pakStream_.close();
            fileTable_.clear();
            header_ = PakInternal::PakHeader{};
            pakFileSize_ = 0;
            formatVersion_ = 0;
            isOpen_ = false;
            alignment_ = 1;
            pakFilename_.clear();
        }

        encryptionKey_ = std::move(other.encryptionKey_);
        pakStream_ = std::move(other.pakStream_);
        pakFilename_ = std::move(other.pakFilename_);
        header_ = other.header_;
        pakFileSize_ = other.pakFileSize_;
        formatVersion_ = other.formatVersion_;
        isOpen_ = other.isOpen_;
        fileTable_ = std::move(other.fileTable_);
        mappedGuard_ = std::move(other.mappedGuard_);
        useMmap_ = other.useMmap_;
        alignment_ = other.alignment_;
        // mutex_ and streamMutex_ stay as-is (non-movable)

        other.isOpen_ = false;
        other.pakFileSize_ = 0;
        other.formatVersion_ = 0;
        other.useMmap_ = false;
        other.alignment_ = 1;
    }
    return *this;
}

bool PakReader::Open(const std::string& pakFilename)
{
    std::unique_lock lock(mutex_);
    if (isOpen_) {
        // Close without locking (we already hold the lock)
        mappedGuard_.reset();
        useMmap_ = false;
        pakStream_.close();
        fileTable_.clear();
        header_ = PakHeader{};
        pakFileSize_ = 0;
        formatVersion_ = 0;
        isOpen_ = false;
        alignment_ = 1;
    }

    pakFilename_ = pakFilename;

    pakStream_.open(pakFilename, std::ios::binary);
    if (!pakStream_) {
        Log(PakLogLevel::Error, "PakReader::Open: Unable to open pak file: " + pakFilename);
        return false;
    }

    if (!ReadPakHeader(pakStream_, header_)) {
        pakStream_.close();
        return false;
    }
    formatVersion_ = header_.version;
    alignment_ = (header_.alignment > 0) ? header_.alignment : 1;

    pakStream_.seekg(0, std::ios::end);
    pakFileSize_ = SafeStreamPos(pakStream_.tellg());

    pakStream_.seekg(header_.fileTableOffset, std::ios::beg);
    if (!pakStream_) {
        Log(PakLogLevel::Error, "PakReader::Open: Failed to seek to file table.");
        pakStream_.close();
        return false;
    }

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream_, header_.numFiles, formatVersion_, entries)) {
        pakStream_.close();
        return false;
    }

    // Build hash map for O(1) lookup
    fileTable_.reserve(entries.size());
    for (auto& entry : entries) {
        if (!ValidateEntry(entry, pakFileSize_)) {
            Log(PakLogLevel::Error, "PakReader::Open: Invalid entry: " + entry.filename);
            pakStream_.close();
            fileTable_.clear();
            return false;
        }
        std::string key = entry.filename;
        fileTable_.emplace(std::move(key), std::move(entry));
    }

    // Attempt memory-mapped I/O -- close ifstream first to avoid double file handle
    pakStream_.close();

    auto guard = std::make_shared<PakInternal::MappedFileGuard>();
    guard->mf = PakPlatform::MapFileReadOnly(pakFilename.c_str());
    if (guard->mf.data && guard->mf.size == pakFileSize_) {
        mappedGuard_ = std::move(guard);
        useMmap_ = true;
    } else {
        // Fallback: re-open ifstream (guard destructor unmaps if needed)
        useMmap_ = false;
        pakStream_.open(pakFilename, std::ios::binary);
        if (!pakStream_) {
            Log(PakLogLevel::Error, "PakReader::Open: Failed to re-open pak file for streaming.");
            fileTable_.clear();
            return false;
        }
    }

    isOpen_ = true;
    Log(PakLogLevel::Info, "PakReader::Open: Opened '" + pakFilename + "' with " +
        std::to_string(header_.numFiles) + " files" +
        (useMmap_ ? " (memory-mapped)" : " (streaming)") + ".");
    return true;
}

void PakReader::Close()
{
    std::unique_lock lock(mutex_);
    if (isOpen_) {
        mappedGuard_.reset();
        useMmap_ = false;
        pakStream_.close();
        fileTable_.clear();
        header_ = PakHeader{};
        pakFileSize_ = 0;
        formatVersion_ = 0;
        isOpen_ = false;
        alignment_ = 1;
        pakFilename_.clear();
    }
}

bool PakReader::IsOpen() const
{
    std::shared_lock lock(mutex_);
    return isOpen_;
}

bool PakReader::IsMapped() const
{
    std::shared_lock lock(mutex_);
    return useMmap_;
}

// ---------------------------------------------------------------------------
// ReadEntry -- branch on mmap vs ifstream
// ---------------------------------------------------------------------------

std::vector<uint8_t> PakReader::ReadEntry(const PakInternal::PakEntry& entry,
    bool useMmap, uint64_t fileSize, const std::string& encryptionKey,
    const std::shared_ptr<PakInternal::MappedFileGuard>& guard) const
{
    uint64_t diskSize = entry.compressedSize;

    if (useMmap && guard) {
        // Memory-mapped path: direct pointer, no syscall
        if (entry.offset + diskSize > fileSize) {
            Log(PakLogLevel::Error, "PakReader: Entry exceeds file bounds: " + entry.filename);
            return {};
        }

        const uint8_t* rawPtr = static_cast<const uint8_t*>(guard->mf.data) + entry.offset;

        // If encrypted, we need a mutable copy
        if (!encryptionKey.empty()) {
            std::vector<uint8_t> rawData(rawPtr, rawPtr + diskSize);
            EncryptDecrypt(rawData, encryptionKey);

            if (entry.flags & PAK_FLAG_COMPRESSED) {
                if (entry.originalSize > LZ4_MAX_SAFE_SIZE || rawData.size() > LZ4_MAX_SAFE_SIZE) {
                    Log(PakLogLevel::Error, "PakReader: Entry exceeds LZ4 size limit: " + entry.filename);
                    return {};
                }
                thread_local std::vector<uint8_t> decompBuf;
                decompBuf.resize(entry.originalSize);
                int result = LZ4_decompress_safe(
                    reinterpret_cast<const char*>(rawData.data()),
                    reinterpret_cast<char*>(decompBuf.data()),
                    static_cast<int>(rawData.size()),
                    static_cast<int>(entry.originalSize));
                if (result < 0 || static_cast<uint64_t>(result) != entry.originalSize) {
                    Log(PakLogLevel::Error, "PakReader: Decompression failed for: " + entry.filename);
                    return {};
                }
                return {decompBuf.begin(), decompBuf.end()};
            }
            return rawData;
        }

        // No encryption
        if (entry.flags & PAK_FLAG_COMPRESSED) {
            if (entry.originalSize > LZ4_MAX_SAFE_SIZE || diskSize > LZ4_MAX_SAFE_SIZE) {
                Log(PakLogLevel::Error, "PakReader: Entry exceeds LZ4 size limit: " + entry.filename);
                return {};
            }
            thread_local std::vector<uint8_t> decompBuf;
            decompBuf.resize(entry.originalSize);
            int result = LZ4_decompress_safe(
                reinterpret_cast<const char*>(rawPtr),
                reinterpret_cast<char*>(decompBuf.data()),
                static_cast<int>(diskSize),
                static_cast<int>(entry.originalSize));
            if (result < 0 || static_cast<uint64_t>(result) != entry.originalSize) {
                Log(PakLogLevel::Error, "PakReader: Decompression failed for: " + entry.filename);
                return {};
            }
            return {decompBuf.begin(), decompBuf.end()};
        }

        // Uncompressed, unencrypted: copy from mmap
        return {rawPtr, rawPtr + diskSize};
    }

    // ifstream fallback: serialize access to the stream
    std::lock_guard streamLock(streamMutex_);

    pakStream_.seekg(entry.offset, std::ios::beg);
    if (!pakStream_) {
        Log(PakLogLevel::Error, "PakReader: Failed to seek for file: " + entry.filename);
        return {};
    }

    std::vector<uint8_t> rawData(diskSize);
    pakStream_.read(reinterpret_cast<char*>(rawData.data()),
                    static_cast<std::streamsize>(diskSize));
    if (!pakStream_) {
        Log(PakLogLevel::Error, "PakReader: Failed to read file: " + entry.filename);
        return {};
    }

    EncryptDecrypt(rawData, encryptionKey);

    if (entry.flags & PAK_FLAG_COMPRESSED) {
        if (entry.originalSize > LZ4_MAX_SAFE_SIZE || rawData.size() > LZ4_MAX_SAFE_SIZE) {
            Log(PakLogLevel::Error, "PakReader: Entry exceeds LZ4 size limit: " + entry.filename);
            return {};
        }
        std::vector<uint8_t> decompressed(entry.originalSize);
        int result = LZ4_decompress_safe(
            reinterpret_cast<const char*>(rawData.data()),
            reinterpret_cast<char*>(decompressed.data()),
            static_cast<int>(rawData.size()),
            static_cast<int>(entry.originalSize));
        if (result < 0 || static_cast<uint64_t>(result) != entry.originalSize) {
            Log(PakLogLevel::Error, "PakReader: Decompression failed for: " + entry.filename);
            return {};
        }
        return decompressed;
    }

    return rawData;
}

// ---------------------------------------------------------------------------
// ReadEntryFromMmap -- mmap-only path for parallel batch reads
// ---------------------------------------------------------------------------

std::vector<uint8_t> PakReader::ReadEntryFromMmap(const PakInternal::PakEntry& entry,
    uint64_t fileSize, const std::string& encryptionKey,
    const std::shared_ptr<PakInternal::MappedFileGuard>& guard) const
{
    uint64_t diskSize = entry.compressedSize;

    if (!guard || entry.offset + diskSize > fileSize) {
        Log(PakLogLevel::Error, "PakReader: Entry exceeds file bounds: " + entry.filename);
        return {};
    }

    const uint8_t* rawPtr = static_cast<const uint8_t*>(guard->mf.data) + entry.offset;

    if (!encryptionKey.empty()) {
        std::vector<uint8_t> rawData(rawPtr, rawPtr + diskSize);
        EncryptDecrypt(rawData, encryptionKey);

        if (entry.flags & PAK_FLAG_COMPRESSED) {
            if (entry.originalSize > LZ4_MAX_SAFE_SIZE || rawData.size() > LZ4_MAX_SAFE_SIZE) {
                Log(PakLogLevel::Error, "PakReader: Entry exceeds LZ4 size limit: " + entry.filename);
                return {};
            }
            thread_local std::vector<uint8_t> decompBuf;
            decompBuf.resize(entry.originalSize);
            int result = LZ4_decompress_safe(
                reinterpret_cast<const char*>(rawData.data()),
                reinterpret_cast<char*>(decompBuf.data()),
                static_cast<int>(rawData.size()),
                static_cast<int>(entry.originalSize));
            if (result < 0 || static_cast<uint64_t>(result) != entry.originalSize) return {};
            return {decompBuf.begin(), decompBuf.end()};
        }
        return rawData;
    }

    if (entry.flags & PAK_FLAG_COMPRESSED) {
        if (entry.originalSize > LZ4_MAX_SAFE_SIZE || diskSize > LZ4_MAX_SAFE_SIZE) {
            Log(PakLogLevel::Error, "PakReader: Entry exceeds LZ4 size limit: " + entry.filename);
            return {};
        }
        thread_local std::vector<uint8_t> decompBuf;
        decompBuf.resize(entry.originalSize);
        int result = LZ4_decompress_safe(
            reinterpret_cast<const char*>(rawPtr),
            reinterpret_cast<char*>(decompBuf.data()),
            static_cast<int>(diskSize),
            static_cast<int>(entry.originalSize));
        if (result < 0 || static_cast<uint64_t>(result) != entry.originalSize) return {};
        return {decompBuf.begin(), decompBuf.end()};
    }

    return {rawPtr, rawPtr + diskSize};
}

// ---------------------------------------------------------------------------
// Public read methods
// ---------------------------------------------------------------------------

std::vector<uint8_t> PakReader::ReadFile(const std::string& filename) const
{
    PakInternal::PakEntry entryCopy;
    std::shared_ptr<PakInternal::MappedFileGuard> localGuard;
    bool localUseMmap;
    uint64_t localFileSize;
    std::string localEncryptionKey;
    {
        std::shared_lock lock(mutex_);
        if (!isOpen_) {
            Log(PakLogLevel::Error, "PakReader::ReadFile: No PAK file is open.");
            return {};
        }

        std::string normalized = PakInternal::NormalizePathSeparators(filename);
        auto it = fileTable_.find(normalized);
        if (it == fileTable_.end()) {
            Log(PakLogLevel::Error, "PakReader::ReadFile: File not found: " + filename);
            return {};
        }
        entryCopy = it->second;
        localGuard = mappedGuard_;
        localUseMmap = useMmap_;
        localFileSize = pakFileSize_;
        localEncryptionKey = encryptionKey_;
    }
    // Lock released -- ReadEntry proceeds without blocking Open/Close
    return ReadEntry(entryCopy, localUseMmap, localFileSize, localEncryptionKey, localGuard);
}

std::shared_ptr<std::vector<uint8_t>> PakReader::LoadFile(const std::string& filename) const
{
    if (!FileExists(filename)) return nullptr;
    auto data = ReadFile(filename);
    return std::make_shared<std::vector<uint8_t>>(std::move(data));
}

PakSpan PakReader::ReadFileZeroCopy(const std::string& filename) const
{
    PakSpan span;
    PakInternal::PakEntry entryCopy;
    std::shared_ptr<PakInternal::MappedFileGuard> localGuard;
    bool localUseMmap;
    uint64_t localFileSize;
    std::string localEncryptionKey;
    {
        std::shared_lock lock(mutex_);
        if (!isOpen_) {
            Log(PakLogLevel::Error, "PakReader::ReadFileZeroCopy: No PAK file is open.");
            return span;
        }

        std::string normalized = PakInternal::NormalizePathSeparators(filename);
        auto it = fileTable_.find(normalized);
        if (it == fileTable_.end()) {
            Log(PakLogLevel::Error, "PakReader::ReadFileZeroCopy: File not found: " + filename);
            return span;
        }
        entryCopy = it->second;
        localGuard = mappedGuard_;
        localUseMmap = useMmap_;
        localFileSize = pakFileSize_;
        localEncryptionKey = encryptionKey_;
    }
    // Lock released

    // True zero-copy: only possible with mmap + uncompressed + unencrypted
    if (localUseMmap && localGuard &&
        !(entryCopy.flags & PakInternal::PAK_FLAG_COMPRESSED) && localEncryptionKey.empty()) {
        if (entryCopy.offset + entryCopy.originalSize > localFileSize) {
            PakInternal::Log(PakLogLevel::Error, "PakReader::ReadFileZeroCopy: Entry exceeds file bounds.");
            return span;
        }
        span.data = static_cast<const uint8_t*>(localGuard->mf.data) + entryCopy.offset;
        span.size = entryCopy.originalSize;
        span.ownsData = false;
        span.mappingRef_ = std::move(localGuard);  // shared ownership keeps mapping alive
        return span;
    }

    // Fallback: allocate and read
    auto vec = ReadEntry(entryCopy, localUseMmap, localFileSize, localEncryptionKey, localGuard);
    if (vec.empty()) {
        // Empty file is valid -- return a zero-size owning span to distinguish from error
        if (entryCopy.originalSize == 0) {
            span.data = nullptr;
            span.size = 0;
            span.ownsData = true;  // signals "found but empty" vs default-constructed "not found"
        }
        return span;
    }

    auto* buf = new uint8_t[vec.size()];
    std::memcpy(buf, vec.data(), vec.size());
    span.data = buf;
    span.size = vec.size();
    span.ownsData = true;
    return span;
}

bool PakReader::FileExists(const std::string& filename) const
{
    std::shared_lock lock(mutex_);
    if (!isOpen_) return false;
    std::string normalized = NormalizePathSeparators(filename);
    return fileTable_.count(normalized) > 0;
}

uint32_t PakReader::GetFileCount() const
{
    std::shared_lock lock(mutex_);
    if (!isOpen_) return 0;
    return static_cast<uint32_t>(fileTable_.size());
}

PakReader::FileInfo PakReader::GetFileInfo(const std::string& filename) const
{
    std::shared_lock lock(mutex_);
    FileInfo info{};
    info.found = false;
    info.compressed = false;
    info.originalSize = 0;
    info.compressedSize = 0;

    if (!isOpen_) return info;

    std::string normalized = NormalizePathSeparators(filename);
    auto it = fileTable_.find(normalized);
    if (it != fileTable_.end()) {
        info.filename = it->second.filename;
        info.originalSize = it->second.originalSize;
        info.compressedSize = it->second.compressedSize;
        info.compressed = (it->second.flags & PAK_FLAG_COMPRESSED) != 0;
        info.found = true;
    }
    return info;
}

// ---------------------------------------------------------------------------
// Batch read -- parallel decompression with mmap
// ---------------------------------------------------------------------------

std::vector<std::pair<std::string, std::vector<uint8_t>>>
PakReader::ReadFiles(const std::vector<std::string>& filenames) const
{
    // Phase 1: resolve entries under shared lock, copy them out
    struct ResolvedEntry {
        size_t originalIndex;
        PakInternal::PakEntry entry;  // copy, not pointer -- safe after lock release
    };
    std::vector<ResolvedEntry> resolved;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> results;
    std::shared_ptr<PakInternal::MappedFileGuard> localGuard;
    bool localUseMmap;
    uint64_t localFileSize;
    std::string localEncryptionKey;
    {
        std::shared_lock lock(mutex_);
        if (!isOpen_ || filenames.empty()) return results;

        resolved.reserve(filenames.size());
        results.resize(filenames.size());

        for (size_t i = 0; i < filenames.size(); ++i) {
            std::string normalized = PakInternal::NormalizePathSeparators(filenames[i]);
            auto it = fileTable_.find(normalized);
            if (it != fileTable_.end()) {
                resolved.push_back({i, it->second});
            }
            results[i].first = filenames[i];
        }
        localGuard = mappedGuard_;
        localUseMmap = useMmap_;
        localFileSize = pakFileSize_;
        localEncryptionKey = encryptionKey_;
    }
    // Lock released -- I/O proceeds without blocking Open/Close

    // Phase 2: read/decompress (parallel for mmap, sequential for ifstream)
    if (localUseMmap && localGuard) {
        unsigned int numThreads = std::thread::hardware_concurrency();
        if (numThreads == 0) numThreads = 2;
        if (numThreads > resolved.size()) numThreads = static_cast<unsigned int>(resolved.size());

        if (numThreads <= 1 || resolved.size() <= 2) {
            for (const auto& r : resolved) {
                results[r.originalIndex].second = ReadEntryFromMmap(r.entry,
                    localFileSize, localEncryptionKey, localGuard);
            }
        } else {
            std::vector<std::thread> threads;
            threads.reserve(numThreads);

            size_t chunkSize = (resolved.size() + numThreads - 1) / numThreads;
            for (unsigned int t = 0; t < numThreads; ++t) {
                size_t start = t * chunkSize;
                size_t end = std::min(start + chunkSize, resolved.size());
                if (start >= end) break;

                threads.emplace_back([this, &resolved, &results, start, end,
                                      localFileSize, &localEncryptionKey, &localGuard]() {
                    for (size_t i = start; i < end; ++i) {
                        results[resolved[i].originalIndex].second =
                            ReadEntryFromMmap(resolved[i].entry,
                                localFileSize, localEncryptionKey, localGuard);
                    }
                });
            }

            for (auto& t : threads) t.join();
        }
    } else {
        // ifstream fallback: read in offset order for sequential I/O
        std::sort(resolved.begin(), resolved.end(),
                  [](const ResolvedEntry& a, const ResolvedEntry& b) {
                      return a.entry.offset < b.entry.offset;
                  });

        for (const auto& r : resolved) {
            results[r.originalIndex].second = ReadEntry(r.entry,
                localUseMmap, localFileSize, localEncryptionKey, localGuard);
        }
    }

    return results;
}
