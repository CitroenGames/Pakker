// Build-path benchmark: how fast an archive can actually be packed.
//
// Pack throughput is what decides whether a full content build is a coffee
// break or an overnight job. The numbers here are single-threaded because
// the builder is single-threaded -- the "cores idle" line at the end is the
// headroom a parallel pipeline would recover.
//
// Zstd level 19 is measured deliberately even though it is slow: it is the
// library's default (PakOptions::zstdLevel), so it is what a shipping build
// pays unless the caller knows to override it.

#include "Pak.h"
#include "BenchSupport.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace bench;

namespace {

struct BuildResult {
    double seconds = 0.0;
    uint64_t archiveBytes = 0;
};

BuildResult MeasureBuild(const fs::path& path, PakCompression compression, int zstdLevel,
                         const std::map<std::string, std::vector<uint8_t>>& files)
{
    PakOptions options;
    options.compression = compression;
    options.zstdLevel = zstdLevel;
    options.alignment = 4096;

    Pakker pakker;
    auto start = Clock::now();
    const bool ok = pakker.CreatePak(path.string(), files, options);
    auto elapsed = Clock::now() - start;

    BuildResult result;
    if (!ok) return result;
    result.seconds = Seconds(elapsed);

    std::error_code ec;
    result.archiveBytes = fs::file_size(path, ec);

    std::error_code removeEc;
    fs::remove(path, removeEc);
    return result;
}

void ReportBuild(const char* label, const BuildResult& result, uint64_t rawBytes)
{
    if (result.seconds <= 0.0) {
        std::cout << "  " << std::left << std::setw(14) << label << "  FAILED\n";
        return;
    }

    const double mibPerSecond = MiB(rawBytes) / result.seconds;
    const double ratio = result.archiveBytes
        ? static_cast<double>(rawBytes) / static_cast<double>(result.archiveBytes)
        : 0.0;
    // Hours to pack a 150 GiB install at this rate, single-threaded.
    const double hoursFor150GiB = (150.0 * 1024.0) / mibPerSecond / 3600.0;

    std::cout << "  " << std::left << std::setw(14) << label << std::right
              << std::fixed << std::setprecision(2)
              << std::setw(10) << result.seconds << " s"
              << std::setw(10) << mibPerSecond << " MiB/s"
              << "   ratio " << std::setw(6) << ratio << ":1"
              << "   150 GiB -> " << std::setw(8) << hoursFor150GiB << " h\n";
}

} // namespace

int main(int argc, char** argv)
{
    size_t corpusMiB = 64;
    if (argc > 1) corpusMiB = static_cast<size_t>(std::strtoull(argv[1], nullptr, 10));

    fs::path root = fs::temp_directory_path() / "pakker_build_benchmark";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    const size_t hardwareThreads = (std::max)(1u, std::thread::hardware_concurrency());

    // -----------------------------------------------------------------
    // Corpus: a mix of sizes, as a real asset tree has.
    // -----------------------------------------------------------------
    std::map<std::string, std::vector<uint8_t>> files;
    uint64_t rawBytes = 0;
    {
        const uint64_t targetBytes = static_cast<uint64_t>(corpusMiB) * 1024ull * 1024ull;
        size_t index = 0;
        while (rawBytes < targetBytes) {
            // Cycle 8 KiB / 128 KiB / 2 MiB entries.
            const size_t sizes[] = { 8ull * 1024, 128ull * 1024, 2ull * 1024 * 1024 };
            const size_t bytes = sizes[index % 3];

            // Every tenth entry reuses an earlier payload verbatim. Real asset
            // trees carry a meaningful share of byte-identical files -- shared
            // LODs, placeholder art, reused audio stings, identical stub
            // materials -- and modelling that is what makes the duplicate
            // measurement below mean anything.
            const uint64_t payloadSeed = (index % 10 == 9 && index >= 30)
                ? static_cast<uint64_t>(index - 30) + 1
                : static_cast<uint64_t>(index) + 1;
            const size_t payloadBytes = (index % 10 == 9 && index >= 30)
                ? sizes[(index - 30) % 3]
                : bytes;

            files.emplace(MakeAssetPath(index), MakeAssetPayload(payloadBytes, payloadSeed));
            rawBytes += payloadBytes;
            ++index;
        }
    }

    std::cout << "Pakker build benchmark\n"
              << "  corpus: " << MiB(rawBytes) << " MiB across " << files.size() << " entries\n"
              << "  hardware threads: " << hardwareThreads << "\n";

    // -----------------------------------------------------------------
    // Duplicate content: shipping asset trees carry a lot of byte-identical
    // payloads (shared LODs, placeholder textures, reused audio). Anything
    // reported here is archive size the builder currently writes twice.
    // -----------------------------------------------------------------
    {
        std::unordered_map<std::string, uint64_t> byContent;
        uint64_t duplicateBytes = 0;
        for (const auto& [name, data] : files) {
            std::string key(reinterpret_cast<const char*>(data.data()), data.size());
            auto [it, inserted] = byContent.emplace(std::move(key), data.size());
            if (!inserted) duplicateBytes += data.size();
        }
        std::cout << "  byte-identical payloads: " << MiB(duplicateBytes) << " MiB ("
                  << std::fixed << std::setprecision(1)
                  << (100.0 * static_cast<double>(duplicateBytes) / static_cast<double>(rawBytes))
                  << "% of corpus) -- written in full by every build below\n";
    }

    Header("CreatePak throughput (single-threaded)");
    ReportBuild("store", MeasureBuild(root / "b.pak", PakCompression::None, 0, files), rawBytes);
    ReportBuild("lz4", MeasureBuild(root / "b.pak", PakCompression::LZ4, 0, files), rawBytes);
    for (int level : { 1, 3, 9, 19 }) {
        const std::string label = "zstd-" + std::to_string(level);
        ReportBuild(label.c_str(), MeasureBuild(root / "b.pak", PakCompression::Zstd, level, files),
                    rawBytes);
    }

    // -----------------------------------------------------------------
    // Folder-based build: the path a content pipeline actually uses. It
    // streams entries from disk one at a time, so it also pays per-file
    // open/read cost on top of compression.
    // -----------------------------------------------------------------
    Header("CreatePakFromFolder throughput (includes per-file disk reads)");
    {
        const fs::path contentRoot = root / "content";
        fs::create_directories(contentRoot, ec);
        for (const auto& [name, data] : files) {
            const fs::path filePath = contentRoot / name;
            fs::create_directories(filePath.parent_path(), ec);
            std::ofstream out(filePath, std::ios::binary);
            out.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
        }

        for (auto [label, compression, level] : {
                 std::tuple<const char*, PakCompression, int>{ "store", PakCompression::None, 0 },
                 std::tuple<const char*, PakCompression, int>{ "lz4", PakCompression::LZ4, 0 },
                 std::tuple<const char*, PakCompression, int>{ "zstd-3", PakCompression::Zstd, 3 },
             }) {
            PakOptions options;
            options.compression = compression;
            options.zstdLevel = level;
            options.alignment = 4096;

            Pakker pakker;
            const fs::path outPath = root / "folder.pak";
            auto start = Clock::now();
            const bool ok = pakker.CreatePakFromFolder(outPath.string(), contentRoot.string(), options);
            auto elapsed = Clock::now() - start;

            BuildResult result;
            if (ok) {
                result.seconds = Seconds(elapsed);
                result.archiveBytes = fs::file_size(outPath, ec);
            }
            fs::remove(outPath, ec);
            ReportBuild(label, result, rawBytes);
        }
    }

    std::cout << "\n  " << (hardwareThreads - 1) << " of " << hardwareThreads
              << " cores are idle throughout every build above.\n";

    fs::remove_all(root, ec);
    return 0;
}
