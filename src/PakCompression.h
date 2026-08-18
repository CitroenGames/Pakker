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

// Compresses [src, src+srcSize) into a chunked payload: a PakChunkHeader,
// then one uint32 compressed size per chunk, then each chunk's bytes back to
// back. Each chunk is compressed independently, so any one of them can be
// decoded without touching the others -- that is what makes ReadRange() work
// on a compressed entry and what bounds time-to-first-byte on a large asset.
//
// Chunks that do not compress are stored raw (compressed size == uncompressed
// size), so incompressible blocks never pay an expansion penalty. Follows the
// same "not smaller, so store the entry uncompressed" contract as
// CompressBuffer(): if the assembled payload is not smaller than the input,
// outBuffer is cleared and outFlags is set to 0.
bool CompressChunked(PakCompression method, int zstdLevel, uint32_t chunkSize,
                     const uint8_t* src, uint64_t srcSize,
                     std::vector<uint8_t>& outBuffer, uint8_t& outFlags);

// Validates a chunked payload's header and per-chunk size table against
// payloadSize and originalSize. Returns PakStatus::Ok and fills outHeader and
// outCompressedSizes (pointing into the payload) when the layout is sound.
PakStatus ParseChunkTable(const uint8_t* payload, uint64_t payloadSize,
                          uint64_t originalSize, PakChunkHeader& outHeader,
                          const uint32_t*& outCompressedSizes,
                          uint64_t& outFirstChunkOffset);

// Decodes [rangeOffset, rangeOffset+rangeSize) of a chunked entry into `dst`,
// touching only the chunks that range actually covers. `scratch` is reused
// across chunks so a range read does not allocate per block.
PakStatus DecompressChunkedRange(uint8_t flags, const uint8_t* payload, uint64_t payloadSize,
                                 uint64_t originalSize, uint64_t rangeOffset,
                                 uint8_t* dst, uint64_t rangeSize,
                                 std::vector<uint8_t>& scratch);

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
