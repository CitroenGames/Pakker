#include "pak.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <limits>

namespace fs = std::filesystem;

// Constructor initializes the encryption key
Pakker::Pakker(const std::string& encryptionKey)
    : encryptionKey_(encryptionKey)
{
    // Ensure we have a non-empty key for better security
    if (encryptionKey_.empty()) {
        encryptionKey_ = "default_fallback_key";
    }
}

// Encrypts or decrypts data in-place using XOR with the encryption key
void Pakker::EncryptDecrypt(std::vector<uint8_t>& data) const
{
    if (data.empty() || encryptionKey_.empty()) return;
    
    const size_t keyLength = encryptionKey_.length();
    for (size_t i = 0; i < data.size(); ++i)
    {
        data[i] ^= static_cast<uint8_t>(encryptionKey_[i % keyLength]);
    }
}

// Normalizes path separators to '/' and validates path
std::string Pakker::NormalizePathSeparators(const std::string& path) const
{
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    
    // Remove any leading slashes to prevent absolute paths
    while (!normalized.empty() && normalized[0] == '/') {
        normalized.erase(0, 1);
    }
    
    return normalized;
}

// Validates filename for security
bool Pakker::IsValidFilename(const std::string& filename) const
{
    if (filename.empty() || filename.length() > MAX_FILENAME_LENGTH) {
        return false;
    }
    
    // Check for directory traversal attempts
    if (filename.find("..") != std::string::npos) {
        return false;
    }
    
    // Check for null bytes
    if (filename.find('\0') != std::string::npos) {
        return false;
    }
    
    // Check for invalid characters (platform-specific, but these are generally unsafe)
    const std::string invalidChars = "<>:\"|?*";
    for (char c : invalidChars) {
        if (filename.find(c) != std::string::npos) {
            return false;
        }
    }
    
    return true;
}

// Safely converts stream position to uint64_t
uint64_t Pakker::SafeStreamPos(std::streampos pos) const
{
    if (pos == std::streampos(-1)) {
        return 0;
    }
    return static_cast<uint64_t>(pos);
}

// Validates file table entry
bool Pakker::ValidateEntry(const PakEntry& entry, uint64_t pakFileSize) const
{
    // Check for overflow in offset + size
    if (entry.offset > pakFileSize || 
        entry.size > pakFileSize ||
        entry.offset + entry.size > pakFileSize) {
        return false;
    }
    
    return IsValidFilename(entry.filename);
}

// Improved WriteFile with better error handling
bool Pakker::WriteFile(const std::string& filename, const std::vector<uint8_t>& buffer) const
{
    if (buffer.empty()) {
        std::cerr << "WriteFile: Empty buffer for file: " << filename << std::endl;
        return false;
    }
    
    std::ofstream fileStream(filename, std::ios::binary);
    if (!fileStream) {
        std::cerr << "WriteFile: Unable to create file: " << filename << std::endl;
        return false;
    }
    
    fileStream.write(reinterpret_cast<const char*>(buffer.data()), 
                     static_cast<std::streamsize>(buffer.size()));
    
    if (!fileStream) {
        std::cerr << "WriteFile: Failed to write data to file: " << filename << std::endl;
        return false;
    }
    
    return true;
}

// Improved ReadPakHeader with better validation
bool Pakker::ReadPakHeader(std::istream& stream, PakHeader& header) const
{
    // Clear header first
    std::memset(&header, 0, sizeof(header));
    
    // Read the entire header at once for better performance
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!stream) {
        std::cerr << "ReadPakHeader: Failed to read PAK header." << std::endl;
        return false;
    }

    // Validate magic number
    if (std::memcmp(header.magic, PAK_MAGIC.data(), 4) != 0) {
        std::cerr << "ReadPakHeader: Invalid magic number." << std::endl;
        return false;
    }

    // Validate version
    if (header.version != SUPPORTED_VERSION) {
        std::cerr << "ReadPakHeader: Unsupported PAK version: " << header.version << std::endl;
        return false;
    }
    
    // Validate number of files
    if (header.numFiles > MAX_FILES_IN_PAK) {
        std::cerr << "ReadPakHeader: Too many files in PAK: " << header.numFiles << std::endl;
        return false;
    }

    return true;
}

// Improved WritePakHeader
bool Pakker::WritePakHeader(std::ostream& stream, const PakHeader& header) const
{
    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!stream) {
        std::cerr << "WritePakHeader: Failed to write header." << std::endl;
        return false;
    }
    return true;
}

// Improved ReadFileTable with validation
bool Pakker::ReadFileTable(std::istream& stream, uint32_t numFiles, std::vector<PakEntry>& entries) const
{
    entries.clear();
    entries.reserve(numFiles);  // Reserve space for better performance
    
    for (uint32_t i = 0; i < numFiles; ++i) {
        uint16_t nameLength;
        stream.read(reinterpret_cast<char*>(&nameLength), sizeof(nameLength));
        if (!stream || nameLength == 0 || nameLength > MAX_FILENAME_LENGTH) {
            std::cerr << "ReadFileTable: Invalid filename length: " << nameLength << std::endl;
            return false;
        }

        std::string filename(nameLength, '\0');
        stream.read(&filename[0], nameLength);
        if (!stream)
        {
            std::cerr << "ReadFileTable: Failed to read filename." << std::endl;
            return false;
        }

        uint64_t offset, size;
        stream.read(reinterpret_cast<char*>(&offset), sizeof(offset));
        stream.read(reinterpret_cast<char*>(&size), sizeof(size));
        if (!stream)
        {
            std::cerr << "ReadFileTable: Failed to read file entry for: " << filename << std::endl;
            return false;
        }

        // Validate the entry before adding it
        PakEntry entry(std::move(filename), offset, size);
        if (!IsValidFilename(entry.filename)) {
            std::cerr << "ReadFileTable: Invalid filename: " << entry.filename << std::endl;
            return false;
        }

        entries.emplace_back(std::move(entry));
    }
    return true;
}

// Improved WriteFileTable with better error handling
bool Pakker::WriteFileTable(std::ostream& stream, const std::vector<PakEntry>& entries) const
{
    for (const auto& entry : entries) {
        if (entry.filename.length() > MAX_FILENAME_LENGTH) {
            std::cerr << "WriteFileTable: Filename too long: " << entry.filename << std::endl;
            return false;
        }
        
        uint16_t nameLength = static_cast<uint16_t>(entry.filename.size());
        stream.write(reinterpret_cast<const char*>(&nameLength), sizeof(nameLength));
        stream.write(entry.filename.data(), nameLength);
        stream.write(reinterpret_cast<const char*>(&entry.offset), sizeof(entry.offset));
        stream.write(reinterpret_cast<const char*>(&entry.size), sizeof(entry.size));
        if (!stream)
        {
            std::cerr << "WriteFileTable: Failed to write file entry for: " << entry.filename << std::endl;
            return false;
        }
    }
    return true;
}

// Creates a PAK file from a map of filenames to their data
bool Pakker::CreatePak(const std::string& pakFilename, const std::map<std::string, std::vector<uint8_t>>& files)
{
    if (files.size() > MAX_FILES_IN_PAK) {
        std::cerr << "CreatePak: Too many files to pack: " << files.size() << std::endl;
        return false;
    }
    
    std::ofstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        std::cerr << "CreatePak: Unable to create pak file: " << pakFilename << std::endl;
        return false;
    }

    PakHeader header;
    header.numFiles = static_cast<uint32_t>(files.size());
    header.fileTableOffset = 0; // Placeholder, will update later

    if (!WritePakHeader(pakStream, header))
    {
        std::cerr << "CreatePak: Failed to write pak header." << std::endl;
        return false;
    }

    std::vector<PakEntry> entries;
    entries.reserve(files.size());

    // Write file data and collect entries
    for (const auto& [filename, data] : files) {
        std::string normalizedFilename = NormalizePathSeparators(filename);
        
        if (!IsValidFilename(normalizedFilename)) {
            std::cerr << "CreatePak: Invalid filename: " << filename << std::endl;
            return false;
        }
        
        std::vector<uint8_t> encryptedData = data;
        EncryptDecrypt(encryptedData);

        uint64_t currentOffset = SafeStreamPos(pakStream.tellp());
        entries.emplace_back(normalizedFilename, currentOffset, static_cast<uint64_t>(encryptedData.size()));

        pakStream.write(reinterpret_cast<const char*>(encryptedData.data()), 
                       static_cast<std::streamsize>(encryptedData.size()));
        if (!pakStream) {
            std::cerr << "CreatePak: Failed to write data for file: " << filename << std::endl;
            return false;
        }
    }

    // Record the file table offset
    header.fileTableOffset = SafeStreamPos(pakStream.tellp());
    std::cout << "CreatePak: File table will be written at offset " << header.fileTableOffset << std::endl;

    // Write the file table
    if (!WriteFileTable(pakStream, entries))
    {
        std::cerr << "CreatePak: Failed to write file table." << std::endl;
        return false;
    }

    // Update the header with the correct fileTableOffset
    pakStream.seekp(0, std::ios::beg);
    if (!pakStream)
    {
        std::cerr << "CreatePak: Failed to seek to header." << std::endl;
        return false;
    }

    if (!WritePakHeader(pakStream, header))
    {
        std::cerr << "CreatePak: Failed to update pak header with fileTableOffset." << std::endl;
        return false;
    }

    pakStream.close();
    std::cout << "CreatePak: PAK file '" << pakFilename << "' created successfully." << std::endl;
    return true;
}

// Extracts all files from a PAK to the specified output directory
bool Pakker::ExtractPak(const std::string& pakFilename, const std::string& outputDir) const
{
    std::ifstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream)
    {
        std::cerr << "ExtractPak: Unable to open pak file: " << pakFilename << std::endl;
        return false;
    }

    PakHeader header;
    if (!ReadPakHeader(pakStream, header))
    {
        std::cerr << "ExtractPak: Failed to read pak header." << std::endl;
        return false;
    }

    // Get file size for validation
    pakStream.seekg(0, std::ios::end);
    uint64_t pakFileSize = SafeStreamPos(pakStream.tellg());

    // Seek to fileTableOffset
    pakStream.seekg(header.fileTableOffset, std::ios::beg);
    if (!pakStream)
    {
        std::cerr << "ExtractPak: Failed to seek to fileTableOffset." << std::endl;
        return false;
    }

    // Read file entries
    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header.numFiles, entries))
    {
        std::cerr << "ExtractPak: Failed to read file table." << std::endl;
        return false;
    }

    // Extract each file
    for (const auto& entry : entries) {
        // Validate entry
        if (!ValidateEntry(entry, pakFileSize)) {
            std::cerr << "ExtractPak: Invalid entry detected: " << entry.filename << std::endl;
            return false;
        }
        
        pakStream.seekg(entry.offset, std::ios::beg);
        if (!pakStream) {
            std::cerr << "ExtractPak: Failed to seek to offset for file: " << entry.filename << std::endl;
            return false;
        }

        std::vector<uint8_t> fileData(entry.size);
        pakStream.read(reinterpret_cast<char*>(fileData.data()), 
                      static_cast<std::streamsize>(entry.size));
        if (!pakStream) {
            std::cerr << "ExtractPak: Failed to read data for file: " << entry.filename << std::endl;
            return false;
        }

        EncryptDecrypt(fileData); // Decrypt the file data

        std::string outputPath = (fs::path(outputDir) / entry.filename).string();

        // **Sanitize the output path to prevent directory traversal attacks**
        fs::path sanitizedOutputPath = fs::weakly_canonical(fs::path(outputPath));
        fs::path sanitizedOutputDir = fs::weakly_canonical(fs::path(outputDir));

        // Ensure the output path is within the output directory
        if (sanitizedOutputPath.string().find(sanitizedOutputDir.string()) != 0)
        {
            std::cerr << "ExtractPak: Detected invalid file path: " << entry.filename << std::endl;
            return false;
        }

        // Create necessary directories
        fs::create_directories(fs::path(outputPath).parent_path());
        if (!WriteFile(outputPath, fileData))
        {
            std::cerr << "ExtractPak: Failed to write file: " << outputPath << std::endl;
            return false;
        }
    }

    pakStream.close();
    std::cout << "ExtractPak: All files extracted successfully to '" << outputDir << "'." << std::endl;
    return true;
}

// Lists all files contained within a PAK
bool Pakker::ListPak(const std::string& pakFilename) const
{
    std::ifstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream)
    {
        std::cerr << "ListPak: Unable to open pak file: " << pakFilename << std::endl;
        return false;
    }

    PakHeader header;
    if (!ReadPakHeader(pakStream, header))
    {
        std::cerr << "ListPak: Failed to read pak header." << std::endl;
        return false;
    }

    // Seek to fileTableOffset
    pakStream.seekg(header.fileTableOffset, std::ios::beg);
    if (!pakStream)
    {
        std::cerr << "ListPak: Failed to seek to fileTableOffset." << std::endl;
        return false;
    }

    // Read file entries
    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header.numFiles, entries))
    {
        std::cerr << "ListPak: Failed to read file table." << std::endl;
        return false;
    }

    // Display file information
    std::cout << "ListPak: Contents of '" << pakFilename << "':" << std::endl;
    for (const auto& entry : entries) {
        std::cout << " - " << entry.filename << " (Offset: " << entry.offset 
                  << ", Size: " << entry.size << " bytes)" << std::endl;
    }

    pakStream.close();
    return true;
}

// Reads a specific file from a PAK and returns its data
std::vector<uint8_t> Pakker::ReadFileFromPak(const std::string& pakFilename, const std::string& filename) const
{
    std::ifstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream)
    {
        std::cerr << "ReadFileFromPak: Unable to open pak file: " << pakFilename << std::endl;
        return {};
    }

    PakHeader header;
    if (!ReadPakHeader(pakStream, header))
    {
        std::cerr << "ReadFileFromPak: Failed to read pak header." << std::endl;
        return {};
    }

    // Get file size for validation
    pakStream.seekg(0, std::ios::end);
    uint64_t pakFileSize = SafeStreamPos(pakStream.tellg());

    // Seek to fileTableOffset
    pakStream.seekg(header.fileTableOffset, std::ios::beg);
    if (!pakStream)
    {
        std::cerr << "ReadFileFromPak: Failed to seek to fileTableOffset." << std::endl;
        return {};
    }

    // Read file entries
    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header.numFiles, entries))
    {
        std::cerr << "ReadFileFromPak: Failed to read file table." << std::endl;
        return {};
    }

    // Search for the file entry
    std::string normalizedFilename = NormalizePathSeparators(filename);
    for (const auto& entry : entries) {
        if (entry.filename == normalizedFilename) {
            // Validate entry
            if (!ValidateEntry(entry, pakFileSize)) {
                std::cerr << "ReadFileFromPak: Invalid entry: " << entry.filename << std::endl;
                return {};
            }
            
            pakStream.seekg(entry.offset, std::ios::beg);
            if (!pakStream) {
                std::cerr << "ReadFileFromPak: Failed to seek to offset for file: " << entry.filename << std::endl;
                return {};
            }

            std::vector<uint8_t> fileData(entry.size);
            pakStream.read(reinterpret_cast<char*>(fileData.data()), 
                          static_cast<std::streamsize>(entry.size));
            if (!pakStream) {
                std::cerr << "ReadFileFromPak: Failed to read data for file: " << entry.filename << std::endl;
                return {};
            }

            EncryptDecrypt(fileData); // Decrypt the file data
            return fileData;
        }
    }

    std::cerr << "ReadFileFromPak: File not found in pak: " << filename << std::endl;
    return {};
}

// Loads a specific file from a PAK and returns a shared pointer to its data
std::shared_ptr<std::vector<uint8_t>> Pakker::LoadFile(const std::string& pakFilename, const std::string& filename) const
{
    std::vector<uint8_t> fileData = ReadFileFromPak(pakFilename, filename);
    if (fileData.empty())
    {
        return nullptr;
    }
    return std::make_shared<std::vector<uint8_t>>(std::move(fileData));
}

// Adds a single file to an existing PAK
bool Pakker::AddFileToPak(const std::string& pakFilename, const std::string& filename, const std::vector<uint8_t>& data)
{
    // Validate filename
    std::string normalizedFilename = NormalizePathSeparators(filename);
    if (!IsValidFilename(normalizedFilename)) {
        std::cerr << "AddFileToPak: Invalid filename: " << filename << std::endl;
        return false;
    }
    
    // Open the PAK file with both input and output capabilities
    std::fstream pakStream(pakFilename, std::ios::in | std::ios::out | std::ios::binary);
    if (!pakStream)
    {
        std::cerr << "AddFileToPak: Unable to open pak file: " << pakFilename << std::endl;
        return false;
    }

    // Read the existing PAK header
    PakHeader header;
    if (!ReadPakHeader(pakStream, header))
    {
        std::cerr << "AddFileToPak: Failed to read pak header." << std::endl;
        return false;
    }

    std::cout << "AddFileToPak: Current file table offset is " << header.fileTableOffset << std::endl;

    // Read existing file table
    pakStream.seekg(header.fileTableOffset, std::ios::beg);
    if (!pakStream)
    {
        std::cerr << "AddFileToPak: Failed to seek to fileTableOffset." << std::endl;
        return false;
    }

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header.numFiles, entries))
    {
        std::cerr << "AddFileToPak: Failed to read file table." << std::endl;
        return false;
    }

    // Check if the file already exists in the PAK
    auto it = std::find_if(entries.begin(), entries.end(), [&](const PakEntry& entry) {
        return entry.filename == normalizedFilename;
    });

    if (it != entries.end())
    {
        std::cerr << "AddFileToPak: File already exists in pak: " << filename << std::endl;
        return false;
    }

    // Check if we would exceed the file limit
    if (header.numFiles + 1 > MAX_FILES_IN_PAK) {
        std::cerr << "AddFileToPak: Would exceed maximum file count." << std::endl;
        return false;
    }

    // Move to the end of the file to append the new file data
    pakStream.seekp(0, std::ios::end);

    if (!pakStream) {
        std::cerr << "AddFileToPak: Failed to seek to end of pak file." << std::endl;
        return false;
    }

    uint64_t newOffset = SafeStreamPos(pakStream.tellp());
    std::cout << "AddFileToPak: Appending new file at offset " << newOffset << std::endl;

    std::vector<uint8_t> encryptedData = data;
    EncryptDecrypt(encryptedData);
    pakStream.write(reinterpret_cast<const char*>(encryptedData.data()), 
                   static_cast<std::streamsize>(encryptedData.size()));
    if (!pakStream) {
        std::cerr << "AddFileToPak: Failed to write data for file: " << filename << std::endl;
        return false;
    }

    // Update entries and header
    entries.emplace_back(normalizedFilename, newOffset, static_cast<uint64_t>(encryptedData.size()));
    header.numFiles += 1;

    // Update the file table offset to current end
    header.fileTableOffset = SafeStreamPos(pakStream.tellp());
    std::cout << "AddFileToPak: New file table offset is " << header.fileTableOffset << std::endl;

    // Seek to the beginning to write the updated header
    pakStream.seekp(0, std::ios::beg);
    if (!pakStream)
    {
        std::cerr << "AddFileToPak: Failed to seek to beginning of pak file." << std::endl;
        return false;
    }

    if (!WritePakHeader(pakStream, header))
    {
        std::cerr << "AddFileToPak: Failed to update pak header with new fileTableOffset." << std::endl;
        return false;
    }

    // Seek to the new fileTableOffset to write the updated file table
    pakStream.seekp(header.fileTableOffset, std::ios::beg);
    if (!pakStream)
    {
        std::cerr << "AddFileToPak: Failed to seek to new fileTableOffset." << std::endl;
        return false;
    }

    if (!WriteFileTable(pakStream, entries))
    {
        std::cerr << "AddFileToPak: Failed to write updated file table." << std::endl;
        return false;
    }

    pakStream.close();
    std::cout << "AddFileToPak: File '" << filename << "' added successfully." << std::endl;
    return true;
}

// Creates a PAK file from all files within a specified folder
bool Pakker::CreatePakFromFolder(const std::string& pakFilename, const std::string& folderPath)
{
    std::map<std::string, std::vector<uint8_t>> files;

    try
    {
        for (const auto& entry : fs::recursive_directory_iterator(folderPath))
        {
            if (fs::is_regular_file(entry.path()))
            {
                std::string relativePath = fs::relative(entry.path(), folderPath).string();
                relativePath = NormalizePathSeparators(relativePath);
                
                if (!IsValidFilename(relativePath)) {
                    std::cerr << "CreatePakFromFolder: Skipping invalid filename: " << relativePath << std::endl;
                    continue;
                }
                
                std::ifstream file(entry.path(), std::ios::binary);
                if (!file)
                {
                    std::cerr << "CreatePakFromFolder: Failed to open file: " << entry.path() << std::endl;
                    continue;
                }
                
                std::vector<uint8_t> content((std::istreambuf_iterator<char>(file)), 
                                           std::istreambuf_iterator<char>());
                files[relativePath] = std::move(content);
            }
        }
    }
    catch (const fs::filesystem_error& e)
    {
        std::cerr << "CreatePakFromFolder: Filesystem error: " << e.what() << std::endl;
        return false;
    }

    return CreatePak(pakFilename, files);
}

// Get file count without reading entire file table
uint32_t Pakker::GetFileCount(const std::string& pakFilename) const
{
    std::ifstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        return 0;
    }

    PakHeader header;
    if (!ReadPakHeader(pakStream, header)) {
        return 0;
    }

    return header.numFiles;
}

// Check if a file exists in PAK without reading it
bool Pakker::FileExists(const std::string& pakFilename, const std::string& filename) const
{
    return GetFileInfo(pakFilename, filename).found;
}

// Get file info without reading the actual file data
Pakker::FileInfo Pakker::GetFileInfo(const std::string& pakFilename, const std::string& filename) const
{
    FileInfo info{};
    info.found = false;

    std::ifstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        return info;  
    }

    PakHeader header;
    if (!ReadPakHeader(pakStream, header)) {
        return info;
    }

    pakStream.seekg(header.fileTableOffset, std::ios::beg);
    if (!pakStream) {
        return info;
    }

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header.numFiles, entries)) {
        return info;
    }

    std::string normalizedFilename = NormalizePathSeparators(filename);
    auto it = std::find_if(entries.begin(), entries.end(), 
                          [&](const PakEntry& entry) {
                              return entry.filename == normalizedFilename;
                          });

    if (it != entries.end()) {
        info.filename = it->filename;
        info.size = it->size;
        info.found = true;
    }

    return info;
}

// Extract single file to disk
bool Pakker::ExtractSingleFile(const std::string& pakFilename, const std::string& filename, const std::string& outputPath) const
{
    std::vector<uint8_t> fileData = ReadFileFromPak(pakFilename, filename);
    if (fileData.empty()) {
        return false;
    }

    // Create necessary directories
    fs::create_directories(fs::path(outputPath).parent_path());
    
    return WriteFile(outputPath, fileData);
}

// Validate PAK file integrity
bool Pakker::ValidatePak(const std::string& pakFilename) const
{
    std::ifstream pakStream(pakFilename, std::ios::binary);
    if (!pakStream) {
        std::cerr << "ValidatePak: Unable to open pak file: " << pakFilename << std::endl;
        return false;
    }

    // Get file size
    pakStream.seekg(0, std::ios::end);
    uint64_t pakFileSize = SafeStreamPos(pakStream.tellg());
    pakStream.seekg(0, std::ios::beg);

    PakHeader header;
    if (!ReadPakHeader(pakStream, header)) {
        return false;
    }

    // Validate file table offset
    if (header.fileTableOffset >= pakFileSize) {
        std::cerr << "ValidatePak: Invalid file table offset." << std::endl;
        return false;
    }

    pakStream.seekg(header.fileTableOffset, std::ios::beg);
    if (!pakStream) {
        std::cerr << "ValidatePak: Failed to seek to file table." << std::endl;
        return false;
    }

    std::vector<PakEntry> entries;
    if (!ReadFileTable(pakStream, header.numFiles, entries)) {
        return false;
    }

    // Validate each entry
    for (const auto& entry : entries) {
        if (!ValidateEntry(entry, pakFileSize)) {
            std::cerr << "ValidatePak: Invalid entry: " << entry.filename << std::endl;
            return false;
        }
    }

    std::cout << "ValidatePak: PAK file '" << pakFilename << "' is valid." << std::endl;
    return true;
}