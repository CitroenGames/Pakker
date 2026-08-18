#include "Pak.h"
#include "PakInternal.h"
#include "PakCompression.h"
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <thread>
#include <unordered_set>
#include <sstream>
#include <tuple>

namespace fs = std::filesystem;

using namespace PakInternal;

// ---------------------------------------------------------------------------
// Parallel encode pipeline
//
// Compression dominates pack time -- at the default zstd level 19 it is
// roughly three orders of magnitude slower than the I/O around it -- and it
// is per-entry independent, so it parallelizes cleanly. Encoding runs on a
// worker pool while writing stays strictly sequential in input order, which
// keeps archives byte-identical no matter how many workers ran.
//
// Memory is bounded by processing entries in batches: a batch is capped both
// by entry count and by total source bytes, so a handful of very large assets
// cannot balloon the working set.
// ---------------------------------------------------------------------------

namespace {

constexpr size_t kMaxBatchEntries = 4096;
constexpr uint64_t kMaxBatchBytes = 128ull * 1024 * 1024;

// One entry's encode result: the exact bytes to write plus what the file
// table needs to record about them.
struct EncodedEntry {
    // Holds the transformed bytes when compression or encryption produced
    // any; for a stored, unencrypted in-memory entry the caller's buffer is
    // referenced directly instead.
    std::vector<uint8_t> storage;
    const uint8_t* sourceBytes = nullptr;
    uint64_t sourceSize = 0;
    bool usesStorage = false;

    uint64_t originalSize = 0;
    uint64_t contentHash = 0;
    uint8_t flags = 0;
    uint8_t chunkSizeLog2 = 0;

    bool ok = false;       // false: skip this entry (unreadable source file)
    bool fatal = false;    // true: abort the whole build

    const uint8_t* Data() const { return usesStorage ? storage.data() : sourceBytes; }
    uint64_t Size() const
    {
        return usesStorage ? static_cast<uint64_t>(storage.size()) : sourceSize;
    }
};

// Rejects a chunk size the format cannot represent. 0 means chunking is off.
bool ValidateChunkSize(uint32_t chunkSize, const char* context)
{
    if (chunkSize == 0) return true;
    if (chunkSize < MIN_COMPRESSION_CHUNK_SIZE || chunkSize > MAX_COMPRESSION_CHUNK_SIZE ||
        (chunkSize & (chunkSize - 1)) != 0) {
        Log(PakLogLevel::Error, std::string(context) +
            ": compressionChunkSize must be a power of two between 4 KiB and 16 MiB.");
        return false;
    }
    return true;
}

uint32_t ResolveWorkerCount(uint32_t requested)
{
    if (requested > 0) return requested;
    const unsigned hardware = std::thread::hardware_concurrency();
    return hardware > 0 ? hardware : 1u;
}

// Runs body(i) for every i in [0, count). Workers pull indices off a shared
// counter so uneven entry sizes self-balance. The calling thread takes part
// rather than idling.
template <typename Body>
void ParallelFor(size_t count, uint32_t workerCount, Body&& body)
{
    if (count == 0) return;
    if (workerCount <= 1 || count == 1) {
        for (size_t i = 0; i < count; ++i) body(i);
        return;
    }

    std::atomic<size_t> next{0};
    auto run = [&]() {
        for (;;) {
            const size_t index = next.fetch_add(1, std::memory_order_relaxed);
            if (index >= count) break;
            body(index);
        }
    };

    const uint32_t spawn = static_cast<uint32_t>(
        (std::min)(static_cast<size_t>(workerCount), count)) - 1;
    std::vector<std::thread> workers;
    workers.reserve(spawn);
    for (uint32_t i = 0; i < spawn; ++i) workers.emplace_back(run);
    run();
    for (auto& worker : workers) worker.join();
}

// Compresses, encrypts and hashes one entry.
//
// When `source` is null the bytes are already sitting in out.storage (the
// folder builder reads straight into it), which avoids a copy of every asset.
void EncodeEntry(const uint8_t* source, uint64_t sourceSize,
                 const PakOptions& options, const std::string& encryptionKey,
                 std::vector<uint8_t>& scratch, EncodedEntry& out)
{
    const bool sourceIsExternal = source != nullptr;
    if (!sourceIsExternal) {
        source = out.storage.data();
        sourceSize = static_cast<uint64_t>(out.storage.size());
        out.usesStorage = true;
    } else {
        out.sourceBytes = source;
        out.sourceSize = sourceSize;
    }
    out.originalSize = sourceSize;

    if (options.compression != PakCompression::None && sourceSize > 0) {
        scratch.clear();
        // Chunk only entries actually bigger than one chunk. Below that a
        // chunked payload is just a whole-entry frame plus a table header, so
        // it costs ratio for nothing.
        const bool chunked = options.compressionChunkSize > 0 &&
                             sourceSize > options.compressionChunkSize;
        const bool ok = chunked
            ? CompressChunked(options.compression, options.zstdLevel,
                              options.compressionChunkSize, source, sourceSize,
                              scratch, out.flags)
            : CompressBuffer(options.compression, options.zstdLevel,
                             source, sourceSize, scratch, out.flags);
        if (!ok) {
            out.fatal = true;
            return;
        }
        if (out.flags != 0) {
            out.storage.swap(scratch);
            out.usesStorage = true;
            if (chunked) {
                uint32_t log2 = 0;
                while ((1u << log2) < options.compressionChunkSize) ++log2;
                out.chunkSizeLog2 = static_cast<uint8_t>(log2);
            }
        }
    }

    if (!encryptionKey.empty()) {
        if (!out.usesStorage) {
            out.storage.assign(source, source + sourceSize);
            out.usesStorage = true;
        }
        EncryptDecrypt(out.storage, encryptionKey);
    }

    out.contentHash = HashBytesFast(out.Data(), out.Size());
    out.ok = true;
}

} // namespace

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
    if (!ValidateChunkSize(options.compressionChunkSize, "CreatePak")) {
        return false;
    }

    // Validate and normalize every name up front, in the map's already-sorted
    // order, so the encode stage below can be pure and order-independent.
    std::vector<std::string> names;
    std::vector<const std::vector<uint8_t>*> payloads;
    names.reserve(files.size());
    payloads.reserve(files.size());
    {
        std::unordered_set<std::string> seenNames;
        for (const auto& [filename, data] : files) {
            std::string normalizedFilename = NormalizePathSeparators(filename);
            if (!IsValidFilename(normalizedFilename)) {
                Log(PakLogLevel::Error, "CreatePak: Invalid filename: " + filename);
                return false;
            }
            if (!seenNames.insert(normalizedFilename).second) {
                Log(PakLogLevel::Error,
                    "CreatePak: Duplicate filename after normalization: " + normalizedFilename);
                return false;
            }
            names.push_back(std::move(normalizedFilename));
            payloads.push_back(&data);
        }
    }

    std::ofstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        Log(PakLogLevel::Error, "CreatePak: Unable to create pak file: " + pakFilename);
        return false;
    }

    PakHeader header;
    header.version = PAK_VERSION_8;
    header.numFiles = static_cast<uint32_t>(files.size());
    header.fileTableOffset = 0;
    header.alignment = alignment;

    if (!WritePakHeader(pakStream, header)) return false;

    std::vector<PakEntry> entries;
    entries.reserve(files.size());

    // With nothing to compress or encrypt there is no CPU work to spread --
    // the payloads are already in memory, so this degenerates to a copy and
    // spawning threads would only add latency.
    const bool needsEncoding =
        options.compression != PakCompression::None || !encryptionKey_.empty();
    const uint32_t workerCount = needsEncoding ? ResolveWorkerCount(options.workerThreads) : 1;

    std::vector<EncodedEntry> encoded;
    for (size_t batchStart = 0; batchStart < names.size(); ) {
        size_t batchEnd = batchStart;
        uint64_t batchBytes = 0;
        while (batchEnd < names.size() &&
               batchEnd - batchStart < kMaxBatchEntries &&
               (batchEnd == batchStart || batchBytes < kMaxBatchBytes)) {
            batchBytes += payloads[batchEnd]->size();
            ++batchEnd;
        }

        const size_t batchCount = batchEnd - batchStart;
        encoded.clear();
        encoded.resize(batchCount);

        ParallelFor(batchCount, workerCount, [&](size_t i) {
            // Reused across every entry this worker handles, so compression
            // does not reallocate its output buffer per file.
            thread_local std::vector<uint8_t> scratch;
            const std::vector<uint8_t>& payload = *payloads[batchStart + i];
            EncodeEntry(payload.data(), static_cast<uint64_t>(payload.size()),
                        options, encryptionKey_, scratch, encoded[i]);
        });

        for (size_t i = 0; i < batchCount; ++i) {
            const EncodedEntry& result = encoded[i];
            const std::string& name = names[batchStart + i];
            if (result.fatal) {
                Log(PakLogLevel::Error, "CreatePak: Compression failed for: " + name);
                return false;
            }

            if (!WritePadding(pakStream, alignment)) {
                Log(PakLogLevel::Error, "CreatePak: Failed to write alignment padding.");
                return false;
            }

            const uint64_t currentOffset = SafeStreamPos(pakStream, pakStream.tellp());
            if (!pakStream) {
                Log(PakLogLevel::Error, "CreatePak: Failed to get stream position.");
                return false;
            }

            entries.emplace_back(name, currentOffset, result.originalSize,
                                 result.Size(), result.flags, result.contentHash,
                                 PakPathHash(name));
            entries.back().chunkSizeLog2 = result.chunkSizeLog2;

            if (result.Size() > 0) {
                pakStream.write(reinterpret_cast<const char*>(result.Data()),
                                static_cast<std::streamsize>(result.Size()));
                if (!pakStream) {
                    Log(PakLogLevel::Error, "CreatePak: Failed to write data for file: " + name);
                    return false;
                }
            }
        }

        batchStart = batchEnd;
    }

    if (!WriteFileTable(pakStream, entries, header)) return false;

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


    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header, entries)) return false;

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

        if (IsCompressed(entry.flags)) {
            std::vector<uint8_t> decompressed(entry.originalSize);
            PakStatus status = DecompressBuffer(entry.flags, fileData.data(), fileData.size(),
                                                decompressed.data(), entry.originalSize);
            if (status != PakStatus::Ok) {
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


    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header, entries)) return false;

    Log(PakLogLevel::Info, "ListPak: Contents of '" + pakFilename + "':");
    for (const auto& entry : entries) {
        std::ostringstream oss;
        oss << " - " << entry.filename
            << " (Offset: " << entry.offset
            << ", Size: " << entry.originalSize << " bytes";
        if (IsCompressed(entry.flags)) {
            oss << ", Compressed (" << (entry.flags & PAK_FLAG_ZSTD_COMPRESSED ? "Zstd" : "LZ4")
                << "): " << entry.compressedSize << " bytes";
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


    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header, entries)) return {};

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

    if (!pakStream) return {};

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header, entries)) return {};

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

            if (IsCompressed(entry.flags)) {
                std::vector<uint8_t> decompressed(entry.originalSize);
                PakStatus status = DecompressBuffer(entry.flags, fileData.data(), fileData.size(),
                                                    decompressed.data(), entry.originalSize);
                if (status != PakStatus::Ok) {
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
                          PakCompression compression,
                          int zstdLevel)
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

    if (!pakStream) return false;

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header, entries)) return false;

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
    if (compression != PakCompression::None && !data.empty()) {
        if (!CompressBuffer(compression, zstdLevel, data.data(), data.size(), compressBuffer, flags)) {
            Log(PakLogLevel::Error, "AddFileToPak: Compression failed for: " + normalizedFilename);
            return false;
        }
        if (flags != 0) {
            writePtr = compressBuffer.data();
            writeSize = compressBuffer.size();
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

    uint64_t contentHash = HashBytesFast(writePtr, writeSize);
    entries.emplace_back(normalizedFilename, newOffset, originalSize,
                         static_cast<uint64_t>(writeSize), flags, contentHash,
                         PakPathHash(normalizedFilename));
    header.numFiles += 1;

    // The table goes immediately after the data just appended, and
    // WriteFileTable is what fills in the table and name-blob offsets -- so it
    // has to run before the header is rewritten with those offsets in it.
    if (!WriteFileTable(pakStream, entries, header)) return false;

    // Truncate any leftover bytes from the old file table before stamping the
    // header, so a short write can never leave a header pointing past the end.
    const uint64_t tableEnd = SafeStreamPos(pakStream, pakStream.tellp());

    pakStream.seekp(0, std::ios::beg);
    if (!WritePakHeader(pakStream, header)) return false;
    pakStream.seekp(static_cast<std::streamoff>(tableEnd), std::ios::beg);

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
    // Collect file paths first without loading contents into memory. Sizes
    // come along so batches below can be bounded by bytes as well as by entry
    // count -- a few very large assets would otherwise blow past the memory
    // budget on their own.
    struct SourceFile {
        std::string name;   // normalized, archive-relative
        fs::path diskPath;
        uint64_t size = 0;

        bool operator<(const SourceFile& other) const
        {
            return std::tie(name, diskPath) < std::tie(other.name, other.diskPath);
        }
    };

    std::vector<SourceFile> filePaths;
    try {
        for (const auto& entry : fs::recursive_directory_iterator(folderPath)) {
            if (fs::is_regular_file(entry.path())) {
                std::string relativePath = fs::relative(entry.path(), folderPath).string();
                relativePath = NormalizePathSeparators(relativePath);
                if (!IsValidFilename(relativePath)) {
                    Log(PakLogLevel::Warning, "CreatePakFromFolder: Skipping invalid filename: " + relativePath);
                    continue;
                }
                std::error_code sizeEc;
                const uint64_t size = static_cast<uint64_t>(entry.file_size(sizeEc));
                filePaths.push_back(SourceFile{std::move(relativePath), entry.path(),
                                               sizeEc ? 0 : size});
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
    if (!ValidateChunkSize(options.compressionChunkSize, "CreatePakFromFolder")) {
        return false;
    }

    std::ofstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        Log(PakLogLevel::Error, "CreatePakFromFolder: Unable to create pak file: " + pakFilename);
        return false;
    }

    PakHeader header;
    header.version = PAK_VERSION_8;
    header.numFiles = static_cast<uint32_t>(filePaths.size());
    header.fileTableOffset = 0;
    header.alignment = alignment;

    if (!WritePakHeader(pakStream, header)) return false;

    std::vector<PakEntry> entries;
    entries.reserve(filePaths.size());

    // Entries are read from disk and encoded on worker threads a batch at a
    // time, then written sequentially in the sorted order collected above, so
    // the archive stays byte-identical regardless of worker count. Only one
    // batch is resident at once, so peak memory tracks kMaxBatchBytes rather
    // than the size of the content tree.
    const uint32_t workerCount = ResolveWorkerCount(options.workerThreads);

    std::vector<EncodedEntry> encoded;
    for (size_t batchStart = 0; batchStart < filePaths.size(); ) {
        size_t batchEnd = batchStart;
        uint64_t batchBytes = 0;
        while (batchEnd < filePaths.size() &&
               batchEnd - batchStart < kMaxBatchEntries &&
               (batchEnd == batchStart || batchBytes < kMaxBatchBytes)) {
            batchBytes += filePaths[batchEnd].size;
            ++batchEnd;
        }
        const size_t batchCount = batchEnd - batchStart;

        encoded.clear();
        encoded.resize(batchCount);

        ParallelFor(batchCount, workerCount, [&](size_t i) {
            thread_local std::vector<uint8_t> scratch;
            EncodedEntry& result = encoded[i];
            const fs::path& diskPath = filePaths[batchStart + i].diskPath;

            // Bulk read: open at end to get size, then read in one call
            std::ifstream file(diskPath, std::ios::binary | std::ios::ate);
            if (!file) {
                Log(PakLogLevel::Warning,
                    "CreatePakFromFolder: Failed to open file: " + diskPath.string());
                return;
            }
            const auto fileSize = file.tellg();
            if (fileSize == std::streampos(-1)) {
                Log(PakLogLevel::Warning,
                    "CreatePakFromFolder: Failed to get file size: " + diskPath.string());
                return;
            }
            file.seekg(0, std::ios::beg);

            // Read straight into the result's storage so the bytes are never
            // copied just to hand them to the encoder.
            result.storage.resize(static_cast<size_t>(fileSize));
            if (fileSize > 0) {
                file.read(reinterpret_cast<char*>(result.storage.data()), fileSize);
                if (!file) {
                    Log(PakLogLevel::Warning,
                        "CreatePakFromFolder: Failed to read file: " + diskPath.string());
                    result.storage.clear();
                    return;
                }
            }
            file.close();

            EncodeEntry(nullptr, 0, options, encryptionKey_, scratch, result);
        });

        for (size_t i = 0; i < batchCount; ++i) {
            const EncodedEntry& result = encoded[i];
            const std::string& normalizedName = filePaths[batchStart + i].name;

            if (result.fatal) {
                Log(PakLogLevel::Error,
                    "CreatePakFromFolder: Compression failed for: " + normalizedName);
                return false;
            }
            // An unreadable source file is skipped, matching the previous
            // behavior; header.numFiles is set from the entries actually written.
            if (!result.ok) continue;

            if (!WritePadding(pakStream, alignment)) {
                Log(PakLogLevel::Error, "CreatePakFromFolder: Failed to write alignment padding.");
                return false;
            }

            const uint64_t currentOffset = SafeStreamPos(pakStream, pakStream.tellp());
            if (!pakStream) {
                Log(PakLogLevel::Error, "CreatePakFromFolder: Failed to get stream position.");
                return false;
            }

            entries.emplace_back(normalizedName, currentOffset, result.originalSize,
                                 result.Size(), result.flags, result.contentHash,
                                 PakPathHash(normalizedName));
            entries.back().chunkSizeLog2 = result.chunkSizeLog2;

            if (result.Size() > 0) {
                pakStream.write(reinterpret_cast<const char*>(result.Data()),
                                static_cast<std::streamsize>(result.Size()));
                if (!pakStream) {
                    Log(PakLogLevel::Error,
                        "CreatePakFromFolder: Failed to write data for file: " + normalizedName);
                    return false;
                }
            }
        }

        batchStart = batchEnd;
    }

    header.numFiles = static_cast<uint32_t>(entries.size());
    if (!WriteFileTable(pakStream, entries, header)) return false;

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

    if (!pakStream) return info;

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header, entries)) return info;

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

bool Pakker::ValidatePak(const std::string& pakFilename, bool deepVerify) const
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

    if (!pakStream) return false;

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header, entries)) return false;

    for (const auto& entry : entries) {
        if (!ValidateEntry(entry, pakFileSize)) {
            Log(PakLogLevel::Error, "ValidatePak: Invalid entry: " + entry.filename);
            return false;
        }
    }

    if (deepVerify) {
        std::vector<uint8_t> diskBytes;
        for (const auto& entry : entries) {
            uint64_t diskSize = entry.compressedSize;
            diskBytes.assign(static_cast<size_t>(diskSize), 0);
            if (diskSize > 0) {
                pakStream.seekg(entry.offset, std::ios::beg);
                if (!pakStream) {
                    Log(PakLogLevel::Error, "ValidatePak: Failed to seek to offset for file: " + entry.filename);
                    return false;
                }
                pakStream.read(reinterpret_cast<char*>(diskBytes.data()),
                              static_cast<std::streamsize>(diskSize));
                if (!pakStream) {
                    Log(PakLogLevel::Error, "ValidatePak: Failed to read data for file: " + entry.filename);
                    return false;
                }
            }

            uint64_t hash = HashBytesFast(diskBytes.data(), diskBytes.size());
            if (hash != entry.contentHash) {
                Log(PakLogLevel::Error, "ValidatePak: Content hash mismatch: " + entry.filename);
                return false;
            }
        }
    }

    Log(PakLogLevel::Info, "ValidatePak: PAK file '" + pakFilename + "' is valid.");
    return true;
}
