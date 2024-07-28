#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "pak.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

// Function to read entire file into a vector
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

bool loadFileToBuffer(const std::string& filename, std::vector<uint8_t>& buffer) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    buffer.resize(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data(), size))) {
        return false;
    }

    return true;
}

int main() {
    PakFile pak;
    std::map<std::string, std::vector<uint8_t>> files;

    // Read an actual audio file
    std::string audioFilename = "example.mp3";  // Make sure this file exists
    std::vector<uint8_t> audioData = readEntireFile(audioFilename);
    if (audioData.empty()) {
        std::cerr << "Failed to read audio file." << std::endl;
        return 1;
    }

    files[audioFilename] = audioData;

    // Create encrypted .pak file
    if (pak.createPak("assets.pak", files)) {
        std::cout << "Encrypted pak file created successfully." << std::endl;
    }
    else {
        std::cerr << "Failed to create encrypted pak file." << std::endl;
        return 1;
    }

    // List contents of encrypted .pak file
    if (pak.listPak("assets.pak")) {
        std::cout << "Listed encrypted pak file contents successfully." << std::endl;
    }
    else {
        std::cerr << "Failed to list encrypted pak file contents." << std::endl;
        return 1;
    }

    // Load audio file from PAK
    auto pakAudioData = pak.loadFile("assets.pak", audioFilename);
    if (!pakAudioData) {
        std::cerr << "Failed to load audio from PAK file." << std::endl;
        return 1;
    }

    // Initialize miniaudio
    ma_result result;
    ma_engine engine;

    result = ma_engine_init(NULL, &engine);
    if (result != MA_SUCCESS) {
        std::cerr << "Failed to initialize audio engine." << std::endl;
        return 1;
    }

    // Create a sound from the memory buffer
    ma_sound sound;
    result = ma_sound_init_from_data(&engine, pakAudioData->data(), pakAudioData->size(), NULL, NULL, &sound);
    ma_sound_init_from_memory(&engine, pakAudioData.data(), pakAudioData.size(), NULL, NULL, &sound);
    if (result != MA_SUCCESS) {
        std::cerr << "Failed to create sound from memory." << std::endl;
        ma_engine_uninit(&engine);
        return 1;
    }

    // Play the sound
    ma_sound_start(&sound);

    std::cout << "Playing audio from PAK file. Press Enter to stop..." << std::endl;
    std::cin.get();

    // Stop the sound and clean up
    ma_sound_stop(&sound);
    ma_sound_uninit(&sound);
    ma_engine_uninit(&engine);

    return 0;
}