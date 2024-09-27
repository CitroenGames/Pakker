#ifndef PAK_H
#define PAK_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>

class Pakker {
public:
    // Constructor with optional encryption key
    explicit Pakker(const std::string& encryptionKey = "example_key");

    // Creates a PAK file from a map of filenames to their data
    bool CreatePak(const std::string& pakFilename, const std::map<std::string, std::vector<uint8_t>>& files);

    // Extracts all files from a PAK to the specified output directory
    bool ExtractPak(const std::string& pakFilename, const std::string& outputDir) const;

    // Lists all files contained within a PAK
    bool ListPak(const std::string& pakFilename) const;

    // Reads a specific file from a PAK and returns its data
    std::vector<uint8_t> ReadFileFromPak(const std::string& pakFilename, const std::string& filename) const;

    // Loads a specific file from a PAK and returns a shared pointer to its data
    std::shared_ptr<std::vector<uint8_t>> LoadFile(const std::string& pakFilename, const std::string& filename) const;

    // Adds a single file to an existing PAK
    bool AddFileToPak(const std::string& pakFilename, const std::string& filename, const std::vector<uint8_t>& data);

    // Creates a PAK file from all files within a specified folder
    bool CreatePakFromFolder(const std::string& pakFilename, const std::string& folderPath);

private:
    struct PakEntry {
        std::string filename;
        uint64_t offset;
        uint64_t size;
    };

    struct PakHeader {
        char magic[4] = { 'P', 'A', 'K', '0' }; // Magic number to identify PAK files
        uint32_t version = 1;                   // Version number for potential future use
        uint32_t numFiles;                      // Number of files contained in the PAK
        uint64_t fileTableOffset;               // Offset in the file where the file table starts
    };

    // Writes raw data to a file
    bool WriteFile(const std::string& filename, const std::vector<uint8_t>& buffer) const;

    // Reads the PAK header from a stream
    bool ReadPakHeader(std::istream& stream, PakHeader& header) const;

    // Writes the PAK header to a stream
    bool WritePakHeader(std::ostream& stream, const PakHeader& header) const;

    // Reads the file table from the given stream at the current position
    bool ReadFileTable(std::istream& stream, uint32_t numFiles, std::vector<PakEntry>& entries) const;

    // Writes the file table to the given stream at the current position
    bool WriteFileTable(std::ostream& stream, const std::vector<PakEntry>& entries) const;

    // Encrypts or decrypts data using XOR with the encryption key
    void EncryptDecrypt(std::vector<uint8_t>& data) const;

    // Normalizes path separators to '/'
    std::string NormalizePathSeparators(const std::string& path) const;

    std::string encryptionKey_; // Encryption key used for encrypting/decrypting data
};

#endif // PAK_H