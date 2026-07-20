#include "Pak.h"
#include "PakInternal.h"
#include "PakPlatform.h"
#include <filesystem>
#include <atomic>
#include <algorithm>
#include <limits>
#include <cstring>

namespace fs = std::filesystem;

using namespace PakInternal;

static std::atomic<uint64_t> g_cacheTempCounter{0};

#pragma pack(push, 1)
struct PersistentCacheHeader {
    char magic[4] = {'P', 'K', 'C', '1'};
    uint32_t version = 1;
    uint64_t keyHigh = 0;
    uint64_t keyLow = 0;
    uint64_t dataSize = 0;
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Cache configuration and maintenance
// ---------------------------------------------------------------------------

void PakReader::SetCacheOptions(const PakCacheOptions& options)
{
    std::lock_guard cacheLock(cacheMutex_);
    cacheOptions_ = options;
    ResolvePersistentCacheDirectoryLocked();
    TrimMemoryCacheLocked();
}

PakCacheOptions PakReader::GetCacheOptions() const
{
    std::lock_guard cacheLock(cacheMutex_);
    return cacheOptions_;
}

PakCacheStats PakReader::GetCacheStats() const
{
    std::lock_guard cacheLock(cacheMutex_);
    PakCacheStats stats = cacheStats_;
    stats.sourceReads = sourceReads_.load(std::memory_order_relaxed);
    stats.memoryBytes = memoryCacheBytes_;
    return stats;
}

void PakReader::ClearMemoryCache()
{
    std::lock_guard cacheLock(cacheMutex_);
    memoryCache_.clear();
    memoryCacheLru_.clear();
    memoryCacheBytes_ = 0;
    cacheStats_.memoryBytes = 0;
}

bool PakReader::ClearPersistentCache()
{
    std::string directory;
    {
        std::lock_guard cacheLock(cacheMutex_);
        ResolvePersistentCacheDirectoryLocked();
        directory = effectivePersistentCacheDirectory_;
        cacheStats_.persistentBytes = 0;
    }

    if (directory.empty()) return true;

    std::error_code ec;
    if (!fs::exists(directory, ec)) return !ec;

    bool ok = true;
    for (fs::recursive_directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path path = it->path();
        if (path.extension() == ".pkc" || path.extension() == ".tmp") {
            fs::remove(path, ec);
            if (ec) {
                ok = false;
                ec.clear();
            }
        }
    }
    return ok && !ec;
}

bool PakReader::ClearCache()
{
    ClearMemoryCache();
    return ClearPersistentCache();
}

bool PakReader::HasPersistentCacheDirectoryLocked() const
{
    return cacheOptions_.enabled &&
           cacheOptions_.persistentCacheEnabled &&
           cacheOptions_.persistentBudgetBytes > 0 &&
           !effectivePersistentCacheDirectory_.empty();
}

void PakReader::ResolvePersistentCacheDirectoryLocked() const
{
    effectivePersistentCacheDirectory_.clear();
    if (!cacheOptions_.enabled || !cacheOptions_.persistentCacheEnabled ||
        cacheOptions_.persistentBudgetBytes == 0) {
        return;
    }

    if (!cacheOptions_.persistentCacheDirectory.empty()) {
        effectivePersistentCacheDirectory_ = cacheOptions_.persistentCacheDirectory;
        return;
    }

    effectivePersistentCacheDirectory_ = PakPlatform::GetDefaultCacheDirectory();
}

std::string PakReader::PersistentCachePathLocked(CacheKey key) const
{
    std::string name = Hex64(key.high) + Hex64(key.low);
    fs::path root(effectivePersistentCacheDirectory_);
    fs::path shard = name.substr(0, 2);
    return (root / shard / (name + ".pkc")).string();
}

void PakReader::TrimMemoryCacheLocked() const
{
    const bool keepMemory = cacheOptions_.enabled &&
                            cacheOptions_.memoryCacheEnabled &&
                            cacheOptions_.memoryBudgetBytes > 0;
    if (!keepMemory) {
        if (!memoryCache_.empty()) {
            cacheStats_.memoryEvictions += static_cast<uint64_t>(memoryCache_.size());
        }
        memoryCache_.clear();
        memoryCacheLru_.clear();
        memoryCacheBytes_ = 0;
        cacheStats_.memoryBytes = 0;
        return;
    }

    while (memoryCacheBytes_ > cacheOptions_.memoryBudgetBytes && !memoryCache_.empty()) {
        if (memoryCacheLru_.empty()) {
            cacheStats_.memoryEvictions += static_cast<uint64_t>(memoryCache_.size());
            memoryCache_.clear();
            memoryCacheBytes_ = 0;
            break;
        }
        const CacheKey oldestKey = memoryCacheLru_.back();
        auto oldest = memoryCache_.find(oldestKey);
        if (oldest == memoryCache_.end()) {
            // Keep the structures self-healing if invariants are ever broken
            // by a future cache mutation path.
            memoryCacheLru_.pop_back();
            continue;
        }
        memoryCacheBytes_ -= oldest->second.size;
        memoryCacheLru_.pop_back();
        memoryCache_.erase(oldest);
        ++cacheStats_.memoryEvictions;
    }
    cacheStats_.memoryBytes = memoryCacheBytes_;
}

void PakReader::TrimPersistentCache() const
{
    std::string directory;
    uint64_t budget = 0;
    {
        std::lock_guard cacheLock(cacheMutex_);
        if (!HasPersistentCacheDirectoryLocked()) return;
        directory = effectivePersistentCacheDirectory_;
        budget = cacheOptions_.persistentBudgetBytes;
    }

    std::error_code ec;
    if (!fs::exists(directory, ec)) return;

    struct FileRecord {
        fs::path path;
        uint64_t size = 0;
        fs::file_time_type modified{};
    };

    std::vector<FileRecord> files;
    uint64_t total = 0;
    for (fs::recursive_directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".pkc") continue;
        uint64_t size = static_cast<uint64_t>(it->file_size(ec));
        if (ec) {
            ec.clear();
            continue;
        }
        auto modified = it->last_write_time(ec);
        if (ec) {
            ec.clear();
            modified = fs::file_time_type::min();
        }
        total += size;
        files.push_back({it->path(), size, modified});
    }

    if (total <= budget) {
        std::lock_guard cacheLock(cacheMutex_);
        cacheStats_.persistentBytes = total;
        return;
    }

    std::sort(files.begin(), files.end(), [](const FileRecord& a, const FileRecord& b) {
        return a.modified < b.modified;
    });

    uint64_t evicted = 0;
    for (const FileRecord& file : files) {
        if (total <= budget) break;
        fs::remove(file.path, ec);
        if (!ec) {
            total -= file.size;
            ++evicted;
        } else {
            ec.clear();
        }
    }

    std::lock_guard cacheLock(cacheMutex_);
    cacheStats_.persistentBytes = total;
    cacheStats_.persistentEvictions += evicted;
}

// ---------------------------------------------------------------------------
// Cache read path
// ---------------------------------------------------------------------------

bool PakReader::ShouldCacheDecoded(const PakInternal::PakEntry& entry,
    const ReadContext& context, const PakCacheOptions& options) const
{
    if (!options.enabled) return false;
    if (entry.originalSize == 0 || entry.originalSize > options.maxSingleEntryBytes) return false;
    if (!options.memoryCacheEnabled && !options.persistentCacheEnabled) return false;

    const bool compressed = PakInternal::IsCompressed(entry.flags);
    return compressed || !encryptionKey_.empty() || !context.useMmap;
}

PakReader::CacheKey PakReader::MakeMemoryCacheKey(PakFileHandle handle,
    const ReadContext& context) noexcept
{
    // The decoded memory cache is reader-local and cleared whenever a new
    // archive replaces the current one. The archive fingerprint plus stable
    // handle index is therefore already a complete identity; hashing the
    // filename and entry metadata again on every cache hit is redundant.
    return CacheKey{context.archiveFingerprint, static_cast<uint64_t>(handle.index)};
}

PakReader::CacheKey PakReader::MakePersistentCacheKey(PakFileHandle handle,
    const PakInternal::PakEntry& entry, const ReadContext& context) const
{
    uint64_t high = FNV_OFFSET_BASIS;
    uint64_t low = FNV_OFFSET_BASIS ^ 0x9e3779b97f4a7c15ull;

    HashValue(high, context.archiveFingerprint);
    HashValue(high, handle.index);
    HashValue(high, entry.offset);
    HashValue(high, entry.originalSize);
    HashValue(high, entry.compressedSize);
    HashValue(high, entry.flags);
    HashString(high, entry.filename);

    HashValue(low, context.archiveFingerprint);
    HashValue(low, entry.contentHash);
    uint64_t encryptionHash = FNV_OFFSET_BASIS;
    HashString(encryptionHash, encryptionKey_);
    HashValue(low, encryptionHash);
    HashValue(low, entry.originalSize);
    HashString(low, entry.filename);

    return CacheKey{high, low};
}

PakStatus PakReader::HashEntrySourceBytes(const PakInternal::PakEntry& entry,
    const ReadContext& context, uint64_t& outHash) const
{
    outHash = 0;
    const uint64_t diskSize = entry.compressedSize;
    if (diskSize == 0) {
        outHash = FNV_OFFSET_BASIS;
        return PakStatus::Ok;
    }

    if (entry.offset > context.fileSize || diskSize > context.fileSize - entry.offset) {
        return PakStatus::CorruptArchive;
    }

    if (context.useMmap && context.guard && context.guard->mf.data) {
        const auto* mappedPtr = static_cast<const uint8_t*>(context.guard->mf.data) + entry.offset;
        outHash = HashBuffer(mappedPtr, diskSize);
        return PakStatus::Ok;
    }

    if (diskSize > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)())) {
        return PakStatus::IoError;
    }
    if (diskSize > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
        return PakStatus::IoError;
    }

    std::vector<uint8_t> source(static_cast<size_t>(diskSize));
    {
        std::lock_guard streamLock(streamMutex_);
        pakStream_.clear();
        pakStream_.seekg(entry.offset, std::ios::beg);
        if (!pakStream_) return PakStatus::IoError;
        pakStream_.read(reinterpret_cast<char*>(source.data()),
                        static_cast<std::streamsize>(source.size()));
        if (!pakStream_) return PakStatus::IoError;
    }
    outHash = HashBuffer(source.data(), source.size());
    return PakStatus::Ok;
}

bool PakReader::TryGetMemoryCache(CacheKey key,
    std::shared_ptr<const std::vector<uint8_t>>& outData) const
{
    std::lock_guard cacheLock(cacheMutex_);
    if (!cacheOptions_.enabled || !cacheOptions_.memoryCacheEnabled ||
        cacheOptions_.memoryBudgetBytes == 0) {
        return false;
    }

    auto it = memoryCache_.find(key);
    if (it == memoryCache_.end()) {
        ++cacheStats_.memoryMisses;
        return false;
    }

    memoryCacheLru_.splice(memoryCacheLru_.begin(), memoryCacheLru_,
                           it->second.lruPosition);
    it->second.lruPosition = memoryCacheLru_.begin();
    outData = it->second.data;
    ++cacheStats_.memoryHits;
    return static_cast<bool>(outData);
}

void PakReader::StoreMemoryCache(CacheKey key,
    std::shared_ptr<const std::vector<uint8_t>> data) const
{
    if (!data) return;

    std::lock_guard cacheLock(cacheMutex_);
    if (!cacheOptions_.enabled || !cacheOptions_.memoryCacheEnabled ||
        cacheOptions_.memoryBudgetBytes == 0 ||
        data->size() > cacheOptions_.maxSingleEntryBytes ||
        data->size() > cacheOptions_.memoryBudgetBytes) {
        return;
    }

    uint64_t size = static_cast<uint64_t>(data->size());
    auto it = memoryCache_.find(key);
    if (it != memoryCache_.end()) {
        memoryCacheBytes_ -= it->second.size;
        memoryCacheLru_.splice(memoryCacheLru_.begin(), memoryCacheLru_,
                               it->second.lruPosition);
        it->second = DecodedCacheEntry{std::move(data), size, memoryCacheLru_.begin()};
    } else {
        memoryCacheLru_.push_front(key);
        memoryCache_.emplace(key,
            DecodedCacheEntry{std::move(data), size, memoryCacheLru_.begin()});
    }

    memoryCacheBytes_ += size;
    ++cacheStats_.memoryStores;
    TrimMemoryCacheLocked();
}

bool PakReader::TryLoadPersistentCache(CacheKey key,
    std::shared_ptr<const std::vector<uint8_t>>& outData) const
{
    std::string pathString;
    {
        std::lock_guard cacheLock(cacheMutex_);
        if (!HasPersistentCacheDirectoryLocked()) return false;
        pathString = PersistentCachePathLocked(key);
    }

    std::ifstream stream(pathString, std::ios::binary | std::ios::ate);
    if (!stream) {
        std::lock_guard cacheLock(cacheMutex_);
        ++cacheStats_.persistentMisses;
        return false;
    }

    std::streampos end = stream.tellg();
    uint64_t cacheFileSize = SafeStreamPos(stream, end);
    if (!stream || cacheFileSize < sizeof(PersistentCacheHeader)) {
        std::lock_guard cacheLock(cacheMutex_);
        ++cacheStats_.persistentMisses;
        return false;
    }

    stream.seekg(0, std::ios::beg);
    PersistentCacheHeader header{};
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!stream || std::memcmp(header.magic, "PKC1", 4) != 0 ||
        header.version != 1 || header.keyHigh != key.high ||
        header.keyLow != key.low) {
        std::lock_guard cacheLock(cacheMutex_);
        ++cacheStats_.persistentMisses;
        return false;
    }

    {
        std::lock_guard cacheLock(cacheMutex_);
        if (header.dataSize > cacheOptions_.maxSingleEntryBytes) {
            ++cacheStats_.persistentMisses;
            return false;
        }
    }

    if (cacheFileSize != sizeof(PersistentCacheHeader) + header.dataSize ||
        header.dataSize > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
        std::lock_guard cacheLock(cacheMutex_);
        ++cacheStats_.persistentMisses;
        return false;
    }

    auto data = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(header.dataSize));
    if (header.dataSize > 0) {
        stream.read(reinterpret_cast<char*>(data->data()),
                    static_cast<std::streamsize>(data->size()));
        if (!stream) {
            std::lock_guard cacheLock(cacheMutex_);
            ++cacheStats_.persistentMisses;
            return false;
        }
    }

    std::error_code ec;
    fs::last_write_time(pathString, fs::file_time_type::clock::now(), ec);

    outData = std::move(data);
    {
        std::lock_guard cacheLock(cacheMutex_);
        ++cacheStats_.persistentHits;
    }
    return true;
}

void PakReader::StorePersistentCache(CacheKey key, const std::vector<uint8_t>& data) const
{
    std::string pathString;
    {
        std::lock_guard cacheLock(cacheMutex_);
        if (!HasPersistentCacheDirectoryLocked() ||
            data.size() > cacheOptions_.maxSingleEntryBytes ||
            data.size() > cacheOptions_.persistentBudgetBytes) {
            return;
        }
        pathString = PersistentCachePathLocked(key);
    }

    std::error_code ec;
    fs::path path(pathString);
    fs::create_directories(path.parent_path(), ec);
    if (ec) return;
    if (fs::exists(path, ec) && !ec) return;
    ec.clear();

    uint64_t tempId = g_cacheTempCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    fs::path tempPath = path;
    tempPath += ".";
    tempPath += std::to_string(tempId);
    tempPath += ".tmp";

    {
        std::ofstream stream(tempPath, std::ios::binary);
        if (!stream) return;

        PersistentCacheHeader header{};
        header.keyHigh = key.high;
        header.keyLow = key.low;
        header.dataSize = static_cast<uint64_t>(data.size());
        stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
        if (!data.empty()) {
            stream.write(reinterpret_cast<const char*>(data.data()),
                         static_cast<std::streamsize>(data.size()));
        }
        if (!stream) {
            stream.close();
            fs::remove(tempPath, ec);
            return;
        }
    }

    fs::rename(tempPath, path, ec);
    if (ec) {
        fs::remove(tempPath, ec);
        return;
    }

    {
        std::lock_guard cacheLock(cacheMutex_);
        ++cacheStats_.persistentStores;
        cacheStats_.persistentBytes += sizeof(PersistentCacheHeader) + static_cast<uint64_t>(data.size());
    }
    TrimPersistentCache();
}

PakStatus PakReader::ReadEntryWithCache(PakFileHandle handle,
    const PakInternal::PakEntry& entry, std::span<uint8_t> destination,
    uint64_t* bytesWritten, const ReadContext& context) const
{
    if (bytesWritten) *bytesWritten = 0;

    PakStatus validation = ValidateReadRequest(entry, static_cast<uint64_t>(destination.size()), context);
    if (validation != PakStatus::Ok) return validation;
    if (entry.originalSize == 0) return PakStatus::Ok;

    // verifyOnRead promises to hash the current source bytes on every call.
    // A decoded cache hit cannot satisfy that contract, so keep verification
    // on the direct source path regardless of cache policy.
    if (context.verifyOnRead) {
        PakStatus status = ReadEntryToBuffer(entry, destination, bytesWritten, context);
        if (status == PakStatus::Ok) {
            sourceReads_.fetch_add(1, std::memory_order_relaxed);
        }
        return status;
    }

    // Mapped, plain entries can never benefit from the decoded cache: the
    // mapping is already the cache and Read() only needs a memcpy. Avoid the
    // cache mutex and policy copy entirely on this dominant shipping path.
    if (context.useMmap && !PakInternal::IsCompressed(entry.flags) &&
        encryptionKey_.empty()) {
        PakStatus status = ReadEntryToBuffer(entry, destination, bytesWritten, context);
        if (status == PakStatus::Ok) {
            sourceReads_.fetch_add(1, std::memory_order_relaxed);
        }
        return status;
    }

    PakCacheOptions options;
    {
        std::lock_guard cacheLock(cacheMutex_);
        options = cacheOptions_;
    }

    if (!ShouldCacheDecoded(entry, context, options)) {
        PakStatus status = ReadEntryToBuffer(entry, destination, bytesWritten, context);
        if (status == PakStatus::Ok) {
            sourceReads_.fetch_add(1, std::memory_order_relaxed);
        }
        return status;
    }

    CacheKey memoryKey = MakeMemoryCacheKey(handle, context);
    std::shared_ptr<const std::vector<uint8_t>> cached;
    if (options.memoryCacheEnabled && TryGetMemoryCache(memoryKey, cached)) {
        if (cached->size() != entry.originalSize) return PakStatus::CorruptArchive;
        std::memcpy(destination.data(), cached->data(), cached->size());
        if (bytesWritten) *bytesWritten = entry.originalSize;
        return PakStatus::Ok;
    }

    bool havePersistentKey = false;
    CacheKey persistentKey{};
    bool persistentAvailable = false;
    {
        std::lock_guard cacheLock(cacheMutex_);
        persistentAvailable = HasPersistentCacheDirectoryLocked();
    }
    if (options.persistentCacheEnabled && persistentAvailable) {
        // contentHash is the archive's persisted FNV-1a hash of these exact
        // on-disk source bytes. Reuse it in the persistent key instead of
        // scanning the compressed payload before every cache lookup.
        persistentKey = MakePersistentCacheKey(handle, entry, context);
        havePersistentKey = true;
        if (TryLoadPersistentCache(persistentKey, cached)) {
            if (cached->size() != entry.originalSize) return PakStatus::CorruptArchive;
            std::memcpy(destination.data(), cached->data(), cached->size());
            StoreMemoryCache(memoryKey, cached);
            if (bytesWritten) *bytesWritten = entry.originalSize;
            return PakStatus::Ok;
        }
    }

    PakStatus status = ReadEntryToBuffer(entry, destination, bytesWritten, context);
    if (status != PakStatus::Ok) return status;

    sourceReads_.fetch_add(1, std::memory_order_relaxed);

    auto output = destination.first(static_cast<size_t>(entry.originalSize));
    auto stored = std::make_shared<std::vector<uint8_t>>(output.begin(), output.end());
    StoreMemoryCache(memoryKey, stored);
    if (havePersistentKey) {
        StorePersistentCache(persistentKey, *stored);
    }
    return PakStatus::Ok;
}
