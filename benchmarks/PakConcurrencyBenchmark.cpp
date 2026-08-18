// Concurrency benchmark: how the read path scales as an engine fans asset
// streaming across worker threads.
//
// A shipping streamer issues lookups and reads from every worker thread at
// once, so what matters is not single-thread latency but whether throughput
// actually multiplies with core count. Anything that touches a shared cache
// line on every operation -- a mutex, or a shared_ptr refcount -- caps that
// scaling regardless of how fast the single-threaded path is.
//
// Each scenario reports aggregate throughput and parallel efficiency against
// the same workload on one thread. Efficiency well under 1.0 at high thread
// counts is the signal worth chasing.

#include "Pak.h"
#include "BenchSupport.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace bench;

namespace {

constexpr size_t kEntryCount = 4096;
constexpr size_t kEntryBytes = 16 * 1024;

std::string EntryName(size_t index)
{
    return "stream/asset_" + std::to_string(index) + ".bin";
}

// Runs `work` on `threadCount` threads, each performing opsPerThread
// operations, and returns the wall time for the whole fan-out.
Clock::duration RunParallel(size_t threadCount, size_t opsPerThread,
                            const std::function<void(size_t, size_t)>& work)
{
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    std::atomic<size_t> ready{0};
    std::atomic<bool> go{false};

    for (size_t t = 0; t < threadCount; ++t) {
        threads.emplace_back([&, t]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            work(t, opsPerThread);
        });
    }

    while (ready.load(std::memory_order_acquire) < threadCount) {
        std::this_thread::yield();
    }

    auto start = Clock::now();
    go.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    return Clock::now() - start;
}

struct Scenario {
    const char* name;
    std::function<void(size_t, size_t)> work;
    size_t opsPerThread;
};

void MeasureScaling(const Scenario& scenario, const std::vector<size_t>& threadCounts)
{
    std::cout << "  " << scenario.name << "\n";

    double baselineOpsPerSecond = 0.0;
    for (size_t threadCount : threadCounts) {
        auto elapsed = RunParallel(threadCount, scenario.opsPerThread, scenario.work);
        const uint64_t totalOps = static_cast<uint64_t>(threadCount) * scenario.opsPerThread;
        const double opsPerSecond = static_cast<double>(totalOps) / Seconds(elapsed);
        if (threadCount == threadCounts.front()) baselineOpsPerSecond = opsPerSecond;

        const double efficiency = baselineOpsPerSecond > 0.0
            ? opsPerSecond / (baselineOpsPerSecond * static_cast<double>(threadCount))
            : 0.0;

        std::cout << "    " << std::right << std::setw(3) << threadCount << " thr"
                  << std::fixed << std::setprecision(2)
                  << std::setw(12) << (opsPerSecond / 1e6) << " Mops/s"
                  << std::setw(10) << NsPerOp(elapsed, totalOps) << " ns/op"
                  << "   efficiency " << std::setw(6) << efficiency << "\n";
    }
    std::cout << "\n";
}

} // namespace

int main()
{
    fs::path root = fs::temp_directory_path() / "pakker_concurrency_benchmark";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    const fs::path storePath = root / "store.pak";
    const fs::path lz4Path = root / "lz4.pak";

    {
        std::map<std::string, std::vector<uint8_t>> files;
        for (size_t i = 0; i < kEntryCount; ++i) {
            files.emplace(EntryName(i), MakeAssetPayload(kEntryBytes, i + 1));
        }

        PakOptions options;
        options.alignment = 4096;
        Pakker pakker;

        options.compression = PakCompression::None;
        if (!pakker.CreatePak(storePath.string(), files, options)) return 1;

        options.compression = PakCompression::LZ4;
        if (!pakker.CreatePak(lz4Path.string(), files, options)) return 1;
    }

    const size_t hardwareThreads = (std::max)(1u, std::thread::hardware_concurrency());
    std::vector<size_t> threadCounts = { 1, 2, 4 };
    if (hardwareThreads >= 8) threadCounts.push_back(8);
    if (hardwareThreads > 8) threadCounts.push_back(hardwareThreads);

    std::cout << "Pakker concurrency benchmark\n"
              << "  entries: " << kEntryCount << " x " << (kEntryBytes / 1024) << " KiB\n"
              << "  hardware threads: " << hardwareThreads << "\n\n";

    // -----------------------------------------------------------------
    // Name resolution
    // -----------------------------------------------------------------
    Header("Lookup scaling");
    {
        PakReader reader;
        if (!reader.Open(storePath.string())) return 1;

        std::vector<std::string> names;
        names.reserve(kEntryCount);
        for (size_t i = 0; i < kEntryCount; ++i) names.push_back(EntryName(i));

        std::atomic<uint64_t> sink{0};
        MeasureScaling({ "Find() by path", [&](size_t thread, size_t ops) {
            uint64_t local = 0;
            for (size_t i = 0; i < ops; ++i) {
                const size_t index = (i * 2654435761u + thread * 7919u) % kEntryCount;
                if (reader.Find(names[index])) ++local;
            }
            sink.fetch_add(local, std::memory_order_relaxed);
        }, 200000 }, threadCounts);
    }

    // -----------------------------------------------------------------
    // Zero-copy views: no decode, no allocation -- should scale cleanly
    // unless shared bookkeeping gets in the way.
    // -----------------------------------------------------------------
    Header("Read scaling");
    {
        PakReader reader;
        if (!reader.Open(storePath.string())) return 1;

        std::vector<PakFileHandle> handles;
        handles.reserve(kEntryCount);
        for (size_t i = 0; i < kEntryCount; ++i) handles.push_back(reader.Find(EntryName(i)));

        std::atomic<uint64_t> sink{0};

        MeasureScaling({ "View() zero-copy", [&](size_t thread, size_t ops) {
            uint64_t local = 0;
            for (size_t i = 0; i < ops; ++i) {
                const size_t index = (i * 2654435761u + thread * 7919u) % kEntryCount;
                PakView view;
                if (reader.View(handles[index], view) == PakStatus::Ok) local += view.size;
            }
            sink.fetch_add(local, std::memory_order_relaxed);
        }, 200000 }, threadCounts);

        MeasureScaling({ "Read() uncompressed (memcpy)", [&](size_t thread, size_t ops) {
            std::vector<uint8_t> destination(kEntryBytes);
            uint64_t local = 0;
            for (size_t i = 0; i < ops; ++i) {
                const size_t index = (i * 2654435761u + thread * 7919u) % kEntryCount;
                uint64_t written = 0;
                if (reader.Read(handles[index], destination, &written) == PakStatus::Ok)
                    local += written;
            }
            sink.fetch_add(local, std::memory_order_relaxed);
        }, 20000 }, threadCounts);
    }

    // -----------------------------------------------------------------
    // Compressed reads with the decoded cache off: pure decompression,
    // which is embarrassingly parallel and should scale near-linearly.
    // -----------------------------------------------------------------
    Header("Compressed read scaling, decoded cache OFF (decode-bound)");
    {
        PakReader reader;
        if (!reader.Open(lz4Path.string())) return 1;

        PakCacheOptions cacheOptions;
        cacheOptions.enabled = false;
        reader.SetCacheOptions(cacheOptions);

        std::vector<PakFileHandle> handles;
        handles.reserve(kEntryCount);
        for (size_t i = 0; i < kEntryCount; ++i) handles.push_back(reader.Find(EntryName(i)));

        std::atomic<uint64_t> sink{0};
        MeasureScaling({ "Read() lz4, no cache", [&](size_t thread, size_t ops) {
            std::vector<uint8_t> destination(kEntryBytes);
            uint64_t local = 0;
            for (size_t i = 0; i < ops; ++i) {
                const size_t index = (i * 2654435761u + thread * 7919u) % kEntryCount;
                uint64_t written = 0;
                if (reader.Read(handles[index], destination, &written) == PakStatus::Ok)
                    local += written;
            }
            sink.fetch_add(local, std::memory_order_relaxed);
        }, 20000 }, threadCounts);
    }

    // -----------------------------------------------------------------
    // Same reads with the decoded cache on and fully warm. Every operation
    // is a cache hit, so this isolates the cache's own synchronization --
    // if hits do not scale, the cache is a throughput ceiling rather than
    // a throughput win once enough threads are streaming.
    // -----------------------------------------------------------------
    Header("Compressed read scaling, decoded cache ON and warm (hit-bound)");
    {
        PakReader reader;
        if (!reader.Open(lz4Path.string())) return 1;

        PakCacheOptions cacheOptions;
        cacheOptions.enabled = true;
        cacheOptions.memoryCacheEnabled = true;
        cacheOptions.persistentCacheEnabled = false;
        cacheOptions.memoryBudgetBytes = static_cast<uint64_t>(kEntryCount) * kEntryBytes * 2;
        cacheOptions.maxSingleEntryBytes = kEntryBytes * 4;
        reader.SetCacheOptions(cacheOptions);

        std::vector<PakFileHandle> handles;
        handles.reserve(kEntryCount);
        for (size_t i = 0; i < kEntryCount; ++i) handles.push_back(reader.Find(EntryName(i)));

        // Warm every entry into the memory cache.
        {
            std::vector<uint8_t> destination(kEntryBytes);
            for (PakFileHandle handle : handles) reader.Read(handle, destination);
        }
        const PakCacheStats warmStats = reader.GetCacheStats();
        std::cout << "  warmed " << warmStats.memoryStores << " entries into the memory cache\n";

        std::atomic<uint64_t> sink{0};
        MeasureScaling({ "Read() lz4, cache hit", [&](size_t thread, size_t ops) {
            std::vector<uint8_t> destination(kEntryBytes);
            uint64_t local = 0;
            for (size_t i = 0; i < ops; ++i) {
                const size_t index = (i * 2654435761u + thread * 7919u) % kEntryCount;
                uint64_t written = 0;
                if (reader.Read(handles[index], destination, &written) == PakStatus::Ok)
                    local += written;
            }
            sink.fetch_add(local, std::memory_order_relaxed);
        }, 20000 }, threadCounts);

        const PakCacheStats stats = reader.GetCacheStats();
        std::cout << "  memory hits " << stats.memoryHits
                  << ", misses " << stats.memoryMisses << "\n";
    }

    fs::remove_all(root, ec);
    return 0;
}
