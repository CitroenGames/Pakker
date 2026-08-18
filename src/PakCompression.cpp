#include "PakCompression.h"
#include "PakInternal.h"
#include <cstring>
#include <algorithm>

#include "vendor/lz4.h"
#include "vendor/zstd/zstd.h"

namespace PakInternal {

bool CompressBuffer(PakCompression method, int zstdLevel,
                     const uint8_t* src, uint64_t srcSize,
                     std::vector<uint8_t>& outBuffer, uint8_t& outFlags)
{
    outFlags = 0;

    if (method == PakCompression::None || srcSize == 0) {
        return true;
    }
    if (srcSize > MAX_COMPRESSIBLE_ENTRY_SIZE) {
        Log(PakLogLevel::Warning, "CompressBuffer: Input too large to compress, storing uncompressed.");
        return true;
    }

    if (method == PakCompression::LZ4) {
        int maxCompressed = LZ4_compressBound(static_cast<int>(srcSize));
        outBuffer.resize(static_cast<size_t>(maxCompressed));
        int compressedSize = LZ4_compress_default(
            reinterpret_cast<const char*>(src),
            reinterpret_cast<char*>(outBuffer.data()),
            static_cast<int>(srcSize),
            maxCompressed);
        if (compressedSize <= 0) {
            Log(PakLogLevel::Error, "CompressBuffer: LZ4 compression failed.");
            outBuffer.clear();
            return false;
        }
        if (static_cast<uint64_t>(compressedSize) >= srcSize) {
            outBuffer.clear();
            return true;
        }
        outBuffer.resize(static_cast<size_t>(compressedSize));
        outFlags = PAK_FLAG_LZ4_COMPRESSED;
        return true;
    }

    // PakCompression::Zstd
    size_t bound = ZSTD_compressBound(static_cast<size_t>(srcSize));
    if (ZSTD_isError(bound)) {
        Log(PakLogLevel::Error, std::string("CompressBuffer: ZSTD_compressBound failed: ") + ZSTD_getErrorName(bound));
        return false;
    }
    outBuffer.resize(bound);
    size_t compressedSize = ZSTD_compress(outBuffer.data(), outBuffer.size(),
                                           src, static_cast<size_t>(srcSize), zstdLevel);
    if (ZSTD_isError(compressedSize)) {
        Log(PakLogLevel::Error, std::string("CompressBuffer: Zstd compression failed: ") + ZSTD_getErrorName(compressedSize));
        outBuffer.clear();
        return false;
    }
    if (static_cast<uint64_t>(compressedSize) >= srcSize) {
        outBuffer.clear();
        return true;
    }
    outBuffer.resize(compressedSize);
    outFlags = PAK_FLAG_ZSTD_COMPRESSED;
    return true;
}

namespace {

// Compresses one chunk into `dst`, returning the bytes written. Falls back to
// a raw copy (return value == srcSize) when compression does not help, which
// is how an incompressible block avoids paying an expansion penalty.
size_t CompressOneChunk(PakCompression method, int zstdLevel,
                        const uint8_t* src, size_t srcSize,
                        uint8_t* dst, size_t dstCapacity)
{
    size_t produced = srcSize; // sentinel meaning "stored raw"

    if (method == PakCompression::LZ4) {
        const int result = LZ4_compress_default(
            reinterpret_cast<const char*>(src), reinterpret_cast<char*>(dst),
            static_cast<int>(srcSize), static_cast<int>(dstCapacity));
        // A non-positive result is not a hard failure here: it means the block
        // did not fit the bound because it is incompressible. Store it raw.
        produced = result > 0 ? static_cast<size_t>(result) : srcSize;
    } else {
        const size_t result = ZSTD_compress(dst, dstCapacity, src, srcSize, zstdLevel);
        produced = ZSTD_isError(result) ? srcSize : result;
    }

    if (produced >= srcSize) {
        std::memcpy(dst, src, srcSize);
        return srcSize;
    }
    return produced;
}

} // namespace

bool CompressChunked(PakCompression method, int zstdLevel, uint32_t chunkSize,
                     const uint8_t* src, uint64_t srcSize,
                     std::vector<uint8_t>& outBuffer, uint8_t& outFlags)
{
    outFlags = 0;
    outBuffer.clear();

    if (method == PakCompression::None || srcSize == 0) return true;
    if (chunkSize < MIN_COMPRESSION_CHUNK_SIZE || chunkSize > MAX_COMPRESSION_CHUNK_SIZE ||
        (chunkSize & (chunkSize - 1)) != 0) {
        Log(PakLogLevel::Error, "CompressChunked: Invalid chunk size.");
        return false;
    }
    if (srcSize > MAX_COMPRESSIBLE_ENTRY_SIZE) {
        Log(PakLogLevel::Warning,
            "CompressChunked: Input too large to compress, storing uncompressed.");
        return true;
    }

    const uint64_t chunkCount64 = (srcSize + chunkSize - 1) / chunkSize;
    if (chunkCount64 > 0xFFFFFFFFull) {
        Log(PakLogLevel::Error, "CompressChunked: Too many chunks.");
        return false;
    }
    const uint32_t chunkCount = static_cast<uint32_t>(chunkCount64);

    const size_t tableBytes = sizeof(PakChunkHeader) +
                              static_cast<size_t>(chunkCount) * sizeof(uint32_t);

    // Worst case every chunk stores raw, so the payload can never exceed the
    // table plus the original bytes.
    outBuffer.resize(tableBytes + static_cast<size_t>(srcSize));

    PakChunkHeader header{};
    header.chunkSize = chunkSize;
    header.chunkCount = chunkCount;
    std::memcpy(outBuffer.data(), &header, sizeof(header));

    size_t writeOffset = tableBytes;
    for (uint32_t i = 0; i < chunkCount; ++i) {
        const uint64_t chunkStart = static_cast<uint64_t>(i) * chunkSize;
        const size_t rawSize = static_cast<size_t>(
            (std::min)(static_cast<uint64_t>(chunkSize), srcSize - chunkStart));

        const size_t produced = CompressOneChunk(
            method, zstdLevel, src + chunkStart, rawSize,
            outBuffer.data() + writeOffset, outBuffer.size() - writeOffset);

        // Written through a fresh pointer each iteration: outBuffer is not
        // resized inside the loop, but taking the address once outside it
        // would be fragile if that ever changed.
        auto* sizes = reinterpret_cast<uint32_t*>(outBuffer.data() + sizeof(PakChunkHeader));
        sizes[i] = static_cast<uint32_t>(produced);
        writeOffset += produced;
    }

    // Same contract as CompressBuffer: if it did not actually get smaller,
    // report "no compression" and let the caller store the raw entry.
    if (static_cast<uint64_t>(writeOffset) >= srcSize) {
        outBuffer.clear();
        return true;
    }

    outBuffer.resize(writeOffset);
    outFlags = static_cast<uint8_t>(
        (method == PakCompression::LZ4 ? PAK_FLAG_LZ4_COMPRESSED : PAK_FLAG_ZSTD_COMPRESSED) |
        PAK_FLAG_CHUNKED);
    return true;
}

PakStatus ParseChunkTable(const uint8_t* payload, uint64_t payloadSize,
                          uint64_t originalSize, PakChunkHeader& outHeader,
                          const uint32_t*& outCompressedSizes,
                          uint64_t& outFirstChunkOffset)
{
    outCompressedSizes = nullptr;
    outFirstChunkOffset = 0;

    if (payloadSize < sizeof(PakChunkHeader)) return PakStatus::CorruptArchive;
    std::memcpy(&outHeader, payload, sizeof(PakChunkHeader));

    if (outHeader.chunkSize < MIN_COMPRESSION_CHUNK_SIZE ||
        outHeader.chunkSize > MAX_COMPRESSION_CHUNK_SIZE ||
        (outHeader.chunkSize & (outHeader.chunkSize - 1)) != 0) {
        return PakStatus::CorruptArchive;
    }
    if (originalSize == 0) return PakStatus::CorruptArchive;

    const uint64_t expectedChunks =
        (originalSize + outHeader.chunkSize - 1) / outHeader.chunkSize;
    if (outHeader.chunkCount != expectedChunks) return PakStatus::CorruptArchive;

    const uint64_t tableBytes = sizeof(PakChunkHeader) +
                                static_cast<uint64_t>(outHeader.chunkCount) * sizeof(uint32_t);
    if (tableBytes > payloadSize) return PakStatus::CorruptArchive;

    outCompressedSizes = reinterpret_cast<const uint32_t*>(payload + sizeof(PakChunkHeader));
    outFirstChunkOffset = tableBytes;

    // Every chunk must fit inside the payload, and none may claim more bytes
    // than its uncompressed size, since raw storage is the ceiling. This runs
    // once per range read and is what keeps a corrupt table from steering a
    // decode off the end of the mapping.
    uint64_t cursor = tableBytes;
    for (uint32_t i = 0; i < outHeader.chunkCount; ++i) {
        const uint64_t chunkStart = static_cast<uint64_t>(i) * outHeader.chunkSize;
        const uint64_t rawSize =
            (std::min)(static_cast<uint64_t>(outHeader.chunkSize), originalSize - chunkStart);
        const uint64_t stored = outCompressedSizes[i];
        if (stored == 0 || stored > rawSize) return PakStatus::CorruptArchive;
        if (stored > payloadSize - cursor) return PakStatus::CorruptArchive;
        cursor += stored;
    }
    if (cursor != payloadSize) return PakStatus::CorruptArchive;

    return PakStatus::Ok;
}

PakStatus DecompressChunkedRange(uint8_t flags, const uint8_t* payload, uint64_t payloadSize,
                                 uint64_t originalSize, uint64_t rangeOffset,
                                 uint8_t* dst, uint64_t rangeSize,
                                 std::vector<uint8_t>& scratch)
{
    if (rangeSize == 0) return PakStatus::Ok;
    if (rangeOffset > originalSize || rangeSize > originalSize - rangeOffset) {
        return PakStatus::InvalidArgument;
    }

    PakChunkHeader header{};
    const uint32_t* sizes = nullptr;
    uint64_t firstChunkOffset = 0;
    PakStatus status = ParseChunkTable(payload, payloadSize, originalSize,
                                       header, sizes, firstChunkOffset);
    if (status != PakStatus::Ok) return status;

    const uint32_t chunkSize = header.chunkSize;
    const uint32_t firstChunk = static_cast<uint32_t>(rangeOffset / chunkSize);
    const uint32_t lastChunk =
        static_cast<uint32_t>((rangeOffset + rangeSize - 1) / chunkSize);

    // Skip the chunks the range does not touch. This is the whole point of
    // chunking: decode cost tracks the range asked for, not the entry size.
    uint64_t payloadCursor = firstChunkOffset;
    for (uint32_t i = 0; i < firstChunk; ++i) payloadCursor += sizes[i];

    uint64_t written = 0;
    for (uint32_t i = firstChunk; i <= lastChunk; ++i) {
        const uint64_t chunkStart = static_cast<uint64_t>(i) * chunkSize;
        const uint64_t rawSize =
            (std::min)(static_cast<uint64_t>(chunkSize), originalSize - chunkStart);
        const uint64_t stored = sizes[i];

        // The slice of this chunk the caller actually asked for.
        const uint64_t copyStart = (std::max)(rangeOffset, chunkStart);
        const uint64_t copyEnd = (std::min)(rangeOffset + rangeSize, chunkStart + rawSize);
        const uint64_t copySize = copyEnd - copyStart;
        const uint64_t copyFromChunk = copyStart - chunkStart;

        if (stored == rawSize) {
            // Stored raw, so no decode at all -- copy the slice directly.
            std::memcpy(dst + written, payload + payloadCursor + copyFromChunk,
                        static_cast<size_t>(copySize));
        } else if (copySize == rawSize) {
            // The whole chunk is wanted: decode straight into the destination.
            status = DecompressBuffer(flags, payload + payloadCursor, stored,
                                      dst + written, rawSize);
            if (status != PakStatus::Ok) return status;
        } else {
            // A partial chunk still has to be decoded whole, then sliced.
            scratch.resize(static_cast<size_t>(rawSize));
            status = DecompressBuffer(flags, payload + payloadCursor, stored,
                                      scratch.data(), rawSize);
            if (status != PakStatus::Ok) return status;
            std::memcpy(dst + written, scratch.data() + copyFromChunk,
                        static_cast<size_t>(copySize));
        }

        written += copySize;
        payloadCursor += stored;
    }

    return written == rangeSize ? PakStatus::Ok : PakStatus::DecompressionFailed;
}

PakStatus DecompressBuffer(uint8_t flags, const uint8_t* src, uint64_t srcSize,
                            uint8_t* dst, uint64_t originalSize)
{
    if (srcSize > MAX_COMPRESSIBLE_ENTRY_SIZE || originalSize > MAX_COMPRESSIBLE_ENTRY_SIZE) {
        Log(PakLogLevel::Error, "DecompressBuffer: Entry exceeds size limit.");
        return PakStatus::CorruptArchive;
    }

    if (flags & PAK_FLAG_LZ4_COMPRESSED) {
        int result = LZ4_decompress_safe(
            reinterpret_cast<const char*>(src),
            reinterpret_cast<char*>(dst),
            static_cast<int>(srcSize),
            static_cast<int>(originalSize));
        if (result < 0 || static_cast<uint64_t>(result) != originalSize) {
            Log(PakLogLevel::Error, "DecompressBuffer: LZ4 decompression failed.");
            return PakStatus::DecompressionFailed;
        }
        return PakStatus::Ok;
    }

    if (flags & PAK_FLAG_ZSTD_COMPRESSED) {
        size_t result = ZSTD_decompress(dst, static_cast<size_t>(originalSize),
                                         src, static_cast<size_t>(srcSize));
        if (ZSTD_isError(result) || static_cast<uint64_t>(result) != originalSize) {
            Log(PakLogLevel::Error, std::string("DecompressBuffer: Zstd decompression failed: ") +
                (ZSTD_isError(result) ? ZSTD_getErrorName(result) : "size mismatch"));
            return PakStatus::DecompressionFailed;
        }
        return PakStatus::Ok;
    }

    // Not compressed: caller shouldn't route uncompressed entries through
    // this function, but handle it defensively as a plain copy.
    if (originalSize != srcSize) {
        Log(PakLogLevel::Error, "DecompressBuffer: Uncompressed entry has mismatched sizes.");
        return PakStatus::CorruptArchive;
    }
    if (originalSize > 0) {
        std::memcpy(dst, src, static_cast<size_t>(originalSize));
    }
    return PakStatus::Ok;
}

} // namespace PakInternal
