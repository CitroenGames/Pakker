#include "PakCompression.h"
#include "PakInternal.h"
#include <cstring>

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
