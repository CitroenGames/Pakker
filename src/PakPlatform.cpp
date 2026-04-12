#include "PakPlatform.h"

#ifndef PAK_NO_MMAP

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace PakPlatform {

#ifdef _WIN32

MappedFile MapFileReadOnly(const char* path)
{
    MappedFile mf{};

    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
                               nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return mf;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart == 0) {
        CloseHandle(hFile);
        return mf;
    }

    HANDLE hMapping = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMapping) {
        CloseHandle(hFile);
        return mf;
    }

    void* mapped = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
    if (!mapped) {
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return mf;
    }

    mf.data = mapped;
    mf.size = static_cast<uint64_t>(fileSize.QuadPart);
    mf.fileHandle = hFile;
    mf.mappingHandle = hMapping;
    return mf;
}

void UnmapFile(MappedFile& mf)
{
    if (mf.data) {
        UnmapViewOfFile(mf.data);
        mf.data = nullptr;
    }
    if (mf.mappingHandle) {
        CloseHandle(mf.mappingHandle);
        mf.mappingHandle = nullptr;
    }
    if (mf.fileHandle) {
        CloseHandle(mf.fileHandle);
        mf.fileHandle = nullptr;
    }
    mf.size = 0;
}

#else // POSIX (Linux, macOS)

MappedFile MapFileReadOnly(const char* path)
{
    MappedFile mf{};

    int fd = open(path, O_RDONLY);
    if (fd < 0) return mf;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
        close(fd);
        return mf;
    }

    void* mapped = mmap(nullptr, static_cast<size_t>(st.st_size),
                         PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        close(fd);
        return mf;
    }

    mf.data = mapped;
    mf.size = static_cast<uint64_t>(st.st_size);
    mf.fd = fd;
    return mf;
}

void UnmapFile(MappedFile& mf)
{
    if (mf.data) {
        munmap(mf.data, static_cast<size_t>(mf.size));
        mf.data = nullptr;
    }
    if (mf.fd >= 0) {
        close(mf.fd);
        mf.fd = -1;
    }
    mf.size = 0;
}

#endif // _WIN32

} // namespace PakPlatform

#else // PAK_NO_MMAP

namespace PakPlatform {

MappedFile MapFileReadOnly(const char*) { return {}; }
void UnmapFile(MappedFile& mf) { mf = {}; }

} // namespace PakPlatform

#endif // PAK_NO_MMAP
