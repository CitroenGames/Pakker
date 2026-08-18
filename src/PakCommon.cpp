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

size_t ThreadShardIndex() noexcept
{
    static std::atomic<size_t> nextIndex{0};
    // Assigned once per thread and cached, so the hot path pays a thread-local
    // read rather than an atomic.
    thread_local const size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
    return index;
}

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
    if (header.version != PAK_VERSION_7) {
        Log(PakLogLevel::Error, "ReadPakHeader: Unsupported PAK version: " + std::to_string(header.version));
        return false;
    }
    if (header.numFiles > MAX_FILES_IN_PAK) {
        Log(PakLogLevel::Error, "ReadPakHeader: Too many files in PAK: " + std::to_string(header.numFiles));
        return false;
    }
    if (header.nameBlobSize > MAX_NAME_BLOB_SIZE) {
        Log(PakLogLevel::Error, "ReadPakHeader: Name blob too large: " +
            std::to_string(header.nameBlobSize));
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

bool ReadFileTable(std::istream& stream, const PakHeader& header,
                   std::vector<PakEntry>& entries)
{
    entries.clear();

    const uint32_t numFiles = header.numFiles;
    if (numFiles == 0) return true;

    std::vector<PakEntryRecord> records(numFiles);
    stream.seekg(static_cast<std::streamoff>(header.fileTableOffset), std::ios::beg);
    if (!stream) {
        Log(PakLogLevel::Error, "ReadFileTable: Failed to seek to file table.");
        return false;
    }
    stream.read(reinterpret_cast<char*>(records.data()),
                static_cast<std::streamsize>(records.size() * sizeof(PakEntryRecord)));
    if (!stream) {
        Log(PakLogLevel::Error, "ReadFileTable: Failed to read entry records.");
        return false;
    }

    std::vector<char> nameBlob(static_cast<size_t>(header.nameBlobSize));
    if (!nameBlob.empty()) {
        stream.seekg(static_cast<std::streamoff>(header.nameBlobOffset), std::ios::beg);
        if (!stream) {
            Log(PakLogLevel::Error, "ReadFileTable: Failed to seek to name blob.");
            return false;
        }
        stream.read(nameBlob.data(), static_cast<std::streamsize>(nameBlob.size()));
        if (!stream) {
            Log(PakLogLevel::Error, "ReadFileTable: Failed to read name blob.");
            return false;
        }
    }

    entries.reserve(numFiles);
    for (const PakEntryRecord& record : records) {
        if (record.nameLength == 0 ||
            record.nameOffset > nameBlob.size() ||
            record.nameLength > nameBlob.size() - record.nameOffset) {
            Log(PakLogLevel::Error, "ReadFileTable: Name range outside the name blob.");
            return false;
        }

        std::string filename(nameBlob.data() + record.nameOffset, record.nameLength);
        if (!IsValidFilename(filename)) {
            Log(PakLogLevel::Error, "ReadFileTable: Invalid filename: " + filename);
            return false;
        }

        entries.emplace_back(std::move(filename), record.offset, record.originalSize,
                             record.compressedSize, record.flags, record.contentHash,
                             record.pathHash);
    }
    return true;
}

bool WriteFileTable(std::ostream& stream, const std::vector<PakEntry>& entries,
                    PakHeader& header)
{
    uint64_t nameBlobSize = 0;
    for (const auto& entry : entries) {
        if (entry.filename.length() > MAX_FILENAME_LENGTH) {
            Log(PakLogLevel::Error, "WriteFileTable: Filename too long: " + entry.filename);
            return false;
        }
        nameBlobSize += entry.filename.size();
    }
    if (nameBlobSize > MAX_NAME_BLOB_SIZE) {
        Log(PakLogLevel::Error, "WriteFileTable: Name blob too large: " +
            std::to_string(nameBlobSize));
        return false;
    }

    const uint64_t tableOffset = SafeStreamPos(stream, stream.tellp());
    if (!stream) {
        Log(PakLogLevel::Error, "WriteFileTable: Failed to get file table offset.");
        return false;
    }

    // Records first, then every name back to back. The reader gets both in
    // one bulk read each and never parses entry by entry.
    std::vector<PakEntryRecord> records;
    records.reserve(entries.size());

    uint32_t nameOffset = 0;
    for (const auto& entry : entries) {
        PakEntryRecord record{};
        record.offset = entry.offset;
        record.originalSize = entry.originalSize;
        record.compressedSize = entry.compressedSize;
        record.contentHash = entry.contentHash;
        record.pathHash = entry.pathHash;
        record.nameOffset = nameOffset;
        record.nameLength = static_cast<uint16_t>(entry.filename.size());
        record.flags = entry.flags;
        records.push_back(record);
        nameOffset += static_cast<uint32_t>(entry.filename.size());
    }

    if (!records.empty()) {
        stream.write(reinterpret_cast<const char*>(records.data()),
                     static_cast<std::streamsize>(records.size() * sizeof(PakEntryRecord)));
        if (!stream) {
            Log(PakLogLevel::Error, "WriteFileTable: Failed to write entry records.");
            return false;
        }
    }

    const uint64_t blobOffset = SafeStreamPos(stream, stream.tellp());
    if (!stream) {
        Log(PakLogLevel::Error, "WriteFileTable: Failed to get name blob offset.");
        return false;
    }

    for (const auto& entry : entries) {
        if (entry.filename.empty()) continue;
        stream.write(entry.filename.data(), static_cast<std::streamsize>(entry.filename.size()));
        if (!stream) {
            Log(PakLogLevel::Error, "WriteFileTable: Failed to write name blob.");
            return false;
        }
    }

    header.fileTableOffset = tableOffset;
    header.nameBlobOffset = blobOffset;
    header.nameBlobSize = nameBlobSize;
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
