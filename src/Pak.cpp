#include "Pak.h"
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_set>

#include "vendor/lz4.h"

namespace fs = std::filesystem;

// ===========================================================================
// Global logging
// ===========================================================================

static PakLogCallback g_logCallback = nullptr;

void PakSetLogCallback(PakLogCallback cb) { g_logCallback = cb; }

namespace PakInternal {

void Log(PakLogLevel level, const std::string& msg)
{
    if (g_logCallback) {
        g_logCallback(level, msg.c_str());
    }
}

// ===========================================================================
// Shared utility functions
// ===========================================================================

std::string NormalizePathSeparators(const std::string& path)
{
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    while (!normalized.empty() && normalized[0] == '/') {
        normalized.erase(0, 1);
    }
    return normalized;
}

bool IsValidFilename(const std::string& filename)
{
    if (filename.empty() || filename.length() > MAX_FILENAME_LENGTH) return false;
    if (filename.find("..") != std::string::npos) return false;
    if (filename.find('\0') != std::string::npos) return false;
    const std::string invalidChars = "<>:\"|?*";
    for (char c : invalidChars) {
        if (filename.find(c) != std::string::npos) return false;
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
        entry.offset + diskSize > pakFileSize) {
        return false;
    }
    return IsValidFilename(entry.filename);
}

// ---------------------------------------------------------------------------
// Header I/O
// ---------------------------------------------------------------------------

bool ReadPakHeader(std::istream& stream, PakHeader& header)
{
    std::memset(&header, 0, sizeof(header));
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!stream) {
        Log(PakLogLevel::Error, "ReadPakHeader: Failed to read PAK header.");
        return false;
    }
    if (std::memcmp(header.magic, PAK_MAGIC.data(), 4) != 0) {
        Log(PakLogLevel::Error, "ReadPakHeader: Invalid magic number.");
        return false;
    }
    if (header.version != PAK_VERSION_1 && header.version != PAK_VERSION_2) {
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

// Returns true for file extensions known to be already compressed.
// Skipping LZ4 on these avoids wasted CPU time (the result is always discarded).
static bool IsLikelyPreCompressed(const std::string& filename)
{
    auto dotPos = filename.rfind('.');
    if (dotPos == std::string::npos) return false;

    std::string ext = filename.substr(dotPos);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    static const std::unordered_set<std::string> compressedExts = {
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".avif",
        ".mp3", ".ogg", ".flac", ".aac", ".wma", ".opus",
        ".mp4", ".webm", ".mkv",
        ".zip", ".gz", ".7z", ".rar", ".bz2", ".xz", ".zst",
        ".pak", ".lz4",
    };

    return compressedExts.count(ext) > 0;
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
    if (buffer.empty()) {
        Log(PakLogLevel::Error, "WriteFile: Empty buffer for file: " + filename);
        return false;
    }
    std::ofstream fileStream(filename, std::ios::binary);
    if (!fileStream) {
        Log(PakLogLevel::Error, "WriteFile: Unable to create file: " + filename);
        return false;
    }
    fileStream.write(reinterpret_cast<const char*>(buffer.data()),
                     static_cast<std::streamsize>(buffer.size()));
    if (!fileStream) {
        Log(PakLogLevel::Error, "WriteFile: Failed to write data to file: " + filename);
        return false;
    }
    return true;
}

bool Pakker::CreatePak(const std::string& pakFilename,
                       const std::map<std::string, std::vector<uint8_t>>& files,
                       bool compress)
{
    if (files.size() > MAX_FILES_IN_PAK) {
        Log(PakLogLevel::Error, "CreatePak: Too many files to pack: " + std::to_string(files.size()));
        return false;
    }

    std::ofstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        Log(PakLogLevel::Error, "CreatePak: Unable to create pak file: " + pakFilename);
        return false;
    }

    PakHeader header;
    header.version = PAK_VERSION_2;
    header.numFiles = static_cast<uint32_t>(files.size());
    header.fileTableOffset = 0;

    if (!WritePakHeader(pakStream, header)) return false;

    std::vector<PakEntry> entries;
    entries.reserve(files.size());

    std::vector<uint8_t> compressBuffer;
    std::vector<uint8_t> encryptBuffer;

    for (const auto& [filename, data] : files) {
        std::string normalizedFilename = NormalizePathSeparators(filename);
        if (!IsValidFilename(normalizedFilename)) {
            Log(PakLogLevel::Error, "CreatePak: Invalid filename: " + filename);
            return false;
        }

        const uint8_t* writePtr = data.data();
        size_t writeSize = data.size();
        uint8_t flags = 0;
        uint64_t originalSize = data.size();

        if (compress && !data.empty() && !IsLikelyPreCompressed(normalizedFilename)) {
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

        // Encryption requires a mutable buffer; only copy when actually encrypting
        if (!encryptionKey_.empty()) {
            encryptBuffer.assign(writePtr, writePtr + writeSize);
            EncryptDecrypt(encryptBuffer, encryptionKey_);
            writePtr = encryptBuffer.data();
            writeSize = encryptBuffer.size();
        }

        uint64_t currentOffset = SafeStreamPos(pakStream.tellp());
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
    if (!WriteFileTable(pakStream, entries, PAK_VERSION_2)) return false;

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
        if (sanitizedOutputPath.string().find(sanitizedOutputDir.string()) != 0) {
            Log(PakLogLevel::Error, "ExtractPak: Detected invalid file path: " + entry.filename);
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
    auto data = ReadFileFromPak(pakFilename, filename);
    if (data.empty()) return nullptr;
    return std::make_shared<std::vector<uint8_t>>(std::move(data));
}

bool Pakker::AddFileToPak(const std::string& pakFilename,
                          const std::string& filename,
                          const std::vector<uint8_t>& data)
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

    pakStream.seekp(0, std::ios::end);
    if (!pakStream) return false;

    uint64_t newOffset = SafeStreamPos(pakStream.tellp());
    std::vector<uint8_t> encryptedData = data;
    EncryptDecrypt(encryptedData, encryptionKey_);

    pakStream.write(reinterpret_cast<const char*>(encryptedData.data()),
                   static_cast<std::streamsize>(encryptedData.size()));
    if (!pakStream) return false;

    uint64_t originalSize = data.size();
    uint64_t compressedSize = encryptedData.size();
    entries.emplace_back(normalizedFilename, newOffset, originalSize, compressedSize, 0);
    header.numFiles += 1;
    header.fileTableOffset = SafeStreamPos(pakStream.tellp());

    pakStream.seekp(0, std::ios::beg);
    if (!WritePakHeader(pakStream, header)) return false;

    pakStream.seekp(header.fileTableOffset, std::ios::beg);
    if (!WriteFileTable(pakStream, entries, header.version)) return false;

    Log(PakLogLevel::Info, "AddFileToPak: File '" + filename + "' added successfully.");
    return true;
}

bool Pakker::CreatePakFromFolder(const std::string& pakFilename,
                                 const std::string& folderPath,
                                 bool compress)
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

    std::ofstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        Log(PakLogLevel::Error, "CreatePakFromFolder: Unable to create pak file: " + pakFilename);
        return false;
    }

    PakHeader header;
    header.version = PAK_VERSION_2;
    header.numFiles = static_cast<uint32_t>(filePaths.size());
    header.fileTableOffset = 0;

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

        const uint8_t* writePtr = data.data();
        size_t writeSize = data.size();
        uint8_t flags = 0;
        uint64_t originalSize = data.size();

        if (compress && !data.empty() && !IsLikelyPreCompressed(normalizedName)) {
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

        // Encrypt in-place on the buffer writePtr already points to
        if (!encryptionKey_.empty()) {
            if (writePtr == data.data()) {
                EncryptDecrypt(data, encryptionKey_);
            } else {
                EncryptDecrypt(compressBuffer, encryptionKey_);
            }
            // writePtr remains valid — EncryptDecrypt does not resize
        }

        uint64_t currentOffset = SafeStreamPos(pakStream.tellp());
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
    header.numFiles = static_cast<uint32_t>(entries.size());
    if (!WriteFileTable(pakStream, entries, PAK_VERSION_2)) return false;

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
    std::vector<uint8_t> fileData = ReadFileFromPak(pakFilename, filename);
    if (fileData.empty()) return false;
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
    : encryptionKey_(std::move(other.encryptionKey_)),
      pakStream_(std::move(other.pakStream_)),
      header_(other.header_),
      pakFileSize_(other.pakFileSize_),
      formatVersion_(other.formatVersion_),
      isOpen_(other.isOpen_),
      fileTable_(std::move(other.fileTable_))
{
    other.isOpen_ = false;
    other.pakFileSize_ = 0;
    other.formatVersion_ = 0;
}

PakReader& PakReader::operator=(PakReader&& other) noexcept
{
    if (this != &other) {
        Close();
        encryptionKey_ = std::move(other.encryptionKey_);
        pakStream_ = std::move(other.pakStream_);
        header_ = other.header_;
        pakFileSize_ = other.pakFileSize_;
        formatVersion_ = other.formatVersion_;
        isOpen_ = other.isOpen_;
        fileTable_ = std::move(other.fileTable_);
        other.isOpen_ = false;
        other.pakFileSize_ = 0;
        other.formatVersion_ = 0;
    }
    return *this;
}

bool PakReader::Open(const std::string& pakFilename)
{
    Close();

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

    isOpen_ = true;
    Log(PakLogLevel::Info, "PakReader::Open: Opened '" + pakFilename + "' with " +
        std::to_string(header_.numFiles) + " files.");
    return true;
}

void PakReader::Close()
{
    if (isOpen_) {
        pakStream_.close();
        fileTable_.clear();
        header_ = PakHeader{};
        pakFileSize_ = 0;
        formatVersion_ = 0;
        isOpen_ = false;
    }
}

bool PakReader::IsOpen() const { return isOpen_; }

std::vector<uint8_t> PakReader::ReadEntry(const PakEntry& entry) const
{
    pakStream_.seekg(entry.offset, std::ios::beg);
    if (!pakStream_) {
        Log(PakLogLevel::Error, "PakReader: Failed to seek for file: " + entry.filename);
        return {};
    }

    uint64_t diskSize = entry.compressedSize;
    std::vector<uint8_t> rawData(diskSize);
    pakStream_.read(reinterpret_cast<char*>(rawData.data()),
                    static_cast<std::streamsize>(diskSize));
    if (!pakStream_) {
        Log(PakLogLevel::Error, "PakReader: Failed to read file: " + entry.filename);
        return {};
    }

    EncryptDecrypt(rawData, encryptionKey_);

    if (entry.flags & PAK_FLAG_COMPRESSED) {
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

std::vector<uint8_t> PakReader::ReadFile(const std::string& filename) const
{
    if (!isOpen_) {
        Log(PakLogLevel::Error, "PakReader::ReadFile: No PAK file is open.");
        return {};
    }

    std::string normalized = NormalizePathSeparators(filename);
    auto it = fileTable_.find(normalized);
    if (it == fileTable_.end()) {
        Log(PakLogLevel::Error, "PakReader::ReadFile: File not found: " + filename);
        return {};
    }

    return ReadEntry(it->second);
}

std::shared_ptr<std::vector<uint8_t>> PakReader::LoadFile(const std::string& filename) const
{
    auto data = ReadFile(filename);
    if (data.empty()) return nullptr;
    return std::make_shared<std::vector<uint8_t>>(std::move(data));
}

bool PakReader::FileExists(const std::string& filename) const
{
    if (!isOpen_) return false;
    std::string normalized = NormalizePathSeparators(filename);
    return fileTable_.count(normalized) > 0;
}

uint32_t PakReader::GetFileCount() const
{
    if (!isOpen_) return 0;
    return static_cast<uint32_t>(fileTable_.size());
}

PakReader::FileInfo PakReader::GetFileInfo(const std::string& filename) const
{
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

std::vector<std::pair<std::string, std::vector<uint8_t>>>
PakReader::ReadFiles(const std::vector<std::string>& filenames) const
{
    std::vector<std::pair<std::string, std::vector<uint8_t>>> results;
    if (!isOpen_ || filenames.empty()) return results;

    // Resolve all entries and remember their original index
    struct ResolvedEntry {
        size_t originalIndex;
        const PakEntry* entry;
    };
    std::vector<ResolvedEntry> resolved;
    resolved.reserve(filenames.size());
    results.resize(filenames.size());

    for (size_t i = 0; i < filenames.size(); ++i) {
        std::string normalized = NormalizePathSeparators(filenames[i]);
        auto it = fileTable_.find(normalized);
        if (it != fileTable_.end()) {
            resolved.push_back({i, &it->second});
        }
        results[i].first = filenames[i];
    }

    // Sort by offset for sequential I/O
    std::sort(resolved.begin(), resolved.end(),
              [](const ResolvedEntry& a, const ResolvedEntry& b) {
                  return a.entry->offset < b.entry->offset;
              });

    // Read in offset order
    for (const auto& r : resolved) {
        results[r.originalIndex].second = ReadEntry(*r.entry);
    }

    return results;
}
