#ifndef PAK_BENCH_SUPPORT_H
#define PAK_BENCH_SUPPORT_H

// Shared helpers for the Pakker benchmark executables: timing, unit
// formatting, latency percentiles, process residency, and synthetic asset
// generation with realistic path shapes and compressibility.
//
// Percentiles matter more than means here. A streaming asset system is judged
// on hitches, so a p99 that is 20x the mean is a shipping problem even when
// the mean looks fine.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace bench {

using Clock = std::chrono::steady_clock;

inline double Ms(Clock::duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

inline double Seconds(Clock::duration d)
{
    return std::chrono::duration<double>(d).count();
}

inline double NsPerOp(Clock::duration d, uint64_t ops)
{
    if (ops == 0) return 0.0;
    return std::chrono::duration<double, std::nano>(d).count() / static_cast<double>(ops);
}

inline double GiBPerSecond(Clock::duration d, uint64_t bytes)
{
    const double seconds = Seconds(d);
    if (seconds <= 0.0) return 0.0;
    return (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) / seconds;
}

inline double MiB(uint64_t bytes)
{
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

// Current process resident/working-set bytes, or 0 when unavailable.
inline uint64_t ResidentBytes()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return static_cast<uint64_t>(pmc.WorkingSetSize);
    return 0;
#elif defined(__linux__)
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long long total = 0, resident = 0;
    int scanned = std::fscanf(f, "%llu %llu", &total, &resident);
    std::fclose(f);
    if (scanned != 2) return 0;
    return static_cast<uint64_t>(resident) * 4096ull;
#else
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// Latency distribution
// ---------------------------------------------------------------------------

struct Latencies {
    std::vector<double> samplesNs;

    void Reserve(size_t n) { samplesNs.reserve(n); }
    void Add(Clock::duration d)
    {
        samplesNs.push_back(std::chrono::duration<double, std::nano>(d).count());
    }

    double Percentile(double fraction)
    {
        if (samplesNs.empty()) return 0.0;
        std::sort(samplesNs.begin(), samplesNs.end());
        size_t index = static_cast<size_t>(fraction * static_cast<double>(samplesNs.size() - 1));
        return samplesNs[index];
    }

    double Mean() const
    {
        if (samplesNs.empty()) return 0.0;
        double total = 0.0;
        for (double value : samplesNs) total += value;
        return total / static_cast<double>(samplesNs.size());
    }
};

// Prints "label: mean / p50 / p99 / max" in microseconds.
inline void PrintLatencies(const char* label, Latencies& latencies)
{
    const double p50 = latencies.Percentile(0.50);
    const double p99 = latencies.Percentile(0.99);
    const double p100 = latencies.Percentile(1.0);
    std::cout << "  " << std::left << std::setw(26) << label << std::right
              << std::fixed << std::setprecision(2)
              << " mean " << std::setw(9) << latencies.Mean() / 1000.0
              << "  p50 " << std::setw(9) << p50 / 1000.0
              << "  p99 " << std::setw(9) << p99 / 1000.0
              << "  max " << std::setw(9) << p100 / 1000.0
              << "  us\n";
}

inline void Header(const char* title)
{
    std::cout << "\n=== " << title << " ===\n";
}

inline void Row(const char* label, double value, const char* unit)
{
    std::cout << "  " << std::left << std::setw(30) << label << std::right
              << std::fixed << std::setprecision(2) << std::setw(12) << value
              << ' ' << unit << "\n";
}

// ---------------------------------------------------------------------------
// Synthetic assets
// ---------------------------------------------------------------------------

// Builds a path with the shape and length distribution of a real shipping
// asset tree, so name-blob and hashing costs are measured against realistic
// string lengths rather than "file_17.bin".
inline std::string MakeAssetPath(size_t index)
{
    static const char* const kCategories[] = {
        "characters", "weapons", "environments", "vehicles", "effects",
        "ui", "audio", "animations", "materials", "shaders",
    };
    static const char* const kKinds[] = {
        "textures/albedo", "textures/normal", "textures/roughness",
        "meshes/lod0", "meshes/lod1", "meshes/lod2",
        "anim/clips", "sound/banks", "mtl", "sdr",
    };

    std::string path = "game/";
    path += kCategories[index % 10];
    path += "/set_";
    path += std::to_string((index / 100) % 512);
    path += "/";
    path += kKinds[(index / 10) % 10];
    path += "/asset_";
    path += std::to_string(index);
    path += ".bin";
    return path;
}

// Fills `out` with data that compresses at roughly the ratio real shipping
// assets do (~2:1 for LZ4). Getting this wrong invalidates every compression
// number: incompressible random bytes understate decode cost and overstate
// archive size, while all-zero or purely-repetitive buffers do the reverse
// and can make decode look faster than memcpy.
//
// Real asset payloads are a mix of already-compressed blocks (BC7 texture
// data, encoded audio) that give up almost nothing, and structured blocks
// (vertex streams, animation curves, text) that compress well. This emits
// that mixture: alternating runs of incompressible noise and runs drawn from
// a small repeating dictionary.
inline void FillCompressible(std::vector<uint8_t>& out, uint64_t seed)
{
    constexpr size_t kTokenSize = 32;
    constexpr size_t kTokenCount = 192;
    constexpr size_t kRunBytes = 256;

    static thread_local std::vector<uint8_t> dictionary;
    if (dictionary.empty()) {
        std::mt19937_64 dictRng(0xA55A5AA5ull);
        dictionary.resize(kTokenSize * kTokenCount);
        for (auto& byte : dictionary)
            byte = static_cast<uint8_t>(dictRng() & 0xff);
    }

    std::mt19937_64 rng(seed);
    size_t written = 0;
    bool incompressibleRun = true;
    while (written < out.size()) {
        const size_t runSize = (std::min)(kRunBytes, out.size() - written);
        if (incompressibleRun) {
            for (size_t i = 0; i < runSize; ++i)
                out[written + i] = static_cast<uint8_t>(rng() & 0xff);
        } else {
            size_t filled = 0;
            while (filled < runSize) {
                const size_t token = static_cast<size_t>(rng() % kTokenCount);
                const size_t chunk = (std::min)(kTokenSize, runSize - filled);
                std::memcpy(out.data() + written + filled,
                            dictionary.data() + token * kTokenSize, chunk);
                filled += chunk;
            }
        }
        written += runSize;
        incompressibleRun = !incompressibleRun;
    }
}

inline std::vector<uint8_t> MakeAssetPayload(size_t bytes, uint64_t seed)
{
    std::vector<uint8_t> data(bytes);
    FillCompressible(data, seed);
    return data;
}

} // namespace bench

#endif // PAK_BENCH_SUPPORT_H
