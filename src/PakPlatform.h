#ifndef PAK_PLATFORM_H
#define PAK_PLATFORM_H

#include <cstdint>
#include <cstddef>

namespace PakPlatform {

struct MappedFile {
    void* data = nullptr;
    uint64_t size = 0;
#ifndef PAK_NO_MMAP
#ifdef _WIN32
    void* fileHandle = nullptr;
    void* mappingHandle = nullptr;
#else
    int fd = -1;
#endif
#endif
};

// Map entire file read-only into memory. Returns {nullptr, 0} on failure.
MappedFile MapFileReadOnly(const char* path);

// Unmap a previously mapped file and reset the struct.
void UnmapFile(MappedFile& mf);

// Best-effort hint that a mapped byte range will be read soon.
// Returns false only when the range is invalid or the platform call fails.
bool PrefetchMappedRange(const MappedFile& mf, uint64_t offset, uint64_t size);

} // namespace PakPlatform

#endif // PAK_PLATFORM_H
