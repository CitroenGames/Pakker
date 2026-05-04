#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "Pak.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <vector>

// Helper: read entire file into a vector
std::vector<uint8_t> readEntireFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "Unable to open file: " << filename << std::endl;
        return {};
    }
    std::streamsize size = file.tellg();
    if (size < 0) {
        std::cerr << "Unable to determine file size: " << filename << std::endl;
        return {};
    }
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        std::cerr << "Unable to read file: " << filename << std::endl;
        return {};
    }
    return buffer;
}

// Example log callback -- wire into your engine's logging system
void exampleLogCallback(PakLogLevel level, const char* message) {
    switch (level) {
        case PakLogLevel::Error:   std::cerr << "[PAK ERROR] " << message << std::endl; break;
        case PakLogLevel::Warning: std::cerr << "[PAK WARN]  " << message << std::endl; break;
        case PakLogLevel::Info:    std::cout << "[PAK INFO]  " << message << std::endl; break;
    }
}

int main() {
    PakSetLogCallback(exampleLogCallback);

    // -----------------------------------------------------------------------
    // BUILD TIME: Create a page-aligned, compressed PAK using PakOptions
    // -----------------------------------------------------------------------
    Pakker pakker;
    std::map<std::string, std::vector<uint8_t>> files;

    std::string audioFilename = "example.mp3";
    std::vector<uint8_t> audioData = readEntireFile(audioFilename);
    if (audioData.empty()) {
        std::cerr << "Failed to read audio file." << std::endl;
        return 1;
    }

    files[audioFilename] = audioData;

    // Page-aligned PAK for optimal mmap performance and GPU upload
    PakOptions opts;
    opts.compress = true;       // LZ4 per-file compression
    opts.alignment = 4096;      // 4KB page alignment for memory-mapped I/O

    if (!pakker.CreatePak("assets.pak", files, opts)) {
        std::cerr << "Failed to create pak file." << std::endl;
        return 1;
    }

    pakker.ListPak("assets.pak");

    // -----------------------------------------------------------------------
    // RUNTIME: PakReader with memory-mapped I/O
    // -----------------------------------------------------------------------
    PakReader reader;

    if (!reader.Open("assets.pak")) {
        std::cerr << "Failed to open pak file for reading." << std::endl;
        return 1;
    }

    std::cout << "PAK has " << reader.GetFileCount() << " file(s)." << std::endl;
    std::cout << "Memory-mapped I/O: " << (reader.IsMapped() ? "yes" : "no") << std::endl;

    // Resolve once and cache the handle in your asset system
    PakFileHandle audioHandle = reader.Find(audioFilename);
    if (!audioHandle) {
        std::cerr << "Audio asset not found in PAK." << std::endl;
        return 1;
    }

    if (const PakFileInfo* info = reader.Info(audioHandle)) {
        std::cout << "Found '" << info->filename << "': "
                  << info->originalSize << " bytes"
                  << (info->compressed ? " (compressed)" : "") << std::endl;
    }

    // -----------------------------------------------------------------------
    // ZERO-COPY READ: Direct pointer into mmap for uncompressed assets
    // -----------------------------------------------------------------------
    {
        PakView view;
        PakStatus status = reader.View(audioHandle, view);
        if (status == PakStatus::Ok) {
            std::cout << "Mapped view: " << view.size << " bytes" << std::endl;
        } else {
            std::cout << "Mapped view unavailable: " << PakStatusToString(status) << std::endl;
        }
        // view keeps the mapping alive even if reader.Close() is called later.
    }

    // -----------------------------------------------------------------------
    // THREAD-SAFE CONCURRENT READS
    // -----------------------------------------------------------------------
    {
        const int numThreads = 4;
        std::vector<std::thread> threads;
        threads.reserve(numThreads);

        std::cout << "Launching " << numThreads << " concurrent read threads..." << std::endl;

        for (int i = 0; i < numThreads; ++i) {
            threads.emplace_back([&reader, audioHandle, i]() {
                std::vector<uint8_t> data;
                PakStatus status = reader.Load(audioHandle, data);
                std::cout << "  Thread " << i << ": read "
                          << data.size() << " bytes (" << PakStatusToString(status) << ")"
                          << std::endl;
            });
        }

        for (auto& t : threads) t.join();
        std::cout << "All threads completed." << std::endl;
    }

    // -----------------------------------------------------------------------
    // BATCH RESOLVE: Feed handles to your engine job system
    // -----------------------------------------------------------------------
    std::string_view batchNames[] = {audioFilename};
    PakFileHandle batchHandles[1];
    std::cout << "Batch resolve found "
              << reader.Resolve(batchNames, batchHandles) << " file(s)." << std::endl;

    // Load audio for playback
    std::vector<uint8_t> pakAudioData;
    if (reader.Load(audioHandle, pakAudioData) != PakStatus::Ok) {
        std::cerr << "Failed to load audio from PAK file." << std::endl;
        return 1;
    }

    // -----------------------------------------------------------------------
    // Audio playback demo using miniaudio
    // -----------------------------------------------------------------------
    std::string tempAudioPath = "temp_audio.mp3";
    {
        std::ofstream tempFile(tempAudioPath, std::ios::binary);
        tempFile.write(reinterpret_cast<const char*>(pakAudioData.data()),
                       static_cast<std::streamsize>(pakAudioData.size()));
    }

    ma_result result;
    ma_engine engine;

    result = ma_engine_init(NULL, &engine);
    if (result != MA_SUCCESS) {
        std::cerr << "Failed to initialize audio engine." << std::endl;
        return 1;
    }

    ma_sound sound;
    result = ma_sound_init_from_file(&engine, tempAudioPath.c_str(), 0, NULL, NULL, &sound);
    if (result != MA_SUCCESS) {
        std::cerr << "Failed to create sound from file." << std::endl;
        ma_engine_uninit(&engine);
        return 1;
    }

    ma_sound_start(&sound);

    std::cout << "Playing audio from PAK file. Press Enter to stop..." << std::endl;
    std::cin.get();

    ma_sound_stop(&sound);
    ma_sound_uninit(&sound);
    ma_engine_uninit(&engine);

    std::filesystem::remove(tempAudioPath);

    return 0;
}
