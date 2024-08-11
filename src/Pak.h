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
    bool createPak(const std::string& pakFilename, const std::map<std::string, std::vector<uint8_t>>& files);
    bool extractPak(const std::string& pakFilename, const std::string& outputDir);
    bool listPak(const std::string& pakFilename);
    std::vector<uint8_t> readFileFromPak(const std::string& pakFilename, const std::string& filename);

    std::shared_ptr<std::vector<uint8_t>> loadFile(const std::string& pakFilename, const std::string& filename);

    bool addFileToPak(const std::string& pakFilename, const std::string& filename, const std::vector<uint8_t>& data);

    bool createPakFromFolder(const std::string& pakFilename, const std::string& folderPath);

private:
    bool writeFile(const std::string& filename, const std::vector<uint8_t>& buffer);
    void encryptDecrypt(std::vector<uint8_t>& data);
    std::vector<PakEntry> entries;
};

#endif // PAK_H