// Read-path throughput benchmark: decode bandwidth and per-read latency
// across a realistic entry-size ladder and all three storage modes
// (uncompressed / LZ4 / Zstd), plus the two costs that whole-entry
// compression imposes on a streaming engine:
//
//   * time-to-first-byte on a large asset -- with no chunk index, the entire
//     entry must be decoded before any of it is usable
//   * ReadRange on a compressed entry -- currently unsupported for the same
//     reason
//
// It also measures content-hash (integrity) bandwidth, which sets the floor
// for "verify game files" over a full install.
//
// The OS page cache is warm for these numbers by design: the goal is to
// isolate CPU cost in the read path, not to measure the storage device.

#include "Pak.h"
#include "BenchSupport.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace bench;

namespace {

struct SizeClass {
    const char* label;
    size_t bytes;
    size_t count;
};

// A shipping archive is dominated by small entries by count and by large
// entries by volume; the ladder spans both so per-read overhead and raw
// bandwidth are both visible.
const SizeClass kLadder[] = {
    { "4 KiB",   4ull * 1024,          256 },
    { "64 KiB",  64ull * 1024,          64 },
    { "1 MiB",   1024ull * 1024,        16 },
    { "16 MiB",  16ull * 1024 * 1024,    4 },
    { "64 MiB",  64ull * 1024 * 1024,    1 },
};

std::string EntryName(const SizeClass& sizeClass, size_t index)
{
    std::string name = "bench/";
    name += sizeClass.label;
    name += "/entry_";
    name += std::to_string(index);
    name += ".bin";
    // Path separators must be archive-legal; the ladder labels contain a
    // space, which is fine, but strip anything that would trip validation.
    for (char& c : name) {
        if (c == ' ') c = '_';
    }
    return name;
}

bool BuildArchive(const fs::path& path, PakCompression compression, int zstdLevel,
                  const std::map<std::string, std::vector<uint8_t>>& files)
{
    PakOptions options;
    options.compression = compression;
    options.zstdLevel = zstdLevel;
    options.alignment = 4096;

    Pakker pakker;
    auto start = Clock::now();
    if (!pakker.CreatePak(path.string(), files, options)) return false;
    auto elapsed = Clock::now() - start;

    uint64_t rawBytes = 0;
    for (const auto& [name, data] : files) rawBytes += data.size();

    std::error_code ec;
    const uint64_t archiveBytes = fs::file_size(path, ec);

    std::cout << "  built " << std::left << std::setw(10)
              << (compression == PakCompression::None ? "store"
                  : compression == PakCompression::LZ4 ? "lz4" : "zstd")
              << std::right << std::fixed << std::setprecision(2)
              << " in " << std::setw(9) << Ms(elapsed) << " ms"
              << "  ratio " << std::setw(6)
              << (archiveBytes ? static_cast<double>(rawBytes) / static_cast<double>(archiveBytes) : 0.0)
              << ":1"
              << "  (" << std::setw(8) << GiBPerSecond(elapsed, rawBytes) << " GiB/s in)\n";
    return true;
}

// Reads every entry of one size class and reports bandwidth plus the latency
// distribution, which is what a frame-budgeted streamer actually cares about.
//
// The reader's cache policy decides what this actually measures. With the
// decoded cache on, repeated reads of the same entry are served by memcpy
// from the cache and the number says nothing about codec speed -- so the
// caller runs this once with the cache off (true decode bandwidth) and once
// with it warm (cache-hit bandwidth), and reports the two separately.
void MeasureSizeClass(PakReader& reader, const SizeClass& sizeClass, const char* mode)
{
    std::vector<PakFileHandle> handles;
    handles.reserve(sizeClass.count);
    for (size_t i = 0; i < sizeClass.count; ++i) {
        PakFileHandle handle = reader.Find(EntryName(sizeClass, i));
        if (handle) handles.push_back(handle);
    }
    if (handles.empty()) return;

    std::vector<uint8_t> destination(sizeClass.bytes);

    // Warm the pages so this measures decode, not first-touch faulting.
    for (PakFileHandle handle : handles) reader.Read(handle, destination);

    // Enough repeats that the small classes get a meaningful sample without
    // the 64 MiB class taking all day.
    const size_t repeats = sizeClass.bytes >= (16ull << 20) ? 4
                         : sizeClass.bytes >= (1ull << 20) ? 16 : 64;

    Latencies latencies;
    latencies.Reserve(handles.size() * repeats);

    uint64_t bytesRead = 0;
    auto start = Clock::now();
    for (size_t r = 0; r < repeats; ++r) {
        for (PakFileHandle handle : handles) {
            auto readStart = Clock::now();
            uint64_t written = 0;
            if (reader.Read(handle, destination, &written) == PakStatus::Ok) {
                bytesRead += written;
            }
            latencies.Add(Clock::now() - readStart);
        }
    }
    auto elapsed = Clock::now() - start;

    std::string label = std::string(mode) + " " + sizeClass.label;
    std::cout << "  " << std::left << std::setw(18) << label << std::right
              << std::fixed << std::setprecision(2)
              << std::setw(9) << GiBPerSecond(elapsed, bytesRead) << " GiB/s"
              << "   p50 " << std::setw(9) << latencies.Percentile(0.50) / 1000.0 << " us"
              << "   p99 " << std::setw(9) << latencies.Percentile(0.99) / 1000.0 << " us\n";
}

} // namespace

int main()
{
    fs::path root = fs::temp_directory_path() / "pakker_throughput_benchmark";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // -----------------------------------------------------------------
    // Corpus
    // -----------------------------------------------------------------
    std::map<std::string, std::vector<uint8_t>> files;
    uint64_t rawBytes = 0;
    uint64_t seed = 1;
    for (const auto& sizeClass : kLadder) {
        for (size_t i = 0; i < sizeClass.count; ++i) {
            files.emplace(EntryName(sizeClass, i), MakeAssetPayload(sizeClass.bytes, seed++));
            rawBytes += sizeClass.bytes;
        }
    }

    std::cout << "Pakker throughput benchmark\n"
              << "  corpus: " << MiB(rawBytes) << " MiB across " << files.size() << " entries\n\n";

    const fs::path storePath = root / "store.pak";
    const fs::path lz4Path = root / "lz4.pak";
    const fs::path zstdPath = root / "zstd.pak";

    Header("Build");
    if (!BuildArchive(storePath, PakCompression::None, 0, files) ||
        !BuildArchive(lz4Path, PakCompression::LZ4, 0, files) ||
        !BuildArchive(zstdPath, PakCompression::Zstd, 3, files)) {
        std::cerr << "failed to build benchmark archives\n";
        return 1;
    }

    files.clear();

    // -----------------------------------------------------------------
    // Decode bandwidth and latency across the ladder
    // -----------------------------------------------------------------
    struct Variant {
        const char* mode;
        const fs::path* path;
    };
    const Variant variants[] = {
        { "store", &storePath },
        { "lz4",   &lz4Path },
        { "zstd",  &zstdPath },
    };

    Header("Decode bandwidth, decoded cache OFF (true codec cost)");
    for (const auto& variant : variants) {
        PakReader reader;
        if (!reader.Open(variant.path->string())) {
            std::cerr << "failed to open " << variant.path->string() << "\n";
            return 1;
        }
        PakCacheOptions cacheOptions;
        cacheOptions.enabled = false;
        reader.SetCacheOptions(cacheOptions);

        for (const auto& sizeClass : kLadder) {
            MeasureSizeClass(reader, sizeClass, variant.mode);
        }
        std::cout << "\n";
    }

    // The same reads with the decoded cache warm. This is the ceiling the
    // cache can deliver, but a full-size install's working set does not fit
    // in any cache budget -- so the numbers above are what streaming actually
    // sees for the overwhelming majority of reads.
    Header("Read bandwidth, decoded cache ON and warm (hit path)");
    for (size_t v = 1; v < 3; ++v) {
        PakReader reader;
        if (!reader.Open(variants[v].path->string())) return 1;

        PakCacheOptions cacheOptions;
        cacheOptions.enabled = true;
        cacheOptions.persistentCacheEnabled = false;
        cacheOptions.memoryBudgetBytes = 512ull * 1024 * 1024;
        cacheOptions.maxSingleEntryBytes = 128ull * 1024 * 1024;
        reader.SetCacheOptions(cacheOptions);

        for (const auto& sizeClass : kLadder) {
            MeasureSizeClass(reader, sizeClass, variants[v].mode);
        }
        std::cout << "\n";
    }

    // -----------------------------------------------------------------
    // Zero-copy View() -- the uncompressed mmap fast path
    // -----------------------------------------------------------------
    Header("Zero-copy View() vs copying Read() (uncompressed, 1 MiB)");
    {
        PakReader reader;
        reader.Open(storePath.string());

        const SizeClass& sizeClass = kLadder[2]; // 1 MiB
        std::vector<PakFileHandle> handles;
        for (size_t i = 0; i < sizeClass.count; ++i) {
            PakFileHandle handle = reader.Find(EntryName(sizeClass, i));
            if (handle) handles.push_back(handle);
        }

        Latencies viewLatencies, readLatencies;
        std::vector<uint8_t> destination(sizeClass.bytes);
        for (size_t r = 0; r < 64; ++r) {
            for (PakFileHandle handle : handles) {
                auto start = Clock::now();
                PakView view;
                reader.View(handle, view);
                viewLatencies.Add(Clock::now() - start);

                start = Clock::now();
                reader.Read(handle, destination);
                readLatencies.Add(Clock::now() - start);
            }
        }
        PrintLatencies("View() zero-copy", viewLatencies);
        PrintLatencies("Read() into buffer", readLatencies);
    }

    // -----------------------------------------------------------------
    // Time-to-first-byte on a large asset
    //
    // Whole-entry compression means the whole 64 MiB must be decoded before
    // the first byte is usable. On an uncompressed entry, ReadRange serves
    // a 64 KiB slice directly. This gap is the cost of having no chunk index.
    // -----------------------------------------------------------------
    Header("Time-to-first-byte, 64 MiB asset (first 64 KiB usable)");
    {
        const SizeClass& big = kLadder[4];
        const std::string name = EntryName(big, 0);
        constexpr size_t kSlice = 64ull * 1024;

        {
            PakReader reader;
            reader.Open(storePath.string());
            PakFileHandle handle = reader.Find(name);
            std::vector<uint8_t> slice(kSlice);

            Latencies latencies;
            for (size_t r = 0; r < 32; ++r) {
                auto start = Clock::now();
                reader.ReadRange(handle, 0, slice);
                latencies.Add(Clock::now() - start);
            }
            PrintLatencies("store  ReadRange 64 KiB", latencies);
        }

        for (const auto& variant : { variants[1], variants[2] }) {
            PakReader reader;
            reader.Open(variant.path->string());
            // Without this, iterations after the first hit the decoded cache
            // and report memcpy speed instead of decode speed.
            PakCacheOptions cacheOptions;
            cacheOptions.enabled = false;
            reader.SetCacheOptions(cacheOptions);
            PakFileHandle handle = reader.Find(name);

            std::vector<uint8_t> slice(kSlice);
            PakStatus rangeStatus = reader.ReadRange(handle, 0, slice);

            std::vector<uint8_t> full;
            Latencies latencies;
            for (size_t r = 0; r < 4; ++r) {
                auto start = Clock::now();
                reader.Load(handle, full);
                latencies.Add(Clock::now() - start);
            }
            std::string label = std::string(variant.mode) + "   full decode";
            PrintLatencies(label.c_str(), latencies);
            std::cout << "    " << variant.mode << " ReadRange 64 KiB -> "
                      << PakStatusToString(rangeStatus) << "\n";
        }
    }

    // -----------------------------------------------------------------
    // Integrity hashing bandwidth -- the "verify game files" floor
    // -----------------------------------------------------------------
    Header("Content-hash bandwidth (VerifyEntry)");
    {
        PakReader reader;
        reader.Open(storePath.string());

        uint64_t hashedBytes = 0;
        const uint32_t count = reader.GetFileCount();
        auto start = Clock::now();
        for (uint32_t i = 0; i < count; ++i) {
            PakFileHandle handle{i};
            const PakFileInfo* info = reader.Info(handle);
            if (!info) continue;
            if (reader.VerifyEntry(handle) == PakStatus::Ok) {
                hashedBytes += info->compressedSize;
            }
        }
        auto elapsed = Clock::now() - start;

        Row("VerifyEntry (whole archive)", GiBPerSecond(elapsed, hashedBytes), "GiB/s");
        Row("  wall time", Ms(elapsed), "ms");

        // What that rate implies for a full-size install.
        const double gibPerSec = GiBPerSecond(elapsed, hashedBytes);
        if (gibPerSec > 0.0) {
            Row("  extrapolated 150 GiB verify", 150.0 / gibPerSec, "s");
        }
    }

    fs::remove_all(root, ec);
    return 0;
}
