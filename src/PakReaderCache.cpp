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

// Entry names are absent when an archive is opened without its name blob, so
// diagnostics fall back to the path hash -- which is still enough to identify
// the entry against a build manifest.
static std::string EntryLabel(const PakFileInfo& entry)
{
    if (!entry.filename.empty()) return std::string(entry.filename);
    return "<path hash " + PakInternal::Hex64(entry.pathHash) + ">";
}

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

const PakCacheOptions PakReader::kDefaultCacheOptions{};

void PakReader::SetCacheOptions(const PakCacheOptions& options)
{
    std::lock_guard cacheLock(cacheMutex_);

    // Changing the budget can change the shard count, which remaps every key
    // to a different shard. Anything already cached would become unreachable,
    // so drop it rather than leak it.
    const size_t previousShards = ActiveShardCount(CacheOptions());

    cacheOptionsOwner_ = std::make_shared<const PakCacheOptions>(options);
    cacheOptions_.store(cacheOptionsOwner_.get(), std::memory_order_release);

    if (ActiveShardCount(options) != previousShards) {
        for (auto& shard : memoryShards_) {
            std::lock_guard shardLock(shard.mutex);
            shard.entries.clear();
            shard.lru.clear();
            shard.bytes = 0;
        }
    }

    ResolvePersistentCacheDirectoryLocked();
    TrimAllMemoryShards();
}

PakCacheOptions PakReader::GetCacheOptions() const
{
    std::lock_guard cacheLock(cacheMutex_);
    return CacheOptions();
}

PakCacheStats PakReader::GetCacheStats() const
{
    PakCacheStats stats;
    {
        std::lock_guard cacheLock(cacheMutex_);
        stats = cacheStats_;
    }
    stats.sourceReads = sourceReads_.Load();

    for (auto& shard : memoryShards_) {
        std::lock_guard shardLock(shard.mutex);
        stats.memoryHits += shard.hits;
        stats.memoryMisses += shard.misses;
        stats.memoryStores += shard.stores;
        stats.memoryEvictions += shard.evictions;
        stats.memoryBytes += shard.bytes;
    }
    return stats;
}

void PakReader::ClearMemoryCache()
{
    for (auto& shard : memoryShards_) {
        std::lock_guard shardLock(shard.mutex);
        shard.entries.clear();
        shard.lru.clear();
        shard.bytes = 0;
    }
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
    const PakCacheOptions& options = CacheOptions();
    return options.enabled &&
           options.persistentCacheEnabled &&
           options.persistentBudgetBytes > 0 &&
           !effectivePersistentCacheDirectory_.empty();
}

void PakReader::ResolvePersistentCacheDirectoryLocked() const
{
    effectivePersistentCacheDirectory_.clear();
    const PakCacheOptions& options = CacheOptions();
    if (!options.enabled || !options.persistentCacheEnabled ||
        options.persistentBudgetBytes == 0) {
        return;
    }

    if (!options.persistentCacheDirectory.empty()) {
        effectivePersistentCacheDirectory_ = options.persistentCacheDirectory;
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

void PakReader::TrimShardLocked(MemoryCacheShard& shard, uint64_t shardBudget) const
{
    if (shardBudget == 0) {
        shard.evictions += static_cast<uint64_t>(shard.entries.size());
        shard.entries.clear();
        shard.lru.clear();
        shard.bytes = 0;
        return;
    }

    while (shard.bytes > shardBudget && !shard.entries.empty()) {
        if (shard.lru.empty()) {
            // Keep the structures self-healing if invariants are ever broken
            // by a future cache mutation path.
            shard.evictions += static_cast<uint64_t>(shard.entries.size());
            shard.entries.clear();
            shard.bytes = 0;
            break;
        }
        const CacheKey oldestKey = shard.lru.back();
        auto oldest = shard.entries.find(oldestKey);
        if (oldest == shard.entries.end()) {
            shard.lru.pop_back();
            continue;
        }
        shard.bytes -= oldest->second.size;
        shard.lru.pop_back();
        shard.entries.erase(oldest);
        ++shard.evictions;
    }
}

void PakReader::TrimAllMemoryShards() const
{
    const PakCacheOptions& options = CacheOptions();
    const bool keepMemory = options.enabled && options.memoryCacheEnabled &&
                            options.memoryBudgetBytes > 0;
    const uint64_t shardBudget = keepMemory ? ShardBudget(options) : 0;

    for (auto& shard : memoryShards_) {
        std::lock_guard shardLock(shard.mutex);
        TrimShardLocked(shard, shardBudget);
    }
}

void PakReader::TrimPersistentCache() const
{
    std::string directory;
    uint64_t budget = 0;
    {
        std::lock_guard cacheLock(cacheMutex_);
        if (!HasPersistentCacheDirectoryLocked()) return;
        directory = effectivePersistentCacheDirectory_;
        budget = CacheOptions().persistentBudgetBytes;
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

bool PakReader::ShouldCacheDecoded(const PakFileInfo& entry,
    const ReadSnapshot& snapshot, const PakCacheOptions& options) const
{
    if (!options.enabled) return false;
    if (entry.originalSize == 0 || entry.originalSize > options.maxSingleEntryBytes) return false;
    if (!options.memoryCacheEnabled && !options.persistentCacheEnabled) return false;

    const bool compressed = PakInternal::IsCompressed(entry.flags);
    return compressed || !encryptionKey_.empty() || !snapshot.useMmap;
}

PakReader::CacheKey PakReader::MakeMemoryCacheKey(PakFileHandle handle,
    const ReadSnapshot& snapshot) noexcept
{
    // The decoded memory cache is reader-local and cleared whenever a new
    // archive replaces the current one. The archive fingerprint plus stable
    // handle index is therefore already a complete identity; hashing the
    // filename and entry metadata again on every cache hit is redundant.
    return CacheKey{snapshot.archiveFingerprint, static_cast<uint64_t>(handle.index)};
}

PakReader::CacheKey PakReader::MakePersistentCacheKey(PakFileHandle handle,
    const PakFileInfo& entry, const ReadSnapshot& snapshot) const
{
    uint64_t high = FNV_OFFSET_BASIS;
    uint64_t low = FNV_OFFSET_BASIS ^ 0x9e3779b97f4a7c15ull;

    HashValue(high, snapshot.archiveFingerprint);
    HashValue(high, handle.index);
    HashValue(high, entry.offset);
    HashValue(high, entry.originalSize);
    HashValue(high, entry.compressedSize);
    HashValue(high, entry.flags);
    HashString(high, entry.filename);

    HashValue(low, snapshot.archiveFingerprint);
    HashValue(low, entry.contentHash);
    uint64_t encryptionHash = FNV_OFFSET_BASIS;
    HashString(encryptionHash, encryptionKey_);
    HashValue(low, encryptionHash);
    HashValue(low, entry.originalSize);
    HashString(low, entry.filename);

    return CacheKey{high, low};
}

PakStatus PakReader::HashEntrySourceBytes(const PakFileInfo& entry,
    const ReadSnapshot& snapshot, uint64_t& outHash) const
{
    outHash = 0;
    const uint64_t diskSize = entry.compressedSize;
    if (diskSize == 0) {
        // Must match what the writer stamps for a zero-byte entry.
        outHash = HashBytesFast(nullptr, 0);
        return PakStatus::Ok;
    }

    if (entry.offset > snapshot.fileSize || diskSize > snapshot.fileSize - entry.offset) {
        return PakStatus::CorruptArchive;
    }

    if (snapshot.useMmap && snapshot.guardPtr && snapshot.guardPtr->mf.data) {
        const auto* mappedPtr = static_cast<const uint8_t*>(snapshot.guardPtr->mf.data) + entry.offset;
        outHash = HashBytesFast(mappedPtr, diskSize);
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
    outHash = HashBytesFast(source.data(), source.size());
    return PakStatus::Ok;
}

bool PakReader::TryGetMemoryCache(CacheKey key,
    std::shared_ptr<const std::vector<uint8_t>>& outData) const
{
    const PakCacheOptions& options = CacheOptions();
    if (!options.enabled || !options.memoryCacheEnabled ||
        options.memoryBudgetBytes == 0) {
        return false;
    }

    MemoryCacheShard& shard = memoryShards_[ShardIndexFor(key, ActiveShardCount(options))];
    std::lock_guard shardLock(shard.mutex);

    auto it = shard.entries.find(key);
    if (it == shard.entries.end()) {
        ++shard.misses;
        return false;
    }

    shard.lru.splice(shard.lru.begin(), shard.lru, it->second.lruPosition);
    it->second.lruPosition = shard.lru.begin();
    outData = it->second.data;
    ++shard.hits;
    return static_cast<bool>(outData);
}

void PakReader::StoreMemoryCache(CacheKey key,
    std::shared_ptr<const std::vector<uint8_t>> data) const
{
    if (!data) return;

    const PakCacheOptions& options = CacheOptions();
    const uint64_t shardBudget = ShardBudget(options);
    if (!options.enabled || !options.memoryCacheEnabled ||
        options.memoryBudgetBytes == 0 ||
        data->size() > options.maxSingleEntryBytes ||
        // An entry bigger than one shard's slice could never be retained, and
        // admitting it would evict that entire shard on its way back out. So
        // with sharding active the effective single-entry ceiling is
        // min(maxSingleEntryBytes, budget / shards) -- which also rules out
        // the pathological case of one entry evicting the whole cache.
        data->size() > shardBudget) {
        return;
    }

    const uint64_t size = static_cast<uint64_t>(data->size());
    MemoryCacheShard& shard = memoryShards_[ShardIndexFor(key, ActiveShardCount(options))];
    std::lock_guard shardLock(shard.mutex);

    auto it = shard.entries.find(key);
    if (it != shard.entries.end()) {
        shard.bytes -= it->second.size;
        shard.lru.splice(shard.lru.begin(), shard.lru, it->second.lruPosition);
        it->second = DecodedCacheEntry{std::move(data), size, shard.lru.begin()};
    } else {
        shard.lru.push_front(key);
        shard.entries.emplace(key,
            DecodedCacheEntry{std::move(data), size, shard.lru.begin()});
    }

    shard.bytes += size;
    ++shard.stores;
    TrimShardLocked(shard, shardBudget);
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
        if (header.dataSize > CacheOptions().maxSingleEntryBytes) {
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
            data.size() > CacheOptions().maxSingleEntryBytes ||
            data.size() > CacheOptions().persistentBudgetBytes) {
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
    const PakFileInfo& entry, std::span<uint8_t> destination,
    uint64_t* bytesWritten, const ReadSnapshot& snapshot) const
{
    if (bytesWritten) *bytesWritten = 0;

    PakStatus validation = ValidateReadRequest(entry, static_cast<uint64_t>(destination.size()), snapshot);
    if (validation != PakStatus::Ok) return validation;
    if (entry.originalSize == 0) return PakStatus::Ok;

    // verifyOnRead promises to hash the current source bytes on every call.
    // A decoded cache hit cannot satisfy that contract, so keep verification
    // on the direct source path regardless of cache policy.
    if (snapshot.verifyOnRead) {
        PakStatus status = ReadEntryToBuffer(entry, destination, bytesWritten, snapshot);
        if (status == PakStatus::Ok) {
            sourceReads_.Increment();
        }
        return status;
    }

    // Mapped, plain entries can never benefit from the decoded cache: the
    // mapping is already the cache and Read() only needs a memcpy. Avoid the
    // cache mutex and policy copy entirely on this dominant shipping path.
    if (snapshot.useMmap && !PakInternal::IsCompressed(entry.flags) &&
        encryptionKey_.empty()) {
        PakStatus status = ReadEntryToBuffer(entry, destination, bytesWritten, snapshot);
        if (status == PakStatus::Ok) {
            sourceReads_.Increment();
        }
        return status;
    }

    // Read straight from the published policy: no lock, and no copy of a
    // struct that owns a std::string.
    const PakCacheOptions& options = CacheOptions();

    if (!ShouldCacheDecoded(entry, snapshot, options)) {
        PakStatus status = ReadEntryToBuffer(entry, destination, bytesWritten, snapshot);
        if (status == PakStatus::Ok) {
            sourceReads_.Increment();
        }
        return status;
    }

    CacheKey memoryKey = MakeMemoryCacheKey(handle, snapshot);
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
        persistentKey = MakePersistentCacheKey(handle, entry, snapshot);
        havePersistentKey = true;
        if (TryLoadPersistentCache(persistentKey, cached)) {
            if (cached->size() != entry.originalSize) return PakStatus::CorruptArchive;
            std::memcpy(destination.data(), cached->data(), cached->size());
            StoreMemoryCache(memoryKey, cached);
            if (bytesWritten) *bytesWritten = entry.originalSize;
            return PakStatus::Ok;
        }
    }

    PakStatus status = ReadEntryToBuffer(entry, destination, bytesWritten, snapshot);
    if (status != PakStatus::Ok) return status;

    sourceReads_.Increment();

    auto output = destination.first(static_cast<size_t>(entry.originalSize));
    auto stored = std::make_shared<std::vector<uint8_t>>(output.begin(), output.end());
    StoreMemoryCache(memoryKey, stored);
    if (havePersistentKey) {
        StorePersistentCache(persistentKey, *stored);
    }
    return PakStatus::Ok;
}
