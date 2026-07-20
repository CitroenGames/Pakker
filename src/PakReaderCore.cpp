#include "Pak.h"
#include "PakInternal.h"
#include "PakCompression.h"
#include "PakPlatform.h"
#include <filesystem>
#include <algorithm>
#include <limits>
#include <cstring>

namespace fs = std::filesystem;

using namespace PakInternal;

static void EncryptDecryptSpan(std::span<uint8_t> data, const std::string& key,
                               uint64_t keyOffset = 0)
{
    if (data.empty() || key.empty()) return;
    const size_t keyLength = key.length();
    size_t keyIndex = static_cast<size_t>(keyOffset % keyLength);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] ^= static_cast<uint8_t>(key[keyIndex]);
        if (++keyIndex == keyLength) keyIndex = 0;
    }
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
    std::lock_guard cacheLock(other.cacheMutex_);

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
    archiveFingerprint_ = other.archiveFingerprint_;
    verifyOnRead_ = other.verifyOnRead_;
    cacheOptions_ = other.cacheOptions_;
    cacheStats_ = other.cacheStats_;
    sourceReads_.store(other.sourceReads_.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
    memoryCacheLru_ = std::move(other.memoryCacheLru_);
    memoryCache_ = std::move(other.memoryCache_);
    memoryCacheBytes_ = other.memoryCacheBytes_;
    effectivePersistentCacheDirectory_ = std::move(other.effectivePersistentCacheDirectory_);

    other.isOpen_ = false;
    other.pakFileSize_ = 0;
    other.useMmap_ = false;
    other.alignment_ = 1;
    other.archiveFingerprint_ = 0;
    other.verifyOnRead_ = false;
    other.memoryCacheBytes_ = 0;
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
        std::scoped_lock cacheLock(cacheMutex_, other.cacheMutex_);

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
            archiveFingerprint_ = 0;
            verifyOnRead_ = false;
            pakFilename_.clear();
        }
        memoryCache_.clear();
        memoryCacheLru_.clear();
        memoryCacheBytes_ = 0;
        cacheStats_.memoryBytes = 0;

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
        archiveFingerprint_ = other.archiveFingerprint_;
        verifyOnRead_ = other.verifyOnRead_;
        cacheOptions_ = other.cacheOptions_;
        cacheStats_ = other.cacheStats_;
        sourceReads_.store(other.sourceReads_.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
        memoryCacheLru_ = std::move(other.memoryCacheLru_);
        memoryCache_ = std::move(other.memoryCache_);
        memoryCacheBytes_ = other.memoryCacheBytes_;
        effectivePersistentCacheDirectory_ = std::move(other.effectivePersistentCacheDirectory_);
        // mutex_ and streamMutex_ stay as-is (non-movable)

        other.isOpen_ = false;
        other.pakFileSize_ = 0;
        other.useMmap_ = false;
        other.alignment_ = 1;
        other.archiveFingerprint_ = 0;
        other.verifyOnRead_ = false;
        other.memoryCacheBytes_ = 0;
    }
    return *this;
}

bool PakReader::Open(const std::string& pakFilename)
{
    return Open(pakFilename, PakOpenOptions{});
}

bool PakReader::Open(const std::string& pakFilename, const PakOpenOptions& options)
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
        archiveFingerprint_ = 0;
        verifyOnRead_ = false;
        {
            std::lock_guard cacheLock(cacheMutex_);
            memoryCache_.clear();
            memoryCacheLru_.clear();
            memoryCacheBytes_ = 0;
            cacheStats_.memoryBytes = 0;
        }
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

    {
        uint64_t fingerprint = FNV_OFFSET_BASIS;
        std::error_code ec;
        fs::path absolutePath = fs::absolute(fs::path(pakFilename), ec);
        std::string pathForHash = ec ? pakFilename : absolutePath.string();
        HashString(fingerprint, pathForHash);
        HashValue(fingerprint, pakFileSize_);
        HashValue(fingerprint, header_.numFiles);
        HashValue(fingerprint, header_.fileTableOffset);
        uint64_t modifiedTime = FileTimeFingerprint(pakFilename);
        HashValue(fingerprint, modifiedTime);
        archiveFingerprint_ = fingerprint;
    }

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
    table->pakFilename = pakFilename_;

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
        info.compressed = PakInternal::IsCompressed(storedEntry.flags);
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
    verifyOnRead_ = options.verifyOnRead;
    {
        std::lock_guard cacheLock(cacheMutex_);
        ResolvePersistentCacheDirectoryLocked();
    }
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
        archiveFingerprint_ = 0;
        verifyOnRead_ = false;
        pakFilename_.clear();
        {
            std::lock_guard cacheLock(cacheMutex_);
            memoryCache_.clear();
            memoryCacheLru_.clear();
            memoryCacheBytes_ = 0;
            cacheStats_.memoryBytes = 0;
        }
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

const PakFileInfo* PakReader::InfoByIndex(uint32_t index) const
{
    std::shared_lock lock(mutex_);
    if (!isOpen_ || !table_ || index >= table_->infos.size()) return nullptr;
    return &table_->infos[index];
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
    context.verifyOnRead = verifyOnRead_;
    context.fileSize = pakFileSize_;
    context.archiveFingerprint = archiveFingerprint_;
    return PakStatus::Ok;
}

PakStatus PakReader::ValidateReadRequest(const PakInternal::PakEntry& entry,
    uint64_t destinationSize, const ReadContext& context) const
{
    const uint64_t diskSize = entry.compressedSize;
    const bool compressed = PakInternal::IsCompressed(entry.flags);

    if (entry.offset > context.fileSize || diskSize > context.fileSize - entry.offset) {
        Log(PakLogLevel::Error, "PakReader::Read: Entry exceeds file bounds: " + entry.filename);
        return PakStatus::CorruptArchive;
    }

    if (!compressed && diskSize != entry.originalSize) {
        Log(PakLogLevel::Error, "PakReader::Read: Uncompressed entry has mismatched disk size: " + entry.filename);
        return PakStatus::CorruptArchive;
    }

    if (entry.originalSize > destinationSize) {
        return PakStatus::BufferTooSmall;
    }

    if (compressed && (entry.originalSize > MAX_COMPRESSIBLE_ENTRY_SIZE || diskSize > MAX_COMPRESSIBLE_ENTRY_SIZE)) {
        Log(PakLogLevel::Error, "PakReader::Read: Entry exceeds compressed size limit: " + entry.filename);
        return PakStatus::CorruptArchive;
    }

    return PakStatus::Ok;
}

PakStatus PakReader::ReadEntryToBuffer(const PakInternal::PakEntry& entry,
    std::span<uint8_t> destination, uint64_t* bytesWritten,
    const ReadContext& context) const
{
    if (bytesWritten) *bytesWritten = 0;

    const uint64_t diskSize = entry.compressedSize;
    const bool compressed = PakInternal::IsCompressed(entry.flags);

    PakStatus validation = ValidateReadRequest(entry, static_cast<uint64_t>(destination.size()), context);
    if (validation != PakStatus::Ok) return validation;

    if (entry.originalSize == 0) {
        return PakStatus::Ok;
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

        // output currently holds the raw on-disk bytes (pre-decrypt) -- verify
        // here, before EncryptDecryptSpan mutates them in place.
        if (context.verifyOnRead) {
            uint64_t hash = HashBuffer(output.data(), output.size());
            if (hash != entry.contentHash) {
                Log(PakLogLevel::Error, "PakReader::Read: Content hash mismatch: " + entry.filename);
                return PakStatus::HashMismatch;
            }
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

        // compressedScratch currently holds the raw on-disk bytes (pre-decrypt,
        // still compressed) -- verify before EncryptDecryptSpan mutates them.
        if (context.verifyOnRead) {
            uint64_t hash = HashBuffer(compressedScratch.data(), compressedScratch.size());
            if (hash != entry.contentHash) {
                Log(PakLogLevel::Error, "PakReader::Read: Content hash mismatch: " + entry.filename);
                return PakStatus::HashMismatch;
            }
        }

        EncryptDecryptSpan(compressedScratch, encryptionKey_);
        compressedData = compressedScratch.data();
    } else if (context.verifyOnRead) {
        // compressedData points directly at mapped, unencrypted on-disk bytes.
        uint64_t hash = HashBuffer(compressedData, diskSize);
        if (hash != entry.contentHash) {
            Log(PakLogLevel::Error, "PakReader::Read: Content hash mismatch: " + entry.filename);
            return PakStatus::HashMismatch;
        }
    }

    PakStatus decompressStatus = PakInternal::DecompressBuffer(
        entry.flags, compressedData, diskSize, output.data(), entry.originalSize);
    if (decompressStatus != PakStatus::Ok) {
        Log(PakLogLevel::Error, "PakReader::Read: Decompression failed for: " + entry.filename);
        return decompressStatus;
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
    if (PakInternal::IsCompressed(entry.flags) || !encryptionKey_.empty()) {
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
    return ReadEntryWithCache(handle, entry, destination, bytesWritten, context);
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
    status = ReadEntryWithCache(handle, entry, outData, nullptr, context);
    if (status != PakStatus::Ok) outData.clear();
    return status;
}

PakStatus PakReader::ReadRange(PakFileHandle handle, uint64_t rangeOffset,
                               std::span<uint8_t> destination, uint64_t* bytesWritten) const
{
    if (bytesWritten) *bytesWritten = 0;

    ReadContext context;
    PakStatus status = CaptureReadContext(handle, context);
    if (status != PakStatus::Ok) return status;

    const auto& entry = context.table->entries[handle.index];

    // Compressed frames have no internal chunk index in this format -- a
    // seekable range read isn't possible without decoding the whole entry.
    // Fail closed rather than faking partial-read semantics via a full decode.
    if (PakInternal::IsCompressed(entry.flags)) {
        return PakStatus::Unsupported;
    }

    if (entry.compressedSize != entry.originalSize) {
        Log(PakLogLevel::Error, "PakReader::ReadRange: Uncompressed entry has mismatched disk size: " + entry.filename);
        return PakStatus::CorruptArchive;
    }

    if (rangeOffset > entry.originalSize ||
        destination.size() > entry.originalSize - rangeOffset) {
        return PakStatus::InvalidArgument;
    }

    if (entry.offset > context.fileSize || entry.originalSize > context.fileSize - entry.offset) {
        Log(PakLogLevel::Error, "PakReader::ReadRange: Entry exceeds file bounds: " + entry.filename);
        return PakStatus::CorruptArchive;
    }

    const uint64_t diskOffset = entry.offset + rangeOffset;

    if (context.useMmap && context.guard && context.guard->mf.data) {
        const uint8_t* mappedPtr = static_cast<const uint8_t*>(context.guard->mf.data) + diskOffset;
        if (!destination.empty()) {
            std::memcpy(destination.data(), mappedPtr, destination.size());
        }
    } else {
        if (destination.size() > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)())) {
            return PakStatus::IoError;
        }
        std::lock_guard streamLock(streamMutex_);
        pakStream_.clear();
        pakStream_.seekg(diskOffset, std::ios::beg);
        if (!pakStream_) return PakStatus::IoError;
        if (!destination.empty()) {
            pakStream_.read(reinterpret_cast<char*>(destination.data()),
                            static_cast<std::streamsize>(destination.size()));
            if (!pakStream_) return PakStatus::IoError;
        }
    }

    // XOR-with-repeating-key is range-safe: byte i only depends on
    // key[i % keyLength], so offset the key index by rangeOffset instead of
    // decrypting from the start of the entry.
    if (!encryptionKey_.empty() && !destination.empty()) {
        EncryptDecryptSpan(destination, encryptionKey_, rangeOffset);
    }

    if (bytesWritten) *bytesWritten = destination.size();
    return PakStatus::Ok;
}

PakStatus PakReader::VerifyEntry(PakFileHandle handle) const
{
    ReadContext context;
    PakStatus status = CaptureReadContext(handle, context);
    if (status != PakStatus::Ok) return status;

    const auto& entry = context.table->entries[handle.index];

    uint64_t hash = 0;
    status = HashEntrySourceBytes(entry, context, hash);
    if (status != PakStatus::Ok) return status;

    return hash == entry.contentHash ? PakStatus::Ok : PakStatus::HashMismatch;
}

PakStatus PakReader::Prefetch(PakFileHandle handle) const
{
    ReadContext context;
    PakStatus status = CaptureReadContext(handle, context);
    if (status != PakStatus::Ok) return status;

    const auto& entry = context.table->entries[handle.index];
    if (entry.compressedSize == 0) return PakStatus::Ok;
    if (!context.useMmap || !context.guard || !context.guard->mf.data) {
        if (!PakPlatform::PrefetchFileRange(context.table->pakFilename.c_str(),
                                            entry.offset, entry.compressedSize)) {
            return PakStatus::IoError;
        }
        return PakStatus::Ok;
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

    auto data = std::make_shared<std::vector<uint8_t>>();
    status = Load(handle, *data);
    if (status != PakStatus::Ok) return span;

    if (data->empty()) {
        span.data = nullptr;
        span.size = 0;
        span.ownsData = true;
        span.cachedDataRef_ = std::move(data);
        return span;
    }

    span.data = data->data();
    span.size = data->size();
    span.ownsData = true;
    span.cachedDataRef_ = std::move(data);
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
// Enumeration
// ---------------------------------------------------------------------------

std::vector<std::string> PakReader::ListFiles() const
{
    std::shared_ptr<const RuntimeTable> table;
    {
        std::shared_lock lock(mutex_);
        if (!isOpen_ || !table_) return {};
        table = table_;
    }

    std::vector<std::string> files;
    files.reserve(table->infos.size());
    for (const auto& info : table->infos) {
        files.emplace_back(info.filename);
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<std::string> PakReader::ListFilesWithPrefix(const std::string& prefix) const
{
    const std::string normalizedPrefix = PakInternal::NormalizePathSeparators(prefix);
    std::vector<std::string> files = ListFiles();

    std::vector<std::string> matching;
    for (auto& file : files) {
        if (file.starts_with(normalizedPrefix)) {
            matching.push_back(std::move(file));
        }
    }
    return matching;
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
