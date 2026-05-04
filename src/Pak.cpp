#include "Pak.h"
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_set>

#include "vendor/lz4.h"

namespace fs = std::filesystem;

// ===========================================================================
// Global logging
// ===========================================================================

static std::atomic<PakLogCallback> g_logCallback{nullptr};

void PakSetLogCallback(PakLogCallback cb) { g_logCallback.store(cb, std::memory_order_release); }

const char* PakStatusToString(PakStatus status)
{
    switch (status) {
        case PakStatus::Ok: return "Ok";
        case PakStatus::NotOpen: return "NotOpen";
        case PakStatus::NotFound: return "NotFound";
        case PakStatus::InvalidHandle: return "InvalidHandle";
        case PakStatus::InvalidArgument: return "InvalidArgument";
        case PakStatus::BufferTooSmall: return "BufferTooSmall";
        case PakStatus::Unsupported: return "Unsupported";
        case PakStatus::CorruptArchive: return "CorruptArchive";
        case PakStatus::IoError: return "IoError";
        case PakStatus::DecompressionFailed: return "DecompressionFailed";
    }
    return "Unknown";
}

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

uint64_t SafeStreamPos(std::ios& stream, std::streampos pos)
{
    if (pos == std::streampos(-1)) {
        stream.setstate(std::ios::failbit);
        return 0;
    }
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
    if (header.version != PAK_VERSION_4) {
        Log(PakLogLevel::Error, "ReadPakHeader: Unsupported PAK version: " + std::to_string(header.version));
        return false;
    }
    if (header.numFiles > MAX_FILES_IN_PAK) {
        Log(PakLogLevel::Error, "ReadPakHeader: Too many files in PAK: " + std::to_string(header.numFiles));
        return false;
    }
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
// File table I/O
// ---------------------------------------------------------------------------

bool ReadFileTable(std::istream& stream, uint32_t numFiles,
                   std::vector<PakEntry>& entries)
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

        uint64_t compressedSize = 0;
        uint8_t flags = 0;
        stream.read(reinterpret_cast<char*>(&compressedSize), sizeof(compressedSize));
        stream.read(reinterpret_cast<char*>(&flags), sizeof(flags));
        if (!stream) {
            Log(PakLogLevel::Error, "ReadFileTable: Failed to read compression fields for: " + filename);
            return false;
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

bool WriteFileTable(std::ostream& stream, const std::vector<PakEntry>& entries)
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
        stream.write(reinterpret_cast<const char*>(&entry.compressedSize), sizeof(entry.compressedSize));
        stream.write(reinterpret_cast<const char*>(&entry.flags), sizeof(entry.flags));

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

static void EncryptDecryptSpan(std::span<uint8_t> data, const std::string& key)
{
    if (data.empty() || key.empty()) return;
    const size_t keyLength = key.length();
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] ^= static_cast<uint8_t>(key[i % keyLength]);
    }
}

// Helper: write zero-padding bytes to align stream position
static bool WritePadding(std::ostream& stream, uint32_t alignment)
{
    if (alignment <= 1) return true;
    uint64_t cur = SafeStreamPos(stream, stream.tellp());
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

    uint32_t alignment = options.alignment;
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
    header.version = PAK_VERSION_4;
    header.numFiles = static_cast<uint32_t>(files.size());
    header.fileTableOffset = 0;
    header.alignment = alignment;

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

        if (options.compress && !data.empty()) {
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

        uint64_t currentOffset = SafeStreamPos(pakStream, pakStream.tellp());
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

    header.fileTableOffset = SafeStreamPos(pakStream, pakStream.tellp());
    if (!pakStream) {
        Log(PakLogLevel::Error, "CreatePak: Failed to get file table offset.");
        return false;
    }
    if (!WriteFileTable(pakStream, entries)) return false;

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
    uint64_t pakFileSize = SafeStreamPos(pakStream, pakStream.tellg());

    pakStream.seekg(header.fileTableOffset, std::ios::beg);
    if (!pakStream) {
        Log(PakLogLevel::Error, "ExtractPak: Failed to seek to fileTableOffset.");
        return false;
    }

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header.numFiles, entries)) return false;

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
    if (!ReadFileTable(pakStream, header.numFiles, entries)) return false;

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

std::vector<std::string> Pakker::ListFiles(const std::string& pakFilename) const
{
    std::ifstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        Log(PakLogLevel::Error, "ListFiles: Unable to open pak file: " + pakFilename);
        return {};
    }

    PakHeader header;
    if (!ReadPakHeader(pakStream, header)) return {};

    pakStream.seekg(header.fileTableOffset, std::ios::beg);
    if (!pakStream) {
        Log(PakLogLevel::Error, "ListFiles: Failed to seek to fileTableOffset.");
        return {};
    }

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header.numFiles, entries)) return {};

    std::vector<std::string> files;
    files.reserve(entries.size());
    for (const auto& entry : entries) {
        files.push_back(entry.filename);
    }

    std::sort(files.begin(), files.end());
    return files;
}

std::vector<std::string> Pakker::ListFilesWithPrefix(const std::string& pakFilename,
                                                     const std::string& prefix) const
{
    const std::string normalizedPrefix = NormalizePathSeparators(prefix);
    const auto files = ListFiles(pakFilename);

    std::vector<std::string> matchingFiles;
    for (const auto& file : files) {
        if (file.starts_with(normalizedPrefix)) {
            matchingFiles.push_back(file);
        }
    }

    return matchingFiles;
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
    uint64_t pakFileSize = SafeStreamPos(pakStream, pakStream.tellg());

    pakStream.seekg(header.fileTableOffset, std::ios::beg);
    if (!pakStream) return {};

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header.numFiles, entries)) return {};

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
    if ((alignment & (alignment - 1)) != 0) {
        Log(PakLogLevel::Error, "AddFileToPak: Alignment in header is not a power of 2.");
        return false;
    }

    pakStream.seekg(header.fileTableOffset, std::ios::beg);
    if (!pakStream) return false;

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header.numFiles, entries)) return false;

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

    uint64_t newOffset = SafeStreamPos(pakStream, pakStream.tellp());
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
    if (compress && !data.empty()) {
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
    header.fileTableOffset = SafeStreamPos(pakStream, pakStream.tellp());
    if (!pakStream) {
        Log(PakLogLevel::Error, "AddFileToPak: Failed to get file table offset.");
        return false;
    }

    pakStream.seekp(0, std::ios::beg);
    if (!WritePakHeader(pakStream, header)) return false;

    pakStream.seekp(header.fileTableOffset, std::ios::beg);
    if (!WriteFileTable(pakStream, entries)) return false;

    // Truncate any leftover bytes from the old file table
    uint64_t finalSize = SafeStreamPos(pakStream, pakStream.tellp());
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

    uint32_t alignment = options.alignment;
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
    header.version = PAK_VERSION_4;
    header.numFiles = static_cast<uint32_t>(filePaths.size());
    header.fileTableOffset = 0;
    header.alignment = alignment;

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
        if (fileSize == std::streampos(-1)) {
            Log(PakLogLevel::Warning, "CreatePakFromFolder: Failed to get file size: " + diskPath.string());
            continue;
        }
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

        if (options.compress && !data.empty()) {
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

        uint64_t currentOffset = SafeStreamPos(pakStream, pakStream.tellp());
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

    header.fileTableOffset = SafeStreamPos(pakStream, pakStream.tellp());
    if (!pakStream) {
        Log(PakLogLevel::Error, "CreatePakFromFolder: Failed to get file table offset.");
        return false;
    }
    header.numFiles = static_cast<uint32_t>(entries.size());
    if (!WriteFileTable(pakStream, entries)) return false;

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
    if (!ReadFileTable(pakStream, header.numFiles, entries)) return info;

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
    uint64_t pakFileSize = SafeStreamPos(pakStream, pakStream.tellg());
    pakStream.seekg(0, std::ios::beg);

    PakHeader header;
    if (!ReadPakHeader(pakStream, header)) return false;

    if (header.fileTableOffset > pakFileSize) {
        Log(PakLogLevel::Error, "ValidatePak: Invalid file table offset.");
        return false;
    }

    pakStream.seekg(header.fileTableOffset, std::ios::beg);
    if (!pakStream) return false;

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header.numFiles, entries)) return false;

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
    isOpen_ = other.isOpen_;
    table_ = std::move(other.table_);
    mappedGuard_ = std::move(other.mappedGuard_);
    useMmap_ = other.useMmap_;
    alignment_ = other.alignment_;

    other.isOpen_ = false;
    other.pakFileSize_ = 0;
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
            table_.reset();
            header_ = PakInternal::PakHeader{};
            pakFileSize_ = 0;
            isOpen_ = false;
            alignment_ = 1;
            pakFilename_.clear();
        }

        encryptionKey_ = std::move(other.encryptionKey_);
        pakStream_ = std::move(other.pakStream_);
        pakFilename_ = std::move(other.pakFilename_);
        header_ = other.header_;
        pakFileSize_ = other.pakFileSize_;
        isOpen_ = other.isOpen_;
        table_ = std::move(other.table_);
        mappedGuard_ = std::move(other.mappedGuard_);
        useMmap_ = other.useMmap_;
        alignment_ = other.alignment_;
        // mutex_ and streamMutex_ stay as-is (non-movable)

        other.isOpen_ = false;
        other.pakFileSize_ = 0;
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
        table_.reset();
        header_ = PakHeader{};
        pakFileSize_ = 0;
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
    alignment_ = (header_.alignment > 0) ? header_.alignment : 1;

    pakStream_.seekg(0, std::ios::end);
    pakFileSize_ = SafeStreamPos(pakStream_, pakStream_.tellg());

    if (header_.fileTableOffset > pakFileSize_) {
        Log(PakLogLevel::Error, "PakReader::Open: Invalid file table offset.");
        pakStream_.close();
        return false;
    }

    pakStream_.seekg(header_.fileTableOffset, std::ios::beg);
    if (!pakStream_) {
        Log(PakLogLevel::Error, "PakReader::Open: Failed to seek to file table.");
        pakStream_.close();
        return false;
    }

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream_, header_.numFiles, entries)) {
        pakStream_.close();
        return false;
    }

    auto table = std::make_shared<RuntimeTable>();
    table->entries.reserve(entries.size());
    table->infos.reserve(entries.size());
    table->indexByName.reserve(entries.size());

    for (auto& entry : entries) {
        if (!ValidateEntry(entry, pakFileSize_)) {
            Log(PakLogLevel::Error, "PakReader::Open: Invalid entry: " + entry.filename);
            pakStream_.close();
            return false;
        }

        uint32_t index = static_cast<uint32_t>(table->entries.size());
        table->entries.emplace_back(std::move(entry));

        const auto& storedEntry = table->entries.back();
        PakFileInfo info{};
        info.filename = storedEntry.filename;
        info.originalSize = storedEntry.originalSize;
        info.compressedSize = storedEntry.compressedSize;
        info.offset = storedEntry.offset;
        info.compressed = (storedEntry.flags & PakInternal::PAK_FLAG_COMPRESSED) != 0;
        table->infos.emplace_back(info);

        auto [_, inserted] = table->indexByName.emplace(storedEntry.filename, index);
        if (!inserted) {
            Log(PakLogLevel::Error, "PakReader::Open: Duplicate entry: " + storedEntry.filename);
            pakStream_.close();
            return false;
        }
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
            return false;
        }
    }

    table_ = std::move(table);
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
        table_.reset();
        header_ = PakHeader{};
        pakFileSize_ = 0;
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
// Handle lookup and metadata
// ---------------------------------------------------------------------------

bool PakReader::NeedsPathNormalization(std::string_view path)
{
    return path.find('\\') != std::string_view::npos ||
           (!path.empty() && path.front() == '/');
}

std::string PakReader::NormalizePath(std::string_view path)
{
    return PakInternal::NormalizePathSeparators(std::string(path));
}

PakFileHandle PakReader::FindInTable(const RuntimeTable& table, std::string_view filename)
{
    if (filename.empty()) return {};

    if (NeedsPathNormalization(filename)) {
        std::string normalized = NormalizePath(filename);
        auto it = table.indexByName.find(normalized);
        return it != table.indexByName.end() ? PakFileHandle{it->second} : PakFileHandle{};
    }

    auto it = table.indexByName.find(filename);
    return it != table.indexByName.end() ? PakFileHandle{it->second} : PakFileHandle{};
}

PakFileHandle PakReader::Find(std::string_view filename) const
{
    std::shared_ptr<const RuntimeTable> table;
    {
        std::shared_lock lock(mutex_);
        if (!isOpen_ || !table_) return {};
        table = table_;
    }
    return FindInTable(*table, filename);
}

size_t PakReader::Resolve(std::span<const std::string_view> filenames,
                          std::span<PakFileHandle> handles) const
{
    size_t resolvedCount = 0;
    size_t count = std::min(filenames.size(), handles.size());

    std::shared_ptr<const RuntimeTable> table;
    {
        std::shared_lock lock(mutex_);
        if (!isOpen_ || !table_) {
            for (size_t i = 0; i < count; ++i) handles[i] = {};
            return 0;
        }
        table = table_;
    }

    for (size_t i = 0; i < count; ++i) {
        handles[i] = FindInTable(*table, filenames[i]);
        if (handles[i]) ++resolvedCount;
    }
    return resolvedCount;
}

const PakFileInfo* PakReader::Info(PakFileHandle handle) const
{
    std::shared_lock lock(mutex_);
    if (!isOpen_ || !table_ || !handle || handle.index >= table_->infos.size()) return nullptr;
    return &table_->infos[handle.index];
}

// ---------------------------------------------------------------------------
// Read helpers
// ---------------------------------------------------------------------------

PakStatus PakReader::CaptureReadContext(PakFileHandle handle, ReadContext& context) const
{
    std::shared_lock lock(mutex_);
    if (!isOpen_ || !table_) return PakStatus::NotOpen;
    if (!handle || handle.index >= table_->entries.size()) return PakStatus::InvalidHandle;

    context.table = table_;
    context.guard = mappedGuard_;
    context.useMmap = useMmap_;
    context.fileSize = pakFileSize_;
    return PakStatus::Ok;
}

PakStatus PakReader::ReadEntryToBuffer(const PakInternal::PakEntry& entry,
    std::span<uint8_t> destination, uint64_t* bytesWritten,
    const ReadContext& context) const
{
    if (bytesWritten) *bytesWritten = 0;

    const uint64_t diskSize = entry.compressedSize;
    const bool compressed = (entry.flags & PakInternal::PAK_FLAG_COMPRESSED) != 0;

    if (entry.offset > context.fileSize || diskSize > context.fileSize - entry.offset) {
        Log(PakLogLevel::Error, "PakReader::Read: Entry exceeds file bounds: " + entry.filename);
        return PakStatus::CorruptArchive;
    }

    if (!compressed && diskSize != entry.originalSize) {
        Log(PakLogLevel::Error, "PakReader::Read: Uncompressed entry has mismatched disk size: " + entry.filename);
        return PakStatus::CorruptArchive;
    }

    if (entry.originalSize > static_cast<uint64_t>(destination.size())) {
        return PakStatus::BufferTooSmall;
    }

    if (entry.originalSize == 0) {
        return PakStatus::Ok;
    }

    if (compressed && (entry.originalSize > LZ4_MAX_SAFE_SIZE || diskSize > LZ4_MAX_SAFE_SIZE)) {
        Log(PakLogLevel::Error, "PakReader::Read: Entry exceeds LZ4 size limit: " + entry.filename);
        return PakStatus::CorruptArchive;
    }

    auto output = destination.first(static_cast<size_t>(entry.originalSize));

    const uint8_t* mappedPtr = nullptr;
    if (context.useMmap && context.guard && context.guard->mf.data) {
        mappedPtr = static_cast<const uint8_t*>(context.guard->mf.data) + entry.offset;
    }

    if (!compressed) {
        if (mappedPtr) {
            std::memcpy(output.data(), mappedPtr, output.size());
        } else {
            if (diskSize > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)())) {
                return PakStatus::IoError;
            }
            std::lock_guard streamLock(streamMutex_);
            pakStream_.clear();
            pakStream_.seekg(entry.offset, std::ios::beg);
            if (!pakStream_) return PakStatus::IoError;
            pakStream_.read(reinterpret_cast<char*>(output.data()),
                            static_cast<std::streamsize>(diskSize));
            if (!pakStream_) return PakStatus::IoError;
        }

        EncryptDecryptSpan(output, encryptionKey_);
        if (bytesWritten) *bytesWritten = entry.originalSize;
        return PakStatus::Ok;
    }

    const uint8_t* compressedData = mappedPtr;
    std::vector<uint8_t> compressedScratch;
    if (!compressedData || !encryptionKey_.empty()) {
        if (diskSize > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)())) {
            return PakStatus::IoError;
        }

        compressedScratch.resize(static_cast<size_t>(diskSize));
        if (mappedPtr) {
            std::memcpy(compressedScratch.data(), mappedPtr, compressedScratch.size());
        } else {
            std::lock_guard streamLock(streamMutex_);
            pakStream_.clear();
            pakStream_.seekg(entry.offset, std::ios::beg);
            if (!pakStream_) return PakStatus::IoError;
            pakStream_.read(reinterpret_cast<char*>(compressedScratch.data()),
                            static_cast<std::streamsize>(diskSize));
            if (!pakStream_) return PakStatus::IoError;
        }

        EncryptDecryptSpan(compressedScratch, encryptionKey_);
        compressedData = compressedScratch.data();
    }

    int result = LZ4_decompress_safe(
        reinterpret_cast<const char*>(compressedData),
        reinterpret_cast<char*>(output.data()),
        static_cast<int>(diskSize),
        static_cast<int>(entry.originalSize));
    if (result < 0 || static_cast<uint64_t>(result) != entry.originalSize) {
        Log(PakLogLevel::Error, "PakReader::Read: Decompression failed for: " + entry.filename);
        return PakStatus::DecompressionFailed;
    }

    if (bytesWritten) *bytesWritten = entry.originalSize;
    return PakStatus::Ok;
}

// ---------------------------------------------------------------------------
// Engine read API
// ---------------------------------------------------------------------------

PakStatus PakReader::View(PakFileHandle handle, PakView& outView) const
{
    outView = PakView{};

    ReadContext context;
    PakStatus status = CaptureReadContext(handle, context);
    if (status != PakStatus::Ok) return status;

    const auto& entry = context.table->entries[handle.index];
    if ((entry.flags & PakInternal::PAK_FLAG_COMPRESSED) || !encryptionKey_.empty()) {
        return PakStatus::Unsupported;
    }
    if (!context.useMmap || !context.guard || !context.guard->mf.data) {
        return PakStatus::Unsupported;
    }
    if (entry.offset > context.fileSize || entry.originalSize > context.fileSize - entry.offset) {
        return PakStatus::CorruptArchive;
    }

    outView.data = static_cast<const uint8_t*>(context.guard->mf.data) + entry.offset;
    outView.size = entry.originalSize;
    outView.mapped = true;
    outView.mappingRef_ = std::move(context.guard);
    return PakStatus::Ok;
}

PakStatus PakReader::Read(PakFileHandle handle, std::span<uint8_t> destination,
                          uint64_t* bytesWritten) const
{
    ReadContext context;
    PakStatus status = CaptureReadContext(handle, context);
    if (status != PakStatus::Ok) return status;

    const auto& entry = context.table->entries[handle.index];
    return ReadEntryToBuffer(entry, destination, bytesWritten, context);
}

PakStatus PakReader::Load(PakFileHandle handle, std::vector<uint8_t>& outData) const
{
    ReadContext context;
    PakStatus status = CaptureReadContext(handle, context);
    if (status != PakStatus::Ok) {
        outData.clear();
        return status;
    }

    const auto& entry = context.table->entries[handle.index];
    if (entry.originalSize > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
        outData.clear();
        return PakStatus::InvalidArgument;
    }

    outData.resize(static_cast<size_t>(entry.originalSize));
    status = ReadEntryToBuffer(entry, outData, nullptr, context);
    if (status != PakStatus::Ok) outData.clear();
    return status;
}

PakStatus PakReader::Prefetch(PakFileHandle handle) const
{
    ReadContext context;
    PakStatus status = CaptureReadContext(handle, context);
    if (status != PakStatus::Ok) return status;

    const auto& entry = context.table->entries[handle.index];
    if (entry.compressedSize == 0) return PakStatus::Ok;
    if (!context.useMmap || !context.guard || !context.guard->mf.data) {
        return PakStatus::Unsupported;
    }
    if (!PakPlatform::PrefetchMappedRange(context.guard->mf, entry.offset, entry.compressedSize)) {
        return PakStatus::IoError;
    }
    return PakStatus::Ok;
}

// ---------------------------------------------------------------------------
// Convenience wrappers
// ---------------------------------------------------------------------------

std::vector<uint8_t> PakReader::ReadFile(const std::string& filename) const
{
    PakFileHandle handle = Find(filename);
    if (!handle) {
        Log(PakLogLevel::Error, "PakReader::ReadFile: File not found: " + filename);
        return {};
    }

    std::vector<uint8_t> data;
    PakStatus status = Load(handle, data);
    if (status != PakStatus::Ok) {
        Log(PakLogLevel::Error, "PakReader::ReadFile: " + std::string(PakStatusToString(status)) +
            " for file: " + filename);
        return {};
    }
    return data;
}

std::shared_ptr<std::vector<uint8_t>> PakReader::LoadFile(const std::string& filename) const
{
    PakFileHandle handle = Find(filename);
    if (!handle) return nullptr;

    auto data = std::make_shared<std::vector<uint8_t>>();
    PakStatus status = Load(handle, *data);
    return status == PakStatus::Ok ? data : nullptr;
}

PakSpan PakReader::ReadFileZeroCopy(const std::string& filename) const
{
    PakSpan span;
    PakFileHandle handle = Find(filename);
    if (!handle) {
        Log(PakLogLevel::Error, "PakReader::ReadFileZeroCopy: File not found: " + filename);
        return span;
    }

    PakView view;
    PakStatus status = View(handle, view);
    if (status == PakStatus::Ok) {
        span.data = view.data;
        span.size = view.size;
        span.ownsData = false;
        span.mappingRef_ = std::move(view.mappingRef_);
        return span;
    }

    std::vector<uint8_t> data;
    status = Load(handle, data);
    if (status != PakStatus::Ok) return span;

    if (data.empty()) {
        span.data = nullptr;
        span.size = 0;
        span.ownsData = true;
        return span;
    }

    auto* buffer = new uint8_t[data.size()];
    std::memcpy(buffer, data.data(), data.size());
    span.data = buffer;
    span.size = data.size();
    span.ownsData = true;
    return span;
}

bool PakReader::FileExists(std::string_view filename) const
{
    return static_cast<bool>(Find(filename));
}

uint32_t PakReader::GetFileCount() const
{
    std::shared_lock lock(mutex_);
    if (!isOpen_ || !table_) return 0;
    return static_cast<uint32_t>(table_->entries.size());
}

PakReader::FileInfo PakReader::GetFileInfo(const std::string& filename) const
{
    std::shared_lock lock(mutex_);
    FileInfo info{};
    info.found = false;
    info.compressed = false;
    info.originalSize = 0;
    info.compressedSize = 0;

    if (!isOpen_ || !table_) return info;

    PakFileHandle handle = FindInTable(*table_, filename);
    if (handle) {
        const PakFileInfo& runtimeInfo = table_->infos[handle.index];
        info.filename = std::string(runtimeInfo.filename);
        info.originalSize = runtimeInfo.originalSize;
        info.compressedSize = runtimeInfo.compressedSize;
        info.compressed = runtimeInfo.compressed;
        info.found = true;
    }
    return info;
}

// ---------------------------------------------------------------------------
// Batch read wrapper
// ---------------------------------------------------------------------------

std::vector<std::pair<std::string, std::vector<uint8_t>>>
PakReader::ReadFiles(const std::vector<std::string>& filenames) const
{
    std::vector<std::pair<std::string, std::vector<uint8_t>>> results;
    results.resize(filenames.size());

    for (size_t i = 0; i < filenames.size(); ++i) {
        results[i].first = filenames[i];
        PakFileHandle handle = Find(filenames[i]);
        if (handle) {
            Load(handle, results[i].second);
        }
    }

    return results;
}
