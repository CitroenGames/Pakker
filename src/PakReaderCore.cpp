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

// Entry names are absent when an archive is opened without its name blob, so
// diagnostics fall back to the path hash -- which is still enough to identify
// the entry against a build manifest.
static std::string EntryLabel(const PakFileInfo& entry)
{
    if (!entry.filename.empty()) return std::string(entry.filename);
    return "<path hash " + PakInternal::Hex64(entry.pathHash) + ">";
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

// Transfers the decoded memory cache shard by shard. std::list nodes survive
// a move, so the LRU iterators stored in each entry stay valid.
void PakReader::AdoptMemoryShards(PakReader& other) const
{
    for (size_t i = 0; i < kMemoryCacheShards; ++i) {
        MemoryCacheShard& destination = memoryShards_[i];
        MemoryCacheShard& source = other.memoryShards_[i];
        std::scoped_lock shardLock(destination.mutex, source.mutex);

        destination.entries = std::move(source.entries);
        destination.lru = std::move(source.lru);
        destination.bytes = source.bytes;
        destination.hits = source.hits;
        destination.misses = source.misses;
        destination.stores = source.stores;
        destination.evictions = source.evictions;

        source.entries.clear();
        source.lru.clear();
        source.bytes = 0;
        source.hits = 0;
        source.misses = 0;
        source.stores = 0;
        source.evictions = 0;
    }
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
    alignment_ = other.alignment_;

    snapshotOwner_ = std::move(other.snapshotOwner_);
    snapshot_.store(other.snapshot_.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
    other.snapshot_.store(nullptr, std::memory_order_relaxed);

    cacheOptionsOwner_ = std::move(other.cacheOptionsOwner_);
    cacheOptions_.store(cacheOptionsOwner_.get(), std::memory_order_relaxed);
    other.cacheOptions_.store(nullptr, std::memory_order_relaxed);
    cacheStats_ = other.cacheStats_;
    sourceReads_.Store(other.sourceReads_.Load());
    other.sourceReads_.Reset();
    AdoptMemoryShards(other);
    effectivePersistentCacheDirectory_ = std::move(other.effectivePersistentCacheDirectory_);

    other.alignment_ = 1;
}

PakReader& PakReader::operator=(PakReader&& other) noexcept
{
    if (this == &other) return *this;

    // Lock both mutexes in address order to prevent deadlock
    std::unique_lock<std::shared_mutex> lk1, lk2;
    if (this < &other) {
        lk1 = std::unique_lock(mutex_);
        lk2 = std::unique_lock(other.mutex_);
    } else {
        lk2 = std::unique_lock(other.mutex_);
        lk1 = std::unique_lock(mutex_);
    }

    // Takes and releases cacheMutex_, so it must run before the scoped_lock
    // below rather than inside it.
    CloseLocked();

    std::scoped_lock cacheLock(cacheMutex_, other.cacheMutex_);

    encryptionKey_ = std::move(other.encryptionKey_);
    pakStream_ = std::move(other.pakStream_);
    pakFilename_ = std::move(other.pakFilename_);
    header_ = other.header_;
    alignment_ = other.alignment_;

    snapshotOwner_ = std::move(other.snapshotOwner_);
    snapshot_.store(other.snapshot_.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
    other.snapshot_.store(nullptr, std::memory_order_relaxed);

    cacheOptionsOwner_ = std::move(other.cacheOptionsOwner_);
    cacheOptions_.store(cacheOptionsOwner_.get(), std::memory_order_relaxed);
    other.cacheOptions_.store(nullptr, std::memory_order_relaxed);
    cacheStats_ = other.cacheStats_;
    sourceReads_.Store(other.sourceReads_.Load());
    other.sourceReads_.Reset();
    AdoptMemoryShards(other);
    effectivePersistentCacheDirectory_ = std::move(other.effectivePersistentCacheDirectory_);
    // mutex_ and streamMutex_ stay as-is (non-movable)

    other.alignment_ = 1;
    return *this;
}

bool PakReader::Open(const std::string& pakFilename)
{
    return Open(pakFilename, PakOpenOptions{});
}

bool PakReader::Open(const std::string& pakFilename, const PakOpenOptions& options)
{
    std::unique_lock lock(mutex_);
    CloseLocked();

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
    const uint64_t pakFileSize = SafeStreamPos(pakStream_, pakStream_.tellg());

    uint64_t fingerprint = FNV_OFFSET_BASIS;
    {
        std::error_code ec;
        fs::path absolutePath = fs::absolute(fs::path(pakFilename), ec);
        std::string pathForHash = ec ? pakFilename : absolutePath.string();
        HashString(fingerprint, pathForHash);
        HashValue(fingerprint, pakFileSize);
        HashValue(fingerprint, header_.numFiles);
        HashValue(fingerprint, header_.fileTableOffset);
        uint64_t modifiedTime = FileTimeFingerprint(pakFilename);
        HashValue(fingerprint, modifiedTime);
    }

    if (header_.fileTableOffset > pakFileSize) {
        Log(PakLogLevel::Error, "PakReader::Open: Invalid file table offset.");
        pakStream_.close();
        return false;
    }

    const uint32_t numFiles = header_.numFiles;
    if (header_.nameBlobSize > pakFileSize ||
        header_.nameBlobOffset > pakFileSize - header_.nameBlobSize) {
        Log(PakLogLevel::Error, "PakReader::Open: Name blob outside archive bounds.");
        pakStream_.close();
        return false;
    }
    if (static_cast<uint64_t>(numFiles) * sizeof(PakEntryRecord) >
            pakFileSize - header_.fileTableOffset) {
        Log(PakLogLevel::Error, "PakReader::Open: Entry table outside archive bounds.");
        pakStream_.close();
        return false;
    }

    auto snapshot = std::make_shared<ReadSnapshot>();
    snapshot->fileSize = pakFileSize;
    snapshot->archiveFingerprint = fingerprint;
    snapshot->verifyOnRead = options.verifyOnRead;

    RuntimeTable& table = snapshot->table;
    table.pakFilename = pakFilename_;
    table.namesLoaded = options.loadNames;

    // One bulk read for the records, one for the names. This is the whole
    // reason the v7 table is fixed-size: no per-entry stream reads, no
    // per-entry allocation, and the arrays below are the final runtime
    // representation rather than an intermediate parse.
    std::vector<PakEntryRecord> records(numFiles);
    if (numFiles > 0) {
        pakStream_.seekg(static_cast<std::streamoff>(header_.fileTableOffset), std::ios::beg);
        if (!pakStream_) {
            Log(PakLogLevel::Error, "PakReader::Open: Failed to seek to file table.");
            pakStream_.close();
            return false;
        }
        pakStream_.read(reinterpret_cast<char*>(records.data()),
                        static_cast<std::streamsize>(records.size() * sizeof(PakEntryRecord)));
        if (!pakStream_) {
            Log(PakLogLevel::Error, "PakReader::Open: Failed to read entry records.");
            pakStream_.close();
            return false;
        }
    }

    if (options.loadNames && header_.nameBlobSize > 0) {
        table.nameBlob.resize(static_cast<size_t>(header_.nameBlobSize));
        pakStream_.seekg(static_cast<std::streamoff>(header_.nameBlobOffset), std::ios::beg);
        if (!pakStream_) {
            Log(PakLogLevel::Error, "PakReader::Open: Failed to seek to name blob.");
            pakStream_.close();
            return false;
        }
        pakStream_.read(table.nameBlob.data(),
                        static_cast<std::streamsize>(table.nameBlob.size()));
        if (!pakStream_) {
            Log(PakLogLevel::Error, "PakReader::Open: Failed to read name blob.");
            pakStream_.close();
            return false;
        }
    }

    table.infos.resize(numFiles);
    for (uint32_t i = 0; i < numFiles; ++i) {
        const PakEntryRecord& record = records[i];

        const uint64_t diskSize = record.compressedSize > 0 ? record.compressedSize
                                                            : record.originalSize;
        if (record.offset > pakFileSize || diskSize > pakFileSize - record.offset) {
            Log(PakLogLevel::Error, "PakReader::Open: Entry exceeds archive bounds.");
            pakStream_.close();
            return false;
        }
        // Names are bounds-checked against the blob size from the header, so
        // this holds whether or not the blob was actually loaded.
        if (record.nameLength == 0 ||
            record.nameOffset > header_.nameBlobSize ||
            record.nameLength > header_.nameBlobSize - record.nameOffset) {
            Log(PakLogLevel::Error, "PakReader::Open: Name range outside the name blob.");
            pakStream_.close();
            return false;
        }

        PakFileInfo& info = table.infos[i];
        info.originalSize = record.originalSize;
        info.compressedSize = record.compressedSize;
        info.offset = record.offset;
        info.pathHash = record.pathHash;
        info.contentHash = record.contentHash;
        info.flags = record.flags;
        info.compressed = PakInternal::IsCompressed(record.flags);

        if (!table.nameBlob.empty()) {
            std::string_view name(table.nameBlob.data() + record.nameOffset, record.nameLength);
            if (!IsValidFilename(std::string(name))) {
                Log(PakLogLevel::Error, "PakReader::Open: Invalid filename: " + std::string(name));
                pakStream_.close();
                return false;
            }
            info.filename = name;
        }
    }

    if (!BuildLookupTable(table)) {
        Log(PakLogLevel::Error, "PakReader::Open: Duplicate path hash in file table.");
        pakStream_.close();
        return false;
    }

    // Attempt memory-mapped I/O -- close ifstream first to avoid double file handle
    pakStream_.close();

    auto guard = std::make_shared<PakInternal::MappedFileGuard>();
    guard->mf = PakPlatform::MapFileReadOnly(pakFilename.c_str());
    if (guard->mf.data && guard->mf.size == pakFileSize) {
        snapshot->guardPtr = guard.get();
        snapshot->guard = std::move(guard);
        snapshot->useMmap = true;
    } else {
        // Fallback: re-open ifstream (guard destructor unmaps if needed)
        snapshot->useMmap = false;
        pakStream_.open(pakFilename, std::ios::binary);
        if (!pakStream_) {
            Log(PakLogLevel::Error, "PakReader::Open: Failed to re-open pak file for streaming.");
            return false;
        }
    }

    {
        std::lock_guard cacheLock(cacheMutex_);
        ResolvePersistentCacheDirectoryLocked();
    }

    // Publish last. Everything the snapshot points at is fully constructed
    // before this store, and the release ordering pairs with the acquire load
    // in AcquireSnapshot() so readers observe it completely initialized.
    snapshotOwner_ = std::move(snapshot);
    snapshot_.store(snapshotOwner_.get(), std::memory_order_release);

    Log(PakLogLevel::Info, "PakReader::Open: Opened '" + pakFilename + "' with " +
        std::to_string(header_.numFiles) + " files" +
        (snapshotOwner_->useMmap ? " (memory-mapped)" : " (streaming)") + ".");
    return true;
}

void PakReader::CloseLocked()
{
    // Unpublish before releasing. After this store no new read can reach the
    // snapshot; the documented threading contract (no Close() concurrent with
    // reads) covers reads already inside one. PakView/PakSpan values already
    // handed out keep the mapping alive through their own shared_ptr.
    snapshot_.store(nullptr, std::memory_order_release);
    snapshotOwner_.reset();

    pakStream_.close();
    header_ = PakHeader{};
    alignment_ = 1;
    pakFilename_.clear();

    std::lock_guard cacheLock(cacheMutex_);
    for (auto& shard : memoryShards_) {
        std::lock_guard shardLock(shard.mutex);
        shard.entries.clear();
        shard.lru.clear();
        shard.bytes = 0;
    }
    cacheStats_.memoryBytes = 0;
}

void PakReader::Close()
{
    std::unique_lock lock(mutex_);
    CloseLocked();
}

bool PakReader::IsOpen() const
{
    return AcquireSnapshot() != nullptr;
}

bool PakReader::IsMapped() const
{
    const ReadSnapshot* snapshot = AcquireSnapshot();
    return snapshot != nullptr && snapshot->useMmap;
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

// Builds the open-addressing name index over the path hashes the archive
// already stores, so opening does no string hashing at all. Kept at <=70%
// load; the table is sized once and never grows.
bool PakReader::BuildLookupTable(RuntimeTable& table)
{
    const size_t count = table.infos.size();
    if (count == 0) {
        table.lookup.clear();
        table.lookupMask = 0;
        return true;
    }

    uint64_t slotCount = 16;
    const uint64_t required = (static_cast<uint64_t>(count) * 10 + 6) / 7;
    while (slotCount < required) slotCount <<= 1;

    table.lookup.assign(static_cast<size_t>(slotCount), PakInternal::LookupSlot{});
    table.lookupMask = slotCount - 1;

    for (size_t i = 0; i < count; ++i) {
        const uint64_t pathHash = table.infos[i].pathHash;
        uint64_t position = pathHash & table.lookupMask;
        while (true) {
            PakInternal::LookupSlot& slot = table.lookup[static_cast<size_t>(position)];
            if (slot.entryIndex == PakFileHandle::InvalidIndex) {
                slot.hashTag = static_cast<uint32_t>(pathHash >> 32);
                slot.entryIndex = static_cast<uint32_t>(i);
                break;
            }
            if (slot.hashTag == static_cast<uint32_t>(pathHash >> 32) &&
                table.infos[slot.entryIndex].pathHash == pathHash) {
                // Two entries sharing a path hash means either a duplicate
                // path or a genuine collision. Either way the archive is
                // unusable as written, so refuse it rather than silently
                // making one of the two unreachable.
                return false;
            }
            position = (position + 1) & table.lookupMask;
        }
    }
    return true;
}

PakFileHandle PakReader::FindByHashInTable(const RuntimeTable& table, uint64_t pathHash,
                                           std::string_view verifyName)
{
    if (table.lookup.empty()) return {};

    const uint32_t tag = static_cast<uint32_t>(pathHash >> 32);
    uint64_t position = pathHash & table.lookupMask;
    while (true) {
        const PakInternal::LookupSlot& slot = table.lookup[static_cast<size_t>(position)];
        if (slot.entryIndex == PakFileHandle::InvalidIndex) return {};

        // The 32-bit tag rejects almost every non-match without touching the
        // entry array, so a probe usually costs one cache line, not two.
        if (slot.hashTag == tag) {
            const PakFileInfo& info = table.infos[slot.entryIndex];
            if (info.pathHash == pathHash) {
                // Confirm against the stored name when it is resident, so a
                // hash collision cannot hand back the wrong asset. With names
                // dropped there is nothing to confirm against and the 64-bit
                // hash has to stand on its own.
                if (!verifyName.empty() && !info.filename.empty() &&
                    info.filename != verifyName) {
                    return {};
                }
                return PakFileHandle{slot.entryIndex};
            }
        }
        position = (position + 1) & table.lookupMask;
    }
}

PakFileHandle PakReader::FindInTable(const RuntimeTable& table, std::string_view filename)
{
    if (filename.empty()) return {};

    if (NeedsPathNormalization(filename)) {
        const std::string normalized = NormalizePath(filename);
        return FindByHashInTable(table, PakPathHash(normalized), normalized);
    }
    return FindByHashInTable(table, PakPathHash(filename), filename);
}

PakFileHandle PakReader::Find(std::string_view filename) const
{
    const ReadSnapshot* snapshot = AcquireSnapshot();
    if (!snapshot) return {};
    return FindInTable(snapshot->table, filename);
}

PakFileHandle PakReader::FindByHash(uint64_t pathHash) const
{
    const ReadSnapshot* snapshot = AcquireSnapshot();
    if (!snapshot) return {};
    return FindByHashInTable(snapshot->table, pathHash, std::string_view{});
}

size_t PakReader::Resolve(std::span<const std::string_view> filenames,
                          std::span<PakFileHandle> handles) const
{
    size_t resolvedCount = 0;
    size_t count = std::min(filenames.size(), handles.size());

    const ReadSnapshot* snapshot = AcquireSnapshot();
    if (!snapshot) {
        for (size_t i = 0; i < count; ++i) handles[i] = {};
        return 0;
    }

    for (size_t i = 0; i < count; ++i) {
        handles[i] = FindInTable(snapshot->table, filenames[i]);
        if (handles[i]) ++resolvedCount;
    }
    return resolvedCount;
}

const PakFileInfo* PakReader::Info(PakFileHandle handle) const
{
    const ReadSnapshot* snapshot = AcquireSnapshot();
    if (!snapshot || !handle || handle.index >= snapshot->table.infos.size()) return nullptr;
    return &snapshot->table.infos[handle.index];
}

const PakFileInfo* PakReader::InfoByIndex(uint32_t index) const
{
    const ReadSnapshot* snapshot = AcquireSnapshot();
    if (!snapshot || index >= snapshot->table.infos.size()) return nullptr;
    return &snapshot->table.infos[index];
}

// ---------------------------------------------------------------------------
// Read helpers
// ---------------------------------------------------------------------------

const PakReader::ReadSnapshot* PakReader::ResolveHandle(PakFileHandle handle,
    PakStatus& outStatus) const noexcept
{
    const ReadSnapshot* snapshot = AcquireSnapshot();
    if (!snapshot) {
        outStatus = PakStatus::NotOpen;
        return nullptr;
    }
    if (!handle || handle.index >= snapshot->table.infos.size()) {
        outStatus = PakStatus::InvalidHandle;
        return nullptr;
    }
    outStatus = PakStatus::Ok;
    return snapshot;
}

PakStatus PakReader::ValidateReadRequest(const PakFileInfo& entry,
    uint64_t destinationSize, const ReadSnapshot& snapshot) const
{
    const uint64_t diskSize = entry.compressedSize;
    const bool compressed = PakInternal::IsCompressed(entry.flags);

    if (entry.offset > snapshot.fileSize || diskSize > snapshot.fileSize - entry.offset) {
        Log(PakLogLevel::Error, "PakReader::Read: Entry exceeds file bounds: " + EntryLabel(entry));
        return PakStatus::CorruptArchive;
    }

    if (!compressed && diskSize != entry.originalSize) {
        Log(PakLogLevel::Error, "PakReader::Read: Uncompressed entry has mismatched disk size: " + EntryLabel(entry));
        return PakStatus::CorruptArchive;
    }

    if (entry.originalSize > destinationSize) {
        return PakStatus::BufferTooSmall;
    }

    if (compressed && (entry.originalSize > MAX_COMPRESSIBLE_ENTRY_SIZE || diskSize > MAX_COMPRESSIBLE_ENTRY_SIZE)) {
        Log(PakLogLevel::Error, "PakReader::Read: Entry exceeds compressed size limit: " + EntryLabel(entry));
        return PakStatus::CorruptArchive;
    }

    return PakStatus::Ok;
}

PakStatus PakReader::ReadEntryToBuffer(const PakFileInfo& entry,
    std::span<uint8_t> destination, uint64_t* bytesWritten,
    const ReadSnapshot& snapshot) const
{
    if (bytesWritten) *bytesWritten = 0;

    const uint64_t diskSize = entry.compressedSize;
    const bool compressed = PakInternal::IsCompressed(entry.flags);

    PakStatus validation = ValidateReadRequest(entry, static_cast<uint64_t>(destination.size()), snapshot);
    if (validation != PakStatus::Ok) return validation;

    if (entry.originalSize == 0) {
        return PakStatus::Ok;
    }

    auto output = destination.first(static_cast<size_t>(entry.originalSize));

    const uint8_t* mappedPtr = nullptr;
    if (snapshot.useMmap && snapshot.guardPtr && snapshot.guardPtr->mf.data) {
        mappedPtr = static_cast<const uint8_t*>(snapshot.guardPtr->mf.data) + entry.offset;
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
        if (snapshot.verifyOnRead) {
            uint64_t hash = HashBytesFast(output.data(), output.size());
            if (hash != entry.contentHash) {
                Log(PakLogLevel::Error, "PakReader::Read: Content hash mismatch: " + EntryLabel(entry));
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
        if (snapshot.verifyOnRead) {
            uint64_t hash = HashBytesFast(compressedScratch.data(), compressedScratch.size());
            if (hash != entry.contentHash) {
                Log(PakLogLevel::Error, "PakReader::Read: Content hash mismatch: " + EntryLabel(entry));
                return PakStatus::HashMismatch;
            }
        }

        EncryptDecryptSpan(compressedScratch, encryptionKey_);
        compressedData = compressedScratch.data();
    } else if (snapshot.verifyOnRead) {
        // compressedData points directly at mapped, unencrypted on-disk bytes.
        uint64_t hash = HashBytesFast(compressedData, diskSize);
        if (hash != entry.contentHash) {
            Log(PakLogLevel::Error, "PakReader::Read: Content hash mismatch: " + EntryLabel(entry));
            return PakStatus::HashMismatch;
        }
    }

    PakStatus decompressStatus = PakInternal::DecompressBuffer(
        entry.flags, compressedData, diskSize, output.data(), entry.originalSize);
    if (decompressStatus != PakStatus::Ok) {
        Log(PakLogLevel::Error, "PakReader::Read: Decompression failed for: " + EntryLabel(entry));
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

    PakStatus status = PakStatus::Ok;
    const ReadSnapshot* snapshotPtr = ResolveHandle(handle, status);
    if (!snapshotPtr) return status;
    const ReadSnapshot& snapshot = *snapshotPtr;

    const auto& entry = snapshot.table.infos[handle.index];
    if (PakInternal::IsCompressed(entry.flags) || !encryptionKey_.empty()) {
        return PakStatus::Unsupported;
    }
    if (!snapshot.useMmap || !snapshot.guardPtr || !snapshot.guardPtr->mf.data) {
        return PakStatus::Unsupported;
    }
    if (entry.offset > snapshot.fileSize || entry.originalSize > snapshot.fileSize - entry.offset) {
        return PakStatus::CorruptArchive;
    }

    outView.data = static_cast<const uint8_t*>(snapshot.guardPtr->mf.data) + entry.offset;
    outView.size = entry.originalSize;
    outView.mapped = true;
    // The one reference count the read path still takes. PakView is
    // documented to stay valid after Close(), which requires it to own a
    // share of the mapping -- there is no way to honour that without it.
    outView.mappingRef_ = snapshot.guard;
    return PakStatus::Ok;
}

PakStatus PakReader::Read(PakFileHandle handle, std::span<uint8_t> destination,
                          uint64_t* bytesWritten) const
{
    PakStatus status = PakStatus::Ok;
    const ReadSnapshot* snapshotPtr = ResolveHandle(handle, status);
    if (!snapshotPtr) return status;
    const ReadSnapshot& snapshot = *snapshotPtr;

    const auto& entry = snapshot.table.infos[handle.index];
    return ReadEntryWithCache(handle, entry, destination, bytesWritten, snapshot);
}

PakStatus PakReader::Load(PakFileHandle handle, std::vector<uint8_t>& outData) const
{
    PakStatus status = PakStatus::Ok;
    const ReadSnapshot* snapshotPtr = ResolveHandle(handle, status);
    if (!snapshotPtr) {
        outData.clear();
        return status;
    }
    const ReadSnapshot& snapshot = *snapshotPtr;

    const auto& entry = snapshot.table.infos[handle.index];
    if (entry.originalSize > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
        outData.clear();
        return PakStatus::InvalidArgument;
    }

    outData.resize(static_cast<size_t>(entry.originalSize));
    status = ReadEntryWithCache(handle, entry, outData, nullptr, snapshot);
    if (status != PakStatus::Ok) outData.clear();
    return status;
}

PakStatus PakReader::ReadRange(PakFileHandle handle, uint64_t rangeOffset,
                               std::span<uint8_t> destination, uint64_t* bytesWritten) const
{
    if (bytesWritten) *bytesWritten = 0;

    PakStatus status = PakStatus::Ok;
    const ReadSnapshot* snapshotPtr = ResolveHandle(handle, status);
    if (!snapshotPtr) return status;
    const ReadSnapshot& snapshot = *snapshotPtr;

    const auto& entry = snapshot.table.infos[handle.index];

    // Compressed frames have no internal chunk index in this format -- a
    // seekable range read isn't possible without decoding the whole entry.
    // Fail closed rather than faking partial-read semantics via a full decode.
    if (PakInternal::IsCompressed(entry.flags)) {
        return PakStatus::Unsupported;
    }

    if (entry.compressedSize != entry.originalSize) {
        Log(PakLogLevel::Error, "PakReader::ReadRange: Uncompressed entry has mismatched disk size: " + EntryLabel(entry));
        return PakStatus::CorruptArchive;
    }

    if (rangeOffset > entry.originalSize ||
        destination.size() > entry.originalSize - rangeOffset) {
        return PakStatus::InvalidArgument;
    }

    if (entry.offset > snapshot.fileSize || entry.originalSize > snapshot.fileSize - entry.offset) {
        Log(PakLogLevel::Error, "PakReader::ReadRange: Entry exceeds file bounds: " + EntryLabel(entry));
        return PakStatus::CorruptArchive;
    }

    const uint64_t diskOffset = entry.offset + rangeOffset;

    if (snapshot.useMmap && snapshot.guardPtr && snapshot.guardPtr->mf.data) {
        const uint8_t* mappedPtr = static_cast<const uint8_t*>(snapshot.guardPtr->mf.data) + diskOffset;
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
    PakStatus status = PakStatus::Ok;
    const ReadSnapshot* snapshotPtr = ResolveHandle(handle, status);
    if (!snapshotPtr) return status;
    const ReadSnapshot& snapshot = *snapshotPtr;

    const auto& entry = snapshot.table.infos[handle.index];

    uint64_t hash = 0;
    status = HashEntrySourceBytes(entry, snapshot, hash);
    if (status != PakStatus::Ok) return status;

    return hash == entry.contentHash ? PakStatus::Ok : PakStatus::HashMismatch;
}

PakStatus PakReader::Prefetch(PakFileHandle handle) const
{
    PakStatus status = PakStatus::Ok;
    const ReadSnapshot* snapshotPtr = ResolveHandle(handle, status);
    if (!snapshotPtr) return status;
    const ReadSnapshot& snapshot = *snapshotPtr;

    const auto& entry = snapshot.table.infos[handle.index];
    if (entry.compressedSize == 0) return PakStatus::Ok;
    if (!snapshot.useMmap || !snapshot.guardPtr || !snapshot.guardPtr->mf.data) {
        if (!PakPlatform::PrefetchFileRange(snapshot.table.pakFilename.c_str(),
                                            entry.offset, entry.compressedSize)) {
            return PakStatus::IoError;
        }
        return PakStatus::Ok;
    }
    if (!PakPlatform::PrefetchMappedRange(snapshot.guardPtr->mf, entry.offset, entry.compressedSize)) {
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
    const ReadSnapshot* snapshot = AcquireSnapshot();
    if (!snapshot) return 0;
    return static_cast<uint32_t>(snapshot->table.infos.size());
}

PakReader::FileInfo PakReader::GetFileInfo(const std::string& filename) const
{
    FileInfo info{};
    info.found = false;
    info.compressed = false;
    info.originalSize = 0;
    info.compressedSize = 0;

    const ReadSnapshot* snapshot = AcquireSnapshot();
    if (!snapshot) return info;

    PakFileHandle handle = FindInTable(snapshot->table, filename);
    if (handle) {
        const PakFileInfo& runtimeInfo = snapshot->table.infos[handle.index];
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
    const ReadSnapshot* snapshot = AcquireSnapshot();
    if (!snapshot) return {};

    // Nothing to enumerate when the archive was opened without its name blob;
    // returning a run of empty strings would be worse than returning nothing.
    if (!snapshot->table.namesLoaded) return {};

    std::vector<std::string> files;
    files.reserve(snapshot->table.infos.size());
    for (const auto& info : snapshot->table.infos) {
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
