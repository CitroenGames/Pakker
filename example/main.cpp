#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "Pak.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

// Helper: read entire file into a vector
std::vector<uint8_t> readEntireFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "Unable to open file: " << filename << std::endl;
        return {};
    }
    std::streamsize size = file.tellg();
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
    // Enable logging for this example (in shipping builds, leave it null for silence)
    PakSetLogCallback(exampleLogCallback);

    // -----------------------------------------------------------------------
    // BUILD TIME: Create a compressed PAK file using Pakker
    // -----------------------------------------------------------------------
    Pakker pakker; // no encryption key = no encryption (fastest)
    std::map<std::string, std::vector<uint8_t>> files;

    std::string audioFilename = "example.mp3";
    std::vector<uint8_t> audioData = readEntireFile(audioFilename);
    if (audioData.empty()) {
        std::cerr << "Failed to read audio file." << std::endl;
        return 1;
    }

    files[audioFilename] = audioData;

    // Create a compressed PAK (LZ4)
    if (!pakker.CreatePak("assets.pak", files, true)) {
        std::cerr << "Failed to create pak file." << std::endl;
        return 1;
    }

    // List contents (build-time utility)
    pakker.ListPak("assets.pak");

    // -----------------------------------------------------------------------
    // RUNTIME: Use PakReader for shipping builds (open once, read many)
    // -----------------------------------------------------------------------
    PakReader reader; // no encryption key = no encryption

    if (!reader.Open("assets.pak")) {
        std::cerr << "Failed to open pak file for reading." << std::endl;
        return 1;
    }

    std::cout << "PAK has " << reader.GetFileCount() << " file(s)." << std::endl;

    // O(1) file existence check (no disk I/O)
    if (reader.FileExists(audioFilename)) {
        auto info = reader.GetFileInfo(audioFilename);
        std::cout << "Found '" << info.filename << "': "
                  << info.originalSize << " bytes"
                  << (info.compressed ? " (compressed)" : "") << std::endl;
    }

    // Load audio from PAK
    auto pakAudioData = reader.LoadFile(audioFilename);
    if (!pakAudioData) {
        std::cerr << "Failed to load audio from PAK file." << std::endl;
        return 1;
    }

    // Batch read example (reads sorted by offset for sequential I/O)
    auto batchResults = reader.ReadFiles({audioFilename});
    std::cout << "Batch read returned " << batchResults.size() << " file(s)." << std::endl;

    // Reader stays open -- no need to reparse the file table for subsequent reads
    // reader.Close() is called automatically by the destructor

    // -----------------------------------------------------------------------
    // Audio playback demo using miniaudio
    // Write the audio data to a temp file since miniaudio needs a file path
    // -----------------------------------------------------------------------
    std::string tempAudioPath = "temp_audio.mp3";
    {
        std::ofstream tempFile(tempAudioPath, std::ios::binary);
        tempFile.write(reinterpret_cast<const char*>(pakAudioData->data()),
                       static_cast<std::streamsize>(pakAudioData->size()));
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
