#include "Pak.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

static double Ms(std::chrono::steady_clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

static double NsPerOperation(std::chrono::steady_clock::duration duration, size_t operations)
{
    return std::chrono::duration<double, std::nano>(duration).count() /
           static_cast<double>(operations);
}

static double GiBPerSecond(std::chrono::steady_clock::duration duration, uint64_t bytes)
{
    const double seconds = std::chrono::duration<double>(duration).count();
    return (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) / seconds;
}

int main()
{
    fs::path root = fs::temp_directory_path() / "pakker_runtime_benchmark";
    fs::remove_all(root);
    fs::create_directories(root);
    fs::path pakPath = root / "bench.pak";

    constexpr size_t fileCount = 2048;
    constexpr size_t fileSize = 4096;

    std::map<std::string, std::vector<uint8_t>> files;
    for (size_t i = 0; i < fileCount; ++i) {
        std::string name = "assets/file_" + std::to_string(i) + ".bin";
        std::vector<uint8_t> data(fileSize);
        for (size_t j = 0; j < data.size(); ++j) {
            data[j] = static_cast<uint8_t>((i + j) & 0xff);
        }
        files.emplace(std::move(name), std::move(data));
    }

    PakOptions options;
    options.compression = PakCompression::None;
    options.alignment = 4096;

    Pakker pakker;
    if (!pakker.CreatePak(pakPath.string(), files, options)) {
        std::cerr << "failed to create benchmark pak\n";
        return 1;
    }

    PakReader reader;
    if (!reader.Open(pakPath.string())) {
        std::cerr << "failed to open benchmark pak\n";
        return 1;
    }

    std::vector<std::string> names;
    std::vector<std::string_view> nameViews;
    names.reserve(fileCount);
    nameViews.reserve(fileCount);
    for (size_t i = 0; i < fileCount; ++i) {
        names.push_back("assets/file_" + std::to_string(i) + ".bin");
        nameViews.push_back(names.back());
    }

    std::vector<PakFileHandle> handles(fileCount);
    constexpr size_t resolveRounds = 512;
    constexpr size_t viewRounds = 256;
    constexpr size_t readRounds = 64;

    // Warm the mapped pages before measuring API overhead. Without this pass,
    // the view result mostly measures first-touch page faults and varies with
    // the host's filesystem cache state.
    size_t resolved = reader.Resolve(nameViews, handles);
    uint64_t checksum = 0;
    for (PakFileHandle handle : handles) {
        PakView view;
        if (reader.View(handle, view) == PakStatus::Ok && view.size > 0) {
            checksum += view.data[0];
        }
    }

    auto t0 = std::chrono::steady_clock::now();
    for (size_t round = 0; round < resolveRounds; ++round) {
        resolved = reader.Resolve(nameViews, handles);
    }
    auto t1 = std::chrono::steady_clock::now();

    for (size_t round = 0; round < viewRounds; ++round) {
        for (PakFileHandle handle : handles) {
            PakView view;
            if (reader.View(handle, view) == PakStatus::Ok && view.size > 0) {
                checksum += view.data[0];
            }
        }
    }
    auto t2 = std::chrono::steady_clock::now();

    std::vector<uint8_t> buffer(fileSize);
    for (size_t round = 0; round < readRounds; ++round) {
        for (PakFileHandle handle : handles) {
            if (reader.Read(handle, buffer) == PakStatus::Ok) {
                checksum += buffer.back();
            }
        }
    }
    auto t3 = std::chrono::steady_clock::now();

    const size_t resolveOperations = fileCount * resolveRounds;
    const size_t viewOperations = fileCount * viewRounds;
    const size_t readOperations = fileCount * readRounds;
    const uint64_t readBytes = static_cast<uint64_t>(readOperations) * fileSize;

    std::cout << "resolved=" << resolved << "/" << fileCount << "\n";
    std::cout << "resolve_ms=" << Ms(t1 - t0)
              << " resolve_ns_per_file=" << NsPerOperation(t1 - t0, resolveOperations) << "\n";
    std::cout << "view_ms=" << Ms(t2 - t1)
              << " view_ns_per_file=" << NsPerOperation(t2 - t1, viewOperations) << "\n";
    std::cout << "read_copy_ms=" << Ms(t3 - t2)
              << " read_copy_gib_s=" << GiBPerSecond(t3 - t2, readBytes) << "\n";

    // Exercise cache churn with a working set larger than the memory budget.
    // This catches regressions in eviction bookkeeping that a single hot
    // cached asset cannot expose.
    fs::path compressedPakPath = root / "bench_compressed.pak";
    PakOptions compressedOptions;
    compressedOptions.compression = PakCompression::LZ4;
    if (!pakker.CreatePak(compressedPakPath.string(), files, compressedOptions)) {
        std::cerr << "failed to create compressed benchmark pak\n";
        return 1;
    }

    PakReader cachedReader;
    PakCacheOptions cacheOptions;
    cacheOptions.persistentCacheEnabled = false;
    cacheOptions.memoryBudgetBytes = 1024 * fileSize;
    cacheOptions.maxSingleEntryBytes = fileSize;
    cachedReader.SetCacheOptions(cacheOptions);
    if (!cachedReader.Open(compressedPakPath.string())) {
        std::cerr << "failed to open compressed benchmark pak\n";
        return 1;
    }

    std::vector<PakFileHandle> cachedHandles(fileCount);
    if (cachedReader.Resolve(nameViews, cachedHandles) != fileCount) {
        std::cerr << "failed to resolve compressed benchmark handles\n";
        return 1;
    }

    constexpr size_t cacheRounds = 4;
    auto t4 = std::chrono::steady_clock::now();
    for (size_t round = 0; round < cacheRounds; ++round) {
        for (PakFileHandle handle : cachedHandles) {
            if (cachedReader.Read(handle, buffer) != PakStatus::Ok) {
                std::cerr << "failed cached benchmark read\n";
                return 1;
            }
            checksum += buffer[round % buffer.size()];
        }
    }
    auto t5 = std::chrono::steady_clock::now();
    const size_t cacheOperations = fileCount * cacheRounds;
    PakCacheStats cacheStats = cachedReader.GetCacheStats();
    std::cout << "cache_churn_ms=" << Ms(t5 - t4)
              << " cache_churn_ns_per_file=" << NsPerOperation(t5 - t4, cacheOperations)
              << " cache_evictions=" << cacheStats.memoryEvictions << "\n";

    constexpr size_t hotCacheReads = 262144;
    const PakFileHandle hotHandle = cachedHandles.back();
    auto t6 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < hotCacheReads; ++i) {
        if (cachedReader.Read(hotHandle, buffer) != PakStatus::Ok) {
            std::cerr << "failed hot-cache benchmark read\n";
            return 1;
        }
        checksum += buffer[i % buffer.size()];
    }
    auto t7 = std::chrono::steady_clock::now();
    const uint64_t hotCacheBytes = static_cast<uint64_t>(hotCacheReads) * fileSize;
    std::cout << "cache_hot_ms=" << Ms(t7 - t6)
              << " cache_hot_ns_per_file=" << NsPerOperation(t7 - t6, hotCacheReads)
              << " cache_hot_gib_s=" << GiBPerSecond(t7 - t6, hotCacheBytes) << "\n";
    std::cout << "checksum=" << checksum << "\n";
    return resolved == fileCount ? 0 : 1;
}
