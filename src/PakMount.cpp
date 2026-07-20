#include "Pak.h"
#include "PakInternal.h"
#include <algorithm>

using namespace PakInternal;

// ===========================================================================
// PakMount -- layered virtual filesystem over multiple PakReader archives
// ===========================================================================

namespace {

bool NeedsPathNormalization(std::string_view path)
{
    return path.find('\\') != std::string_view::npos ||
           (!path.empty() && path.front() == '/');
}

} // namespace

PakMount::PakMount(PakMount&& other) noexcept
{
    std::unique_lock lock(other.mutex_);
    layers_ = std::move(other.layers_);
    index_ = std::move(other.index_);
    other.layers_.clear();
    other.index_.reset();
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
        index_ = std::move(other.index_);
        other.layers_.clear();
        other.index_.reset();
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Mounting
// ---------------------------------------------------------------------------

bool PakMount::Mount(const std::string& pakFilename, const std::string& encryptionKey)
{
    auto reader = std::make_shared<PakReader>(encryptionKey);
    if (!reader->Open(pakFilename)) return false;
    return MountReader(std::move(reader));
}

bool PakMount::MountReader(std::shared_ptr<PakReader> reader)
{
    if (!reader || !reader->IsOpen()) return false;

    std::unique_lock lock(mutex_);
    layers_.push_back(std::move(reader));
    RebuildMergedIndexLocked();
    return true;
}

void PakMount::Clear()
{
    std::unique_lock lock(mutex_);
    layers_.clear();
    index_.reset();
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

void PakMount::RebuildMergedIndexLocked()
{
    auto newIndex = std::make_shared<MergedIndex>();

    // Low-to-high priority so later (higher-index / more-recently-mounted)
    // layers naturally overwrite earlier ones on name conflicts.
    for (uint32_t layerIdx = 0; layerIdx < layers_.size(); ++layerIdx) {
        const auto& reader = layers_[layerIdx];
        uint32_t count = reader->GetFileCount();
        for (uint32_t i = 0; i < count; ++i) {
            const PakFileInfo* info = reader->InfoByIndex(i);
            if (!info) continue;
            newIndex->byName[std::string(info->filename)] =
                MergedEntry{layerIdx, PakFileHandle{i}};
        }
    }

    newIndex->sortedNames.reserve(newIndex->byName.size());
    for (const auto& [name, entry] : newIndex->byName) {
        newIndex->sortedNames.push_back(name);
    }
    std::sort(newIndex->sortedNames.begin(), newIndex->sortedNames.end());

    index_ = std::move(newIndex);
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

PakMountHandle PakMount::FindInIndex(const MergedIndex& index, std::string_view filename)
{
    if (filename.empty()) return {};

    auto it = index.byName.find(filename);
    if (it == index.byName.end()) return {};
    return PakMountHandle{it->second.layerIndex, it->second.fileHandle};
}

PakMountHandle PakMount::Find(std::string_view filename) const
{
    std::shared_ptr<const MergedIndex> index;
    {
        std::shared_lock lock(mutex_);
        index = index_;
    }
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

    std::shared_ptr<const MergedIndex> index;
    {
        std::shared_lock lock(mutex_);
        index = index_;
    }
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
// Handle dispatch -- snapshot the winning layer's reader, then forward
// straight into its own handle-based method. Mirrors PakReader's own
// snapshot-then-release discipline: the mutex_ is held only long enough to
// copy a shared_ptr, never during I/O or decompression.
// ---------------------------------------------------------------------------

const PakFileInfo* PakMount::Info(PakMountHandle handle) const
{
    std::shared_ptr<PakReader> reader;
    {
        std::shared_lock lock(mutex_);
        if (!handle || handle.layerIndex >= layers_.size()) return nullptr;
        reader = layers_[handle.layerIndex];
    }
    return reader->Info(handle.fileHandle);
}

PakStatus PakMount::View(PakMountHandle handle, PakView& outView) const
{
    std::shared_ptr<PakReader> reader;
    {
        std::shared_lock lock(mutex_);
        if (!handle || handle.layerIndex >= layers_.size()) return PakStatus::InvalidHandle;
        reader = layers_[handle.layerIndex];
    }
    return reader->View(handle.fileHandle, outView);
}

PakStatus PakMount::Read(PakMountHandle handle, std::span<uint8_t> destination,
                         uint64_t* bytesWritten) const
{
    std::shared_ptr<PakReader> reader;
    {
        std::shared_lock lock(mutex_);
        if (!handle || handle.layerIndex >= layers_.size()) return PakStatus::InvalidHandle;
        reader = layers_[handle.layerIndex];
    }
    return reader->Read(handle.fileHandle, destination, bytesWritten);
}

PakStatus PakMount::Load(PakMountHandle handle, std::vector<uint8_t>& outData) const
{
    std::shared_ptr<PakReader> reader;
    {
        std::shared_lock lock(mutex_);
        if (!handle || handle.layerIndex >= layers_.size()) {
            outData.clear();
            return PakStatus::InvalidHandle;
        }
        reader = layers_[handle.layerIndex];
    }
    return reader->Load(handle.fileHandle, outData);
}

PakStatus PakMount::Prefetch(PakMountHandle handle) const
{
    std::shared_ptr<PakReader> reader;
    {
        std::shared_lock lock(mutex_);
        if (!handle || handle.layerIndex >= layers_.size()) return PakStatus::InvalidHandle;
        reader = layers_[handle.layerIndex];
    }
    return reader->Prefetch(handle.fileHandle);
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
    std::shared_ptr<PakReader> reader;
    {
        PakMountHandle handle = Find(filename);
        if (!handle) return PakSpan{};
        std::shared_lock lock(mutex_);
        if (handle.layerIndex >= layers_.size()) return PakSpan{};
        reader = layers_[handle.layerIndex];
    }
    return reader->ReadFileZeroCopy(filename);
}

bool PakMount::FileExists(std::string_view filename) const
{
    return static_cast<bool>(Find(filename));
}

uint32_t PakMount::GetFileCount() const
{
    std::shared_ptr<const MergedIndex> index;
    {
        std::shared_lock lock(mutex_);
        index = index_;
    }
    return index ? static_cast<uint32_t>(index->byName.size()) : 0;
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

std::vector<std::string> PakMount::ListFiles() const
{
    std::shared_ptr<const MergedIndex> index;
    {
        std::shared_lock lock(mutex_);
        index = index_;
    }
    if (!index) return {};
    return index->sortedNames;
}

std::vector<std::string> PakMount::ListFilesWithPrefix(const std::string& prefix) const
{
    std::shared_ptr<const MergedIndex> index;
    {
        std::shared_lock lock(mutex_);
        index = index_;
    }
    if (!index) return {};

    std::string normalizedPrefix = NormalizePathSeparators(prefix);

    // sortedNames is sorted, so every match forms one contiguous run
    // starting at the first name >= the prefix -- binary search straight to
    // it instead of an O(n) scan.
    auto begin = std::lower_bound(index->sortedNames.begin(), index->sortedNames.end(),
                                   normalizedPrefix);

    std::vector<std::string> matches;
    for (auto it = begin; it != index->sortedNames.end() && it->starts_with(normalizedPrefix); ++it) {
        matches.push_back(*it);
    }
    return matches;
}
