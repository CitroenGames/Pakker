#include "pak.h"
#include <fstream>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

void Pakker::EncryptDecrypt(std::vector<uint8_t>& data)
{
    const std::string key = ENCRYPTION_KEY;
    size_t keyLength = key.length(); // safer than strlen
    for (size_t i = 0; i < data.size(); ++i) 
    {
        data[i] ^= key[i % keyLength];
    }
}

bool Pakker::CreatePak(const std::string& pakFilename, const std::map<std::string, std::vector<uint8_t>>& files) 
{
    std::ofstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) 
    {
        std::cerr << __FUNCTION__ << ": Error unable to create pak file." << std::endl;
        return false;
    }

    uint32_t numFiles = files.size();
    pakStream.write(reinterpret_cast<char*>(&numFiles), sizeof(numFiles));

    uint32_t fileTableOffset = pakStream.tellp();
    pakStream.seekp(fileTableOffset + numFiles * (256 + sizeof(uint32_t) * 2));

    for (const auto& [filename, data] : files) 
    {
        std::vector<uint8_t> encryptedData = data;
        EncryptDecrypt(encryptedData);

        PakEntry entry;
        entry.filename = filename;
        entry.offset = pakStream.tellp();
        entry.size = encryptedData.size();
        entries.push_back(entry);

        pakStream.write(reinterpret_cast<char*>(encryptedData.data()), encryptedData.size());
    }

    pakStream.seekp(fileTableOffset);
    for (const auto& entry : entries) 
    {
        char filenameBuffer[256] = { 0 };
        strncpy(filenameBuffer, entry.filename.c_str(), sizeof(filenameBuffer) - 1);
        pakStream.write(filenameBuffer, sizeof(filenameBuffer));
        pakStream.write(reinterpret_cast<const char*>(&entry.offset), sizeof(entry.offset));
        pakStream.write(reinterpret_cast<const char*>(&entry.size), sizeof(entry.size));
    }

    pakStream.close();
    return true;
}

bool Pakker::ExtractPak(const std::string& pakFilename, const std::string& outputDir) 
{
    std::ifstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        std::cerr << __FUNCTION__ << ": Error unable to open pak file." << std::endl;
        return false;
    }

    // Read number of files
    uint32_t numFiles;
    pakStream.read(reinterpret_cast<char*>(&numFiles), sizeof(numFiles));

    // Read file table
    entries.clear();
    for (uint32_t i = 0; i < numFiles; ++i) {
        char filename[256] = {0};
        uint32_t offset, size;
        pakStream.read(filename, sizeof(filename));
        pakStream.read(reinterpret_cast<char*>(&offset), sizeof(offset));
        pakStream.read(reinterpret_cast<char*>(&size), sizeof(size));

        PakEntry entry;
        entry.filename = filename;
        entry.offset = offset;
        entry.size = size;
        entries.push_back(entry);
    }

    // Extract files
    for (const auto& entry : entries) {
        std::string fullPath = outputDir + "/" + entry.filename;
        fs::create_directories(fs::path(fullPath).parent_path());
        pakStream.seekg(entry.offset);
        std::vector<uint8_t> fileData(entry.size);
        pakStream.read(reinterpret_cast<char*>(fileData.data()), entry.size);

        EncryptDecrypt(fileData);  // Decrypt the file data

        std::string outputPath = outputDir + "/" + entry.filename;
        if (!WriteFile(outputPath, fileData)) {
            std::cerr << __FUNCTION__ << ": Error unable to write file " << outputPath << std::endl;
            return false;
        }
    }

    pakStream.close();
    return true;
}

bool Pakker::ListPak(const std::string& pakFilename) 
{
    std::ifstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        std::cerr << __FUNCTION__ << ": Error unable to open pak file." << std::endl;
        return false;
    }

    // Read number of files
    uint32_t numFiles;
    pakStream.read(reinterpret_cast<char*>(&numFiles), sizeof(numFiles));

    // Read file table
    entries.clear();
    for (uint32_t i = 0; i < numFiles; ++i) {
        char filename[256] = { 0 };
        uint32_t offset, size;
        pakStream.read(filename, sizeof(filename));
        pakStream.read(reinterpret_cast<char*>(&offset), sizeof(offset));
        pakStream.read(reinterpret_cast<char*>(&size), sizeof(size));

        PakEntry entry;
        entry.filename = filename;
        entry.offset = offset;
        entry.size = size;
        entries.push_back(entry);
    }

    // List files
    for (const auto& entry : entries) {
        std::cout << entry.filename << " (Offset: " << entry.offset << ", Size: " << entry.size << ")" << std::endl;
    }

    pakStream.close();
    return true;
}

std::vector<uint8_t> Pakker::ReadFileFromPak(const std::string& pakFilename, const std::string& filename) 
{
    std::ifstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        std::cerr << __FUNCTION__ << ": Error unable to open pak file." << std::endl;
        return {};
    }

    uint32_t numFiles;
    pakStream.read(reinterpret_cast<char*>(&numFiles), sizeof(numFiles));

    for (uint32_t i = 0; i < numFiles; ++i) {
        char entryFilename[256] = { 0 };
        uint32_t offset, size;
        pakStream.read(entryFilename, sizeof(entryFilename));
        pakStream.read(reinterpret_cast<char*>(&offset), sizeof(offset));
        pakStream.read(reinterpret_cast<char*>(&size), sizeof(size));

        if (filename == entryFilename) {
            std::vector<uint8_t> fileData(size);
            pakStream.seekg(offset);
            pakStream.read(reinterpret_cast<char*>(fileData.data()), size);
            EncryptDecrypt(fileData);
            return fileData;
        }
    }

    std::cerr << __FUNCTION__ << ": Error file not found in pak." << std::endl;
    return {};
}

std::shared_ptr<std::vector<uint8_t>> Pakker::LoadFile(const std::string& pakFilename, const std::string& filename) 
{
    std::vector<uint8_t> fileData = ReadFileFromPak(pakFilename, filename);
    if (fileData.empty()) {
        return nullptr;
    }
    return std::make_shared<std::vector<uint8_t>>(std::move(fileData));
}

bool Pakker::AddFileToPak(const std::string& pakFilename, const std::string& filename, const std::vector<uint8_t>& data) 
{
    std::fstream pakStream(pakFilename, std::ios::in | std::ios::out | std::ios::binary);
    if (!pakStream) {
        std::cerr << __FUNCTION__ << ": Error unable to open pak file." << std::endl;
        return false;
    }

    // Read number of files
    uint32_t numFiles;
    pakStream.read(reinterpret_cast<char*>(&numFiles), sizeof(numFiles));
    numFiles++;

    // Read existing file table
    std::vector<PakEntry> existingEntries;
    for (uint32_t i = 0; i < numFiles - 1; ++i) 
    {
        char entryFilename[256] = { 0 };
        uint32_t offset, size;
        pakStream.read(entryFilename, sizeof(entryFilename));
        pakStream.read(reinterpret_cast<char*>(&offset), sizeof(offset));
        pakStream.read(reinterpret_cast<char*>(&size), sizeof(size));

        PakEntry entry;
        entry.filename = entryFilename;
        entry.offset = offset;
        entry.size = size;
        existingEntries.push_back(entry);
    }

    // Add new file data
    pakStream.seekp(0, std::ios::end);
    std::vector<uint8_t> encryptedData = data;
    EncryptDecrypt(encryptedData);

    PakEntry newEntry;
    newEntry.filename = filename;
    newEntry.offset = pakStream.tellp();
    newEntry.size = encryptedData.size();

    pakStream.write(reinterpret_cast<char*>(encryptedData.data()), encryptedData.size());

    // Update file table
    pakStream.seekp(sizeof(uint32_t));
    pakStream.write(reinterpret_cast<char*>(&numFiles), sizeof(numFiles));

    for (const auto& entry : existingEntries) 
    {
        char filenameBuffer[256] = { 0 };
        strncpy(filenameBuffer, entry.filename.c_str(), sizeof(filenameBuffer) - 1);
        pakStream.write(filenameBuffer, sizeof(filenameBuffer));
        pakStream.write(reinterpret_cast<const char*>(&entry.offset), sizeof(entry.offset));
        pakStream.write(reinterpret_cast<const char*>(&entry.size), sizeof(entry.size));
    }

    char newFilenameBuffer[256] = { 0 };
    strncpy(newFilenameBuffer, newEntry.filename.c_str(), sizeof(newFilenameBuffer) - 1);
    pakStream.write(newFilenameBuffer, sizeof(newFilenameBuffer));
    pakStream.write(reinterpret_cast<const char*>(&newEntry.offset), sizeof(newEntry.offset));
    pakStream.write(reinterpret_cast<const char*>(&newEntry.size), sizeof(newEntry.size));

    pakStream.close();
    return true;
}

// helper function to normalize path separators
std::string normalizePathSeparators(const std::string& path) 
{
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return normalized;
}

bool Pakker::CreatePakFromFolder(const std::string& pakFilename, const std::string& folderPath) 
{
    std::map<std::string, std::vector<uint8_t>> files;

    for (const auto& entry : fs::recursive_directory_iterator(folderPath)) 
    {
        if (fs::is_regular_file(entry.path())) {
            std::string relativePath = fs::relative(entry.path(), folderPath).string();
            relativePath = normalizePathSeparators(relativePath);  // Normalize to forward slashes
            std::ifstream file(entry.path(), std::ios::binary);
            std::vector<uint8_t> content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            files[relativePath] = content;
        }
    }

    return CreatePak(pakFilename, files);
}

bool Pakker::WriteFile(const std::string& filename, const std::vector<uint8_t>& buffer) 
{
    std::ofstream fileStream(filename, std::ios::binary);
    if (!fileStream) {
        return false;
    }
    fileStream.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    return true;
}