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
    options.compress = false;
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
    auto t0 = std::chrono::steady_clock::now();
    size_t resolved = reader.Resolve(nameViews, handles);
    auto t1 = std::chrono::steady_clock::now();

    uint64_t checksum = 0;
    for (PakFileHandle handle : handles) {
        PakView view;
        if (reader.View(handle, view) == PakStatus::Ok && view.size > 0) {
            checksum += view.data[0];
        }
    }
    auto t2 = std::chrono::steady_clock::now();

    std::vector<uint8_t> buffer(fileSize);
    for (PakFileHandle handle : handles) {
        if (reader.Read(handle, buffer) == PakStatus::Ok) {
            checksum += buffer.back();
        }
    }
    auto t3 = std::chrono::steady_clock::now();

    std::cout << "resolved=" << resolved << "/" << fileCount << "\n";
    std::cout << "resolve_ms=" << Ms(t1 - t0) << "\n";
    std::cout << "view_ms=" << Ms(t2 - t1) << "\n";
    std::cout << "read_copy_ms=" << Ms(t3 - t2) << "\n";
    std::cout << "checksum=" << checksum << "\n";
    return resolved == fileCount ? 0 : 1;
}
