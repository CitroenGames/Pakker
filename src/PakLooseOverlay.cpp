#include "PakLooseOverlay.h"
#include "PakInternal.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

using namespace PakInternal;

PakLooseOverlay::PakLooseOverlay(std::shared_ptr<PakReader> reader, std::string looseDirectory)
    : reader_(std::move(reader)), looseDirectory_(std::move(looseDirectory))
{
}

std::string PakLooseOverlay::ResolveLooseFilePath(std::string_view filename) const
{
    std::string normalized = NormalizePathSeparators(std::string(filename));
    if (normalized.empty()) return {};

    fs::path candidate = fs::path(looseDirectory_) / normalized;

    std::error_code ec;
    fs::path sanitizedCandidate = fs::weakly_canonical(candidate, ec);
    if (ec) return {};
    fs::path sanitizedDir = fs::weakly_canonical(fs::path(looseDirectory_), ec);
    if (ec) return {};

    fs::path rel = fs::relative(sanitizedCandidate, sanitizedDir, ec);
    if (ec) return {};
    std::string relStr = rel.string();
    if (relStr.empty() || relStr.starts_with("..")) {
        Log(PakLogLevel::Error, "PakLooseOverlay: Detected path traversal: " + std::string(filename));
        return {};
    }

    if (!fs::is_regular_file(sanitizedCandidate, ec) || ec) return {};
    return sanitizedCandidate.string();
}

bool PakLooseOverlay::FileExists(std::string_view filename) const
{
    if (!ResolveLooseFilePath(filename).empty()) return true;
    return reader_ && reader_->FileExists(filename);
}

PakStatus PakLooseOverlay::Read(std::string_view filename, std::span<uint8_t> destination,
                                uint64_t* bytesWritten) const
{
    if (bytesWritten) *bytesWritten = 0;

    std::string loosePath = ResolveLooseFilePath(filename);
    if (loosePath.empty()) {
        if (!reader_) return PakStatus::NotFound;
        PakFileHandle handle = reader_->Find(filename);
        if (!handle) return PakStatus::NotFound;
        return reader_->Read(handle, destination, bytesWritten);
    }

    std::ifstream file(loosePath, std::ios::binary | std::ios::ate);
    if (!file) {
        Log(PakLogLevel::Error, "PakLooseOverlay::Read: Unable to open loose file: " + loosePath);
        return PakStatus::IoError;
    }
    auto size = file.tellg();
    if (size == std::streampos(-1)) return PakStatus::IoError;
    uint64_t fileSize = static_cast<uint64_t>(size);
    if (fileSize > destination.size()) return PakStatus::BufferTooSmall;

    file.seekg(0, std::ios::beg);
    if (fileSize > 0) {
        file.read(reinterpret_cast<char*>(destination.data()), static_cast<std::streamsize>(fileSize));
        if (!file) return PakStatus::IoError;
    }

    if (bytesWritten) *bytesWritten = fileSize;
    return PakStatus::Ok;
}

PakStatus PakLooseOverlay::Load(std::string_view filename, std::vector<uint8_t>& outData) const
{
    outData.clear();

    std::string loosePath = ResolveLooseFilePath(filename);
    if (loosePath.empty()) {
        if (!reader_) return PakStatus::NotFound;
        PakFileHandle handle = reader_->Find(filename);
        if (!handle) return PakStatus::NotFound;
        return reader_->Load(handle, outData);
    }

    std::ifstream file(loosePath, std::ios::binary | std::ios::ate);
    if (!file) {
        Log(PakLogLevel::Error, "PakLooseOverlay::Load: Unable to open loose file: " + loosePath);
        return PakStatus::IoError;
    }
    auto size = file.tellg();
    if (size == std::streampos(-1)) return PakStatus::IoError;
    uint64_t fileSize = static_cast<uint64_t>(size);

    outData.resize(static_cast<size_t>(fileSize));
    file.seekg(0, std::ios::beg);
    if (fileSize > 0) {
        file.read(reinterpret_cast<char*>(outData.data()), static_cast<std::streamsize>(fileSize));
        if (!file) {
            outData.clear();
            return PakStatus::IoError;
        }
    }

    return PakStatus::Ok;
}
