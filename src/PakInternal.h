#ifndef PAK_INTERNAL_H
#define PAK_INTERNAL_H

// Private implementation header. Not part of the public API -- consumers of
// the library should only ever include Pak.h. This exists solely to share a
// handful of helpers across PakBuilder.cpp, PakReaderCore.cpp, and
// PakReaderCache.cpp without duplicating them.

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <filesystem>
#include <system_error>
#include <sstream>
#include <iomanip>

namespace PakInternal {

// Maximum input size for LZ4 functions (from LZ4_MAX_INPUT_SIZE). Shared by
// the builder's compress path and the reader's decompress path.
static constexpr uint64_t LZ4_MAX_SAFE_SIZE = 0x7E000000;

// FNV-1a fingerprint helpers used by PakReader for its archive fingerprint
// (Open()) and its decoded-cache-key generation (MakeCacheKey,
// HashEntrySourceBytes).
static constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ull;
static constexpr uint64_t FNV_PRIME = 1099511628211ull;

inline void HashBytes(uint64_t& hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= FNV_PRIME;
    }
}

template <typename T>
inline void HashValue(uint64_t& hash, const T& value)
{
    HashBytes(hash, &value, sizeof(value));
}

inline void HashString(uint64_t& hash, std::string_view value)
{
    uint64_t size = static_cast<uint64_t>(value.size());
    HashValue(hash, size);
    if (!value.empty()) HashBytes(hash, value.data(), value.size());
}

inline uint64_t HashBuffer(const uint8_t* data, uint64_t size)
{
    uint64_t hash = FNV_OFFSET_BASIS;
    if (data && size > 0) HashBytes(hash, data, static_cast<size_t>(size));
    return hash;
}

inline std::string Hex64(uint64_t value)
{
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << value;
    return stream.str();
}

inline uint64_t FileTimeFingerprint(const std::filesystem::path& path)
{
    std::error_code ec;
    auto time = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<uint64_t>(time.time_since_epoch().count());
}

} // namespace PakInternal

#endif // PAK_INTERNAL_H
