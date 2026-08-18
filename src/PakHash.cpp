// Fast hashing, confined to a single translation unit.
//
// xxHash ships inside the vendored zstd tree and is already compiled into
// this library (vendor/zstd/common/xxhash.c). Keeping the include here --
// the same way PakCompression.cpp confines lz4.h and zstd.h -- stops the
// vendored headers from leaking into PakInternal.h, which is included far
// more widely.
//
// Two details of zstd's local adaptation drive what is used here:
//
//   * it vendors xxHash with XXH_NAMESPACE=ZSTD_, so the names below resolve
//     to ZSTD_XXH64 at link time -- the same symbol zstd itself already pulls
//     in, so there is no second copy of the implementation;
//   * it hard-defines XXH_NO_XXH3, compiling the XXH3 family out entirely.
//
// So this uses XXH64 rather than the newer XXH3. XXH64 runs at roughly
// 13 GB/s against the ~0.8 GB/s of the byte-at-a-time FNV-1a it replaces,
// which is the difference that matters; reaching for XXH3's extra 2-3x would
// mean patching vendored third-party code for no practical gain.

#include "Pak.h"
#include "PakInternal.h"

#include "vendor/zstd/common/xxhash.h"

uint64_t PakPathHash(std::string_view path) noexcept
{
    return static_cast<uint64_t>(XXH64(path.data(), path.size(), 0));
}

namespace PakInternal {

uint64_t HashBytesFast(const void* data, size_t size) noexcept
{
    return static_cast<uint64_t>(XXH64(data, size, 0));
}

} // namespace PakInternal
