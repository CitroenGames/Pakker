#ifndef PAK_H
#define PAK_H

#include <string>
#include <vector>
#include <map>
#include <memory>

#define ENCRYPTION_KEY "example_key"

struct PakEntry {
    std::string filename;
    uint32_t offset;
    uint32_t size;
};

class Pakker {
public:
    bool CreatePak(const std::string& pakFilename, const std::map<std::string, std::vector<uint8_t>>& files);
    bool ExtractPak(const std::string& pakFilename, const std::string& outputDir);
    bool ListPak(const std::string& pakFilename);
    std::vector<uint8_t> ReadFileFromPak(const std::string& pakFilename, const std::string& filename);

    std::shared_ptr<std::vector<uint8_t>> LoadFile(const std::string& pakFilename, const std::string& filename);

    bool AddFileToPak(const std::string& pakFilename, const std::string& filename, const std::vector<uint8_t>& data);

    bool CreatePakFromFolder(const std::string& pakFilename, const std::string& folderPath);

private:
    bool WriteFile(const std::string& filename, const std::vector<uint8_t>& buffer);
    void EncryptDecrypt(std::vector<uint8_t>& data);
    std::vector<PakEntry> entries;
};

#endif // PAK_H