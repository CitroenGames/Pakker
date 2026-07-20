#include "Pak.h"
#include <algorithm>
#include <atomic>
#include <cstring>

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
        case PakStatus::HashMismatch: return "HashMismatch";
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
    if (header.version != PAK_VERSION_6) {
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

        uint64_t contentHash = 0;
        stream.read(reinterpret_cast<char*>(&contentHash), sizeof(contentHash));
        if (!stream) {
            Log(PakLogLevel::Error, "ReadFileTable: Failed to read content hash for: " + filename);
            return false;
        }

        PakEntry entry(std::move(filename), offset, originalSize, compressedSize, flags, contentHash);
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
        stream.write(reinterpret_cast<const char*>(&entry.contentHash), sizeof(entry.contentHash));

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
    size_t keyIndex = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] ^= static_cast<uint8_t>(key[keyIndex]);
        if (++keyIndex == keyLength) keyIndex = 0;
    }
}

} // namespace PakInternal
