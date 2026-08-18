// Scale benchmark: measures the metadata-side costs that dominate a
// large-game archive (hundreds of thousands of entries, dozens of mounted
// layers) rather than raw decode bandwidth, which PakRuntimeBenchmark
// already covers.
//
// Payloads are deliberately tiny -- the point is to isolate per-entry
// file-table cost (open time, resident bytes, lookup throughput, mount index
// build) from I/O bandwidth. Those per-entry costs are what stop scaling
// first on a large shipping asset set.

#include "Pak.h"
#include "BenchSupport.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace bench;

int main(int argc, char** argv)
{
    size_t fileCount = 200000;
    size_t layerCount = 24;
    if (argc > 1) fileCount = static_cast<size_t>(std::strtoull(argv[1], nullptr, 10));
    if (argc > 2) layerCount = static_cast<size_t>(std::strtoull(argv[2], nullptr, 10));

    fs::path root = fs::temp_directory_path() / "pakker_scale_benchmark";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    fs::path pakPath = root / "scale.pak";

    std::cout << "Pakker scale benchmark\n"
              << "  entries: " << fileCount << "\n"
              << "  layers:  " << layerCount << "\n\n";

    // ---------------------------------------------------------------
    // Build
    // ---------------------------------------------------------------
    std::vector<std::string> names;
    names.reserve(fileCount);
    {
        std::map<std::string, std::vector<uint8_t>> files;
        uint64_t nameBytes = 0;
        for (size_t i = 0; i < fileCount; ++i) {
            std::string name = MakeAssetPath(i);
            nameBytes += name.size();
            std::vector<uint8_t> data(64);
            for (size_t j = 0; j < data.size(); ++j)
                data[j] = static_cast<uint8_t>((i + j) & 0xff);
            names.push_back(name);
            files.emplace(std::move(name), std::move(data));
        }
        std::cout << "  mean path length: "
                  << (nameBytes / (fileCount ? fileCount : 1)) << " bytes\n\n";

        PakOptions options;
        options.compression = PakCompression::None;
        options.alignment = 16;

        Pakker pakker;
        auto start = Clock::now();
        if (!pakker.CreatePak(pakPath.string(), files, options)) {
            std::cerr << "failed to create scale pak\n";
            return 1;
        }
        auto elapsed = Clock::now() - start;
        std::cout << "CreatePak       : " << Ms(elapsed) << " ms\n";
    }

    const uint64_t archiveBytes = fs::file_size(pakPath, ec);
    std::cout << "archive size    : " << MiB(archiveBytes) << " MiB\n\n";

    // ---------------------------------------------------------------
    // Open: wall time and resident bytes held by the in-memory file table
    // ---------------------------------------------------------------
    {
        const uint64_t before = ResidentBytes();
        PakReader reader;
        auto start = Clock::now();
        if (!reader.Open(pakPath.string())) {
            std::cerr << "failed to open scale pak\n";
            return 1;
        }
        auto elapsed = Clock::now() - start;
        const uint64_t after = ResidentBytes();

        std::cout << "Open            : " << Ms(elapsed) << " ms\n";
        if (before && after >= before) {
            std::cout << "  table resident: " << MiB(after - before) << " MiB ("
                      << ((after - before) / (fileCount ? fileCount : 1))
                      << " bytes/entry)\n";
        }

        // -----------------------------------------------------------
        // Lookup throughput, random order to defeat cache locality
        // -----------------------------------------------------------
        std::vector<uint32_t> order(fileCount);
        for (size_t i = 0; i < fileCount; ++i) order[i] = static_cast<uint32_t>(i);
        std::mt19937 rng(12345);
        std::shuffle(order.begin(), order.end(), rng);

        size_t hits = 0;
        start = Clock::now();
        for (uint32_t index : order) {
            if (reader.Find(names[index])) ++hits;
        }
        elapsed = Clock::now() - start;
        std::cout << "Find (random)   : " << NsPerOp(elapsed, fileCount)
                  << " ns/lookup (" << hits << " hits)\n";

        // Misses matter too: a layered VFS probes layers that do not hold
        // the asset far more often than the one that does.
        std::vector<std::string> absentNames;
        absentNames.reserve(fileCount);
        for (uint32_t index : order) absentNames.push_back(names[index] + ".absent");

        size_t misses = 0;
        start = Clock::now();
        for (const auto& absent : absentNames) {
            if (!reader.Find(absent)) ++misses;
        }
        elapsed = Clock::now() - start;
        std::cout << "Find (miss)     : " << NsPerOp(elapsed, fileCount)
                  << " ns/lookup (" << misses << " misses)\n";

        // The shipping pattern: hash paths once at build time, carry the
        // 64-bit ids, and never touch a path string at runtime.
        std::vector<uint64_t> pathHashes;
        pathHashes.reserve(fileCount);
        for (uint32_t index : order) pathHashes.push_back(PakPathHash(names[index]));

        size_t hashHits = 0;
        start = Clock::now();
        for (uint64_t pathHash : pathHashes) {
            if (reader.FindByHash(pathHash)) ++hashHits;
        }
        elapsed = Clock::now() - start;
        std::cout << "FindByHash      : " << NsPerOp(elapsed, fileCount)
                  << " ns/lookup (" << hashHits << " hits)\n";

        // -----------------------------------------------------------
        // Full-archive integrity sweep ("verify game files")
        // -----------------------------------------------------------
        start = Clock::now();
        size_t verified = 0;
        for (uint32_t i = 0; i < static_cast<uint32_t>(fileCount); ++i) {
            if (reader.VerifyEntry(PakFileHandle{i}) == PakStatus::Ok) ++verified;
        }
        elapsed = Clock::now() - start;
        std::cout << "VerifyEntry all : " << Ms(elapsed) << " ms ("
                  << verified << " ok)\n";

        // -----------------------------------------------------------
        // Enumeration
        // -----------------------------------------------------------
        start = Clock::now();
        std::vector<std::string> listed = reader.ListFiles();
        elapsed = Clock::now() - start;
        std::cout << "ListFiles       : " << Ms(elapsed) << " ms ("
                  << listed.size() << " names)\n\n";
    }

    // ---------------------------------------------------------------
    // Shipping mode: no resident name blob. Lookups go through precomputed
    // path hashes, so the names are dead weight in a build that never shows
    // an asset path to a human.
    // ---------------------------------------------------------------
    {
        const uint64_t before = ResidentBytes();
        PakReader reader;
        PakOpenOptions openOptions;
        openOptions.loadNames = false;

        auto start = Clock::now();
        if (!reader.Open(pakPath.string(), openOptions)) {
            std::cerr << "failed to open scale pak without names\n";
            return 1;
        }
        auto elapsed = Clock::now() - start;
        const uint64_t after = ResidentBytes();

        std::cout << "Open (no names) : " << Ms(elapsed) << " ms\n";
        if (before && after >= before) {
            std::cout << "  table resident: " << MiB(after - before) << " MiB ("
                      << ((after - before) / (fileCount ? fileCount : 1))
                      << " bytes/entry)\n";
        }

        std::vector<uint64_t> pathHashes;
        pathHashes.reserve(fileCount);
        for (const auto& name : names) pathHashes.push_back(PakPathHash(name));

        size_t hits = 0;
        start = Clock::now();
        for (uint64_t pathHash : pathHashes) {
            if (reader.FindByHash(pathHash)) ++hits;
        }
        elapsed = Clock::now() - start;
        std::cout << "FindByHash      : " << NsPerOp(elapsed, fileCount)
                  << " ns/lookup (" << hits << " hits)\n\n";
    }

    // ---------------------------------------------------------------
    // Layered mount: a shipping build mounts a base archive plus season
    // patches, DLC, and language packs.
    // ---------------------------------------------------------------
    {
        const uint64_t before = ResidentBytes();
        PakMount mount;
        auto start = Clock::now();
        for (size_t layer = 0; layer < layerCount; ++layer) {
            if (!mount.Mount(pakPath.string())) {
                std::cerr << "failed to mount layer " << layer << "\n";
                return 1;
            }
        }
        auto elapsed = Clock::now() - start;
        const uint64_t after = ResidentBytes();

        std::cout << "Mount " << layerCount << " layers : " << Ms(elapsed) << " ms ("
                  << (Ms(elapsed) / static_cast<double>(layerCount)) << " ms/layer)\n";
        if (before && after >= before)
            std::cout << "  mount resident: " << MiB(after - before) << " MiB\n";
        std::cout << "  merged entries: " << mount.GetFileCount() << "\n";
    }

    fs::remove_all(root, ec);
    return 0;
}
