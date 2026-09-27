#include "Pak.h"
#include "PakInternal.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

using namespace PakInternal;

// ===========================================================================
// PakMount -- layered virtual filesystem over multiple PakReader archives
// ===========================================================================

namespace {

namespace fs = std::filesystem;

bool NeedsPathNormalization(std::string_view path)
{
    return path.find('\\') != std::string_view::npos ||
           (!path.empty() && path.front() == '/');
}

// Keeps the open-addressing table at most ~70% full. Linear probing degrades
// sharply past that, and the slots are small enough that the headroom is
// cheap: 16 bytes per slot against ~50 bytes for the path string the old
// index stored per entry per layer.
constexpr uint64_t kMaxLoadNumerator = 7;
constexpr uint64_t kMaxLoadDenominator = 10;

uint64_t NextPowerOfTwo(uint64_t value)
{
    uint64_t result = 16;
    while (result < value) result <<= 1;
    return result;
}

std::string ArchiveIdentity(const std::string& filename, const PakOpenOptions& options,
                            const std::string& encryptionKey)
{
    std::error_code ec;
    fs::path path = fs::weakly_canonical(fs::path(filename), ec);
    if (ec) {
        ec.clear();
        path = fs::absolute(fs::path(filename), ec);
        if (ec) path = fs::path(filename);
    }
    std::string identity = path.lexically_normal().generic_string();
#ifdef _WIN32
    std::transform(identity.begin(), identity.end(), identity.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    identity.push_back('\0');
    identity.append(encryptionKey);
    identity.push_back('\0');
    identity.push_back(options.verifyOnRead ? '\1' : '\0');
    identity.push_back(options.loadNames ? '\1' : '\0');
    return identity;
}

} // namespace

PakMount::PakMount(PakMount&& other) noexcept
{
    std::unique_lock lock(other.mutex_);
    layers_ = std::move(other.layers_);
    layerIdentities_ = std::move(other.layerIdentities_);
    layerRefCounts_ = std::move(other.layerRefCounts_);
    generation_ = other.generation_;
    indexOwner_ = std::move(other.indexOwner_);
    index_.store(other.index_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    other.index_.store(nullptr, std::memory_order_relaxed);
    other.layers_.clear();
    other.layerIdentities_.clear();
    other.layerRefCounts_.clear();
}

PakMount& PakMount::operator=(PakMount&& other) noexcept
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

        layers_ = std::move(other.layers_);
        layerIdentities_ = std::move(other.layerIdentities_);
        layerRefCounts_ = std::move(other.layerRefCounts_);
        generation_ = other.generation_;
        indexOwner_ = std::move(other.indexOwner_);
        index_.store(other.index_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        other.index_.store(nullptr, std::memory_order_relaxed);
        other.layers_.clear();
        other.layerIdentities_.clear();
        other.layerRefCounts_.clear();
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Merged index construction
// ---------------------------------------------------------------------------

void PakMount::GrowIndex(MergedIndex& index, uint32_t additionalEntries)
{
    const uint64_t needed = static_cast<uint64_t>(index.count) + additionalEntries;
    const uint64_t required = (needed * kMaxLoadDenominator + kMaxLoadNumerator - 1) /
                              kMaxLoadNumerator;
    if (!index.slots.empty() && index.slots.size() >= required) return;

    std::vector<MergedSlot> oldSlots = std::move(index.slots);

    const uint64_t slotCount = NextPowerOfTwo(required);
    index.slots.assign(static_cast<size_t>(slotCount), MergedSlot{});
    index.mask = slotCount - 1;

    for (const MergedSlot& slot : oldSlots) {
        if (slot.Empty()) continue;
        uint64_t position = slot.pathHash & index.mask;
        while (!index.slots[static_cast<size_t>(position)].Empty()) {
            position = (position + 1) & index.mask;
        }
        index.slots[static_cast<size_t>(position)] = slot;
    }
}

PakMount::MergedSlot PakMount::InsertSlot(MergedIndex& index, uint64_t pathHash,
                                          uint32_t layerIndex, PakFileHandle fileHandle)
{
    uint64_t position = pathHash & index.mask;
    while (true) {
        MergedSlot& slot = index.slots[static_cast<size_t>(position)];
        if (slot.Empty()) {
            slot.pathHash = pathHash;
            slot.layerIndex = layerIndex;
            slot.fileHandle = fileHandle;
            ++index.count;
            return MergedSlot{};
        }
        if (slot.pathHash == pathHash) {
            // Same path hash already present: normally the mount override
            // rule (a higher-priority layer replacing a lower one). Hand the
            // previous occupant back so the caller can tell that apart from a
            // true hash collision without probing a second time.
            const MergedSlot previous = slot;
            slot.layerIndex = layerIndex;
            slot.fileHandle = fileHandle;
            return previous;
        }
        position = (position + 1) & index.mask;
    }
}

void PakMount::OverlayLayer(MergedIndex& index, uint32_t layerIndex, PakReader& reader)
{
    const uint32_t count = reader.GetFileCount();
    GrowIndex(index, count);

    for (uint32_t i = 0; i < count; ++i) {
        const PakFileInfo* info = reader.InfoByIndex(i);
        if (!info) continue;

        // The archive stores this hash, so mounting never re-hashes a path.
        // It is also what lets a layer mounted without its name blob still
        // participate in the merged namespace.
        const MergedSlot replaced = InsertSlot(index, info->pathHash, layerIndex,
                                               PakFileHandle{i});

        // A genuine 64-bit hash collision between two different paths would
        // silently shadow one asset with another, which is close to
        // undebuggable from the outside. Turning that into a loud error costs
        // one string compare, and only on a slot that was already occupied --
        // which is almost always just the ordinary override case.
        if (replaced.Empty()) continue;

        PakReader* owner = index.layerReaders[replaced.layerIndex];
        const PakFileInfo* replacedInfo = owner
            ? owner->InfoByIndex(replaced.fileHandle.index) : nullptr;
        if (replacedInfo && !replacedInfo->filename.empty() &&
            !info->filename.empty() &&
            replacedInfo->filename != info->filename) {
            Log(PakLogLevel::Error,
                "PakMount: path hash collision between '" +
                std::string(replacedInfo->filename) + "' and '" +
                std::string(info->filename) + "'; the latter wins.");
        }
    }
}

const PakMount::MergedSlot* PakMount::ProbeIndex(const MergedIndex& index, uint64_t pathHash)
{
    if (index.slots.empty()) return nullptr;

    uint64_t position = pathHash & index.mask;
    while (true) {
        const MergedSlot& slot = index.slots[static_cast<size_t>(position)];
        if (slot.Empty()) return nullptr;
        if (slot.pathHash == pathHash) return &slot;
        position = (position + 1) & index.mask;
    }
}

void PakMount::PublishIndexLocked(std::shared_ptr<const MergedIndex> index)
{
    indexOwner_ = std::move(index);
    index_.store(indexOwner_.get(), std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Mounting
// ---------------------------------------------------------------------------

bool PakMount::Mount(const std::string& pakFilename, const std::string& encryptionKey)
{
    return Mount(pakFilename, PakOpenOptions{}, encryptionKey);
}

bool PakMount::Mount(const std::string& pakFilename, const PakOpenOptions& options,
                     const std::string& encryptionKey)
{
    const std::string identity = ArchiveIdentity(pakFilename, options, encryptionKey);
    {
        std::unique_lock lock(mutex_);
        for (size_t i = 0; i < layerIdentities_.size(); ++i) {
            if (layerIdentities_[i] == identity) {
                ++layerRefCounts_[i];
                return true;
            }
        }
    }

    auto reader = std::make_shared<PakReader>(encryptionKey);
    if (!reader->Open(pakFilename, options)) return false;

    std::unique_lock lock(mutex_);
    return MountReaderLocked(std::move(reader), identity);
}

bool PakMount::MountReader(std::shared_ptr<PakReader> reader)
{
    if (!reader || !reader->IsOpen()) return false;

    std::unique_lock lock(mutex_);
    for (size_t i = 0; i < layers_.size(); ++i) {
        if (layers_[i].get() == reader.get()) {
            ++layerRefCounts_[i];
            return true;
        }
    }
    return MountReaderLocked(std::move(reader), {});
}

bool PakMount::MountReaderLocked(std::shared_ptr<PakReader> reader, std::string identity)
{
    if (!identity.empty()) {
        for (size_t i = 0; i < layerIdentities_.size(); ++i) {
            if (layerIdentities_[i] == identity) {
                ++layerRefCounts_[i];
                return true;
            }
        }
    }

    // Copy the previous index forward and overlay only the incoming layer.
    // The copy is a flat vector memcpy rather than a per-entry rehash, which
    // is what keeps mounting linear in total entries instead of quadratic.
    auto merged = indexOwner_
        ? std::make_shared<MergedIndex>(*indexOwner_)
        : std::make_shared<MergedIndex>();

    const uint32_t layerIndex = static_cast<uint32_t>(layers_.size());
    merged->layerReaders.push_back(reader.get());
    merged->generation = generation_;
    OverlayLayer(*merged, layerIndex, *reader);

    layers_.push_back(std::move(reader));
    layerIdentities_.push_back(std::move(identity));
    layerRefCounts_.push_back(1);
    PublishIndexLocked(std::move(merged));
    return true;
}

bool PakMount::Unmount(const std::string& pakFilename, const std::string& encryptionKey)
{
    return Unmount(pakFilename, PakOpenOptions{}, encryptionKey);
}

bool PakMount::Unmount(const std::string& pakFilename, const PakOpenOptions& options,
                       const std::string& encryptionKey)
{
    const std::string identity = ArchiveIdentity(pakFilename, options, encryptionKey);
    std::unique_lock lock(mutex_);
    for (size_t i = 0; i < layerIdentities_.size(); ++i) {
        if (layerIdentities_[i] == identity) return RemoveLayerLocked(i);
    }
    return false;
}

bool PakMount::UnmountReader(const std::shared_ptr<PakReader>& reader)
{
    if (!reader) return false;
    std::unique_lock lock(mutex_);
    for (size_t i = 0; i < layers_.size(); ++i) {
        if (layers_[i].get() == reader.get()) return RemoveLayerLocked(i);
    }
    return false;
}

bool PakMount::RemoveLayerLocked(size_t layerIndex)
{
    if (layerIndex >= layers_.size()) return false;
    if (layerRefCounts_[layerIndex] > 1) {
        --layerRefCounts_[layerIndex];
        return true;
    }

    layers_.erase(layers_.begin() + static_cast<std::ptrdiff_t>(layerIndex));
    layerIdentities_.erase(layerIdentities_.begin() + static_cast<std::ptrdiff_t>(layerIndex));
    layerRefCounts_.erase(layerRefCounts_.begin() + static_cast<std::ptrdiff_t>(layerIndex));
    ++generation_;
    if (generation_ == 0) ++generation_;
    RebuildIndexLocked();
    return true;
}

void PakMount::RebuildIndexLocked()
{
    if (layers_.empty()) {
        index_.store(nullptr, std::memory_order_release);
        indexOwner_.reset();
        return;
    }

    auto merged = std::make_shared<MergedIndex>();
    merged->generation = generation_;
    merged->layerReaders.reserve(layers_.size());
    for (const auto& layer : layers_) merged->layerReaders.push_back(layer.get());
    for (uint32_t i = 0; i < layers_.size(); ++i) OverlayLayer(*merged, i, *layers_[i]);
    PublishIndexLocked(std::move(merged));
}

void PakMount::Clear()
{
    std::unique_lock lock(mutex_);
    index_.store(nullptr, std::memory_order_release);
    indexOwner_.reset();
    layers_.clear();
    layerIdentities_.clear();
    layerRefCounts_.clear();
    ++generation_;
    if (generation_ == 0) ++generation_;
}

size_t PakMount::LayerCount() const
{
    std::shared_lock lock(mutex_);
    return layers_.size();
}

std::shared_ptr<PakReader> PakMount::GetLayerReader(size_t layerIndex) const
{
    std::shared_lock lock(mutex_);
    if (layerIndex >= layers_.size()) return nullptr;
    return layers_[layerIndex];
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

PakMountHandle PakMount::FindInIndex(const MergedIndex& index, std::string_view filename)
{
    if (filename.empty()) return {};

    const MergedSlot* slot = ProbeIndex(index, PakPathHash(filename));
    if (!slot) return {};

    // Confirm the name, so a hash collision cannot hand back the wrong asset.
    // Archives opened without a resident name blob report an empty filename;
    // there the 64-bit hash is all there is to go on, which is the documented
    // trade for dropping the names.
    PakReader* owner = index.layerReaders[slot->layerIndex];
    if (owner) {
        const PakFileInfo* info = owner->InfoByIndex(slot->fileHandle.index);
        if (info && !info->filename.empty() && info->filename != filename) return {};
    }

    return PakMountHandle{slot->layerIndex, slot->fileHandle, index.generation};
}

PakMountHandle PakMount::Find(std::string_view filename) const
{
    const MergedIndex* index = AcquireIndex();
    if (!index) return {};

    if (NeedsPathNormalization(filename)) {
        return FindInIndex(*index, NormalizePathSeparators(std::string(filename)));
    }
    return FindInIndex(*index, filename);
}

size_t PakMount::Resolve(std::span<const std::string_view> filenames,
                         std::span<PakMountHandle> handles) const
{
    size_t resolvedCount = 0;
    size_t count = std::min(filenames.size(), handles.size());

    const MergedIndex* index = AcquireIndex();
    if (!index) {
        for (size_t i = 0; i < count; ++i) handles[i] = {};
        return 0;
    }

    for (size_t i = 0; i < count; ++i) {
        std::string_view name = filenames[i];
        handles[i] = NeedsPathNormalization(name)
            ? FindInIndex(*index, NormalizePathSeparators(std::string(name)))
            : FindInIndex(*index, name);
        if (handles[i]) ++resolvedCount;
    }
    return resolvedCount;
}

// ---------------------------------------------------------------------------
// Handle dispatch -- resolve the winning layer from the published index and
// forward straight into that reader's own handle-based method. No lock and
// no reference count on the way through; see PakReader::ReadSnapshot for the
// reasoning.
// ---------------------------------------------------------------------------

const PakFileInfo* PakMount::Info(PakMountHandle handle) const
{
    const MergedIndex* index = AcquireIndex();
    if (!index || !handle || handle.generation != index->generation ||
        handle.layerIndex >= index->layerReaders.size()) return nullptr;
    return index->layerReaders[handle.layerIndex]->Info(handle.fileHandle);
}

PakStatus PakMount::View(PakMountHandle handle, PakView& outView) const
{
    const MergedIndex* index = AcquireIndex();
    if (!index || !handle || handle.generation != index->generation ||
        handle.layerIndex >= index->layerReaders.size())
        return PakStatus::InvalidHandle;
    return index->layerReaders[handle.layerIndex]->View(handle.fileHandle, outView);
}

PakStatus PakMount::Read(PakMountHandle handle, std::span<uint8_t> destination,
                         uint64_t* bytesWritten) const
{
    const MergedIndex* index = AcquireIndex();
    if (!index || !handle || handle.generation != index->generation ||
        handle.layerIndex >= index->layerReaders.size())
        return PakStatus::InvalidHandle;
    return index->layerReaders[handle.layerIndex]->Read(handle.fileHandle, destination, bytesWritten);
}

uint32_t PakMount::LayerRefCount(size_t layerIndex) const
{
    std::shared_lock lock(mutex_);
    return layerIndex < layerRefCounts_.size() ? layerRefCounts_[layerIndex] : 0;
}

PakStatus PakMount::ReadRange(PakMountHandle handle, uint64_t rangeOffset,
                              std::span<uint8_t> destination, uint64_t* bytesWritten) const
{
    const MergedIndex* index = AcquireIndex();
    if (!index || !handle || handle.generation != index->generation ||
        handle.layerIndex >= index->layerReaders.size()) {
        if (bytesWritten) *bytesWritten = 0;
        return PakStatus::InvalidHandle;
    }
    return index->layerReaders[handle.layerIndex]->ReadRange(
        handle.fileHandle, rangeOffset, destination, bytesWritten);
}

PakStatus PakMount::Load(PakMountHandle handle, std::vector<uint8_t>& outData) const
{
    const MergedIndex* index = AcquireIndex();
    if (!index || !handle || handle.generation != index->generation ||
        handle.layerIndex >= index->layerReaders.size()) {
        outData.clear();
        return PakStatus::InvalidHandle;
    }
    return index->layerReaders[handle.layerIndex]->Load(handle.fileHandle, outData);
}

PakStatus PakMount::Prefetch(PakMountHandle handle) const
{
    const MergedIndex* index = AcquireIndex();
    if (!index || !handle || handle.generation != index->generation ||
        handle.layerIndex >= index->layerReaders.size())
        return PakStatus::InvalidHandle;
    return index->layerReaders[handle.layerIndex]->Prefetch(handle.fileHandle);
}

PakStatus PakMount::VerifyEntry(PakMountHandle handle) const
{
    const MergedIndex* index = AcquireIndex();
    if (!index || !handle || handle.generation != index->generation ||
        handle.layerIndex >= index->layerReaders.size())
        return PakStatus::InvalidHandle;
    return index->layerReaders[handle.layerIndex]->VerifyEntry(handle.fileHandle);
}

// ---------------------------------------------------------------------------
// Convenience wrappers
// ---------------------------------------------------------------------------

std::vector<uint8_t> PakMount::ReadFile(const std::string& filename) const
{
    PakMountHandle handle = Find(filename);
    if (!handle) return {};
    std::vector<uint8_t> data;
    if (Load(handle, data) != PakStatus::Ok) return {};
    return data;
}

std::shared_ptr<std::vector<uint8_t>> PakMount::LoadFile(const std::string& filename) const
{
    PakMountHandle handle = Find(filename);
    if (!handle) return nullptr;
    auto data = std::make_shared<std::vector<uint8_t>>();
    return Load(handle, *data) == PakStatus::Ok ? data : nullptr;
}

PakSpan PakMount::ReadFileZeroCopy(const std::string& filename) const
{
    // PakSpan's mapping-lifetime fields are private to PakReader (friend
    // class), so PakMount cannot assemble one directly -- determine which
    // layer owns this file, then delegate to that layer's own
    // ReadFileZeroCopy(), which can.
    PakMountHandle handle = Find(filename);
    if (!handle) return PakSpan{};

    const MergedIndex* index = AcquireIndex();
    if (!index || handle.generation != index->generation ||
        handle.layerIndex >= index->layerReaders.size()) return PakSpan{};
    return index->layerReaders[handle.layerIndex]->ReadFileZeroCopy(filename);
}

bool PakMount::FileExists(std::string_view filename) const
{
    return static_cast<bool>(Find(filename));
}

uint32_t PakMount::GetFileCount() const
{
    const MergedIndex* index = AcquireIndex();
    return index ? index->count : 0;
}

// ---------------------------------------------------------------------------
// Enumeration
//
// Names are no longer cached in the index, so these materialize and sort on
// demand. That is a deliberate trade: enumeration is a tools/debug path,
// while mounting is on the critical path of every level load.
// ---------------------------------------------------------------------------

std::vector<std::string> PakMount::ListFiles() const
{
    const MergedIndex* index = AcquireIndex();
    if (!index) return {};

    std::vector<std::string> files;
    files.reserve(index->count);
    for (const MergedSlot& slot : index->slots) {
        if (slot.Empty()) continue;
        PakReader* owner = index->layerReaders[slot.layerIndex];
        if (!owner) continue;
        const PakFileInfo* info = owner->InfoByIndex(slot.fileHandle.index);
        if (!info || info->filename.empty()) continue;
        files.emplace_back(info->filename);
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<std::string> PakMount::ListFilesWithPrefix(const std::string& prefix) const
{
    const std::string normalizedPrefix = NormalizePathSeparators(prefix);
    std::vector<std::string> files = ListFiles();

    // ListFiles() returns sorted names, so matches form one contiguous run
    // starting at the first name >= the prefix.
    auto begin = std::lower_bound(files.begin(), files.end(), normalizedPrefix);

    std::vector<std::string> matches;
    for (auto it = begin; it != files.end() && it->starts_with(normalizedPrefix); ++it) {
        matches.push_back(std::move(*it));
    }
    return matches;
}
