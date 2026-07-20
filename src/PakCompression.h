#ifndef PAK_COMPRESSION_H
#define PAK_COMPRESSION_H

// Private implementation header. Not part of the public API. Contains the
// LZ4/Zstd compress+decompress dispatch shared by PakBuilder.cpp (build-time
// writer) and PakReaderCore.cpp (runtime reader), so the vendored codec
// headers (vendor/lz4.h, vendor/zstd/zstd.h) stay confined to
// PakCompression.cpp's single translation unit instead of leaking into
// PakInternal.h, which is transitively included much more widely.

#include "Pak.h"
#include <cstdint>
#include <vector>

namespace PakInternal {

// Compresses [src, src+srcSize) per `method`. On success, and when the
// result is actually smaller than the original, resizes `outBuffer` to hold
// the compressed bytes and sets `outFlags` to the corresponding
// PAK_FLAG_*_COMPRESSED bit. If compression isn't beneficial (result >=
// original), not applicable (method == PakCompression::None, empty input),
// or the input exceeds MAX_COMPRESSIBLE_ENTRY_SIZE, `outBuffer` is left
// untouched and `outFlags` is set to 0 -- callers should fall back to
// storing the original bytes uncompressed, matching this project's existing
// "store uncompressed if not smaller" behavior. Returns false only on a hard
// compressor failure (should not normally happen).
bool CompressBuffer(PakCompression method, int zstdLevel,
                     const uint8_t* src, uint64_t srcSize,
                     std::vector<uint8_t>& outBuffer, uint8_t& outFlags);

// Decompresses `src` (srcSize on-disk bytes, whose method is encoded in
// `flags`) into the caller-provided `dst` buffer, which must be exactly
// `originalSize` bytes. Writes directly into `dst` (no extra allocation) so
// it drops into a destination buffer/span the caller already owns.
// Returns PakStatus::Ok, PakStatus::DecompressionFailed (codec rejected the
// input), or PakStatus::CorruptArchive (srcSize/originalSize exceed the
// codec-neutral size ceiling).
PakStatus DecompressBuffer(uint8_t flags, const uint8_t* src, uint64_t srcSize,
                            uint8_t* dst, uint64_t originalSize);

} // namespace PakInternal

#endif // PAK_COMPRESSION_H
