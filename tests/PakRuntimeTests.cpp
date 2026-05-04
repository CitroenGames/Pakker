#include "Pak.h"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

static std::vector<uint8_t> Bytes(std::string_view text)
{
    return {text.begin(), text.end()};
}

static fs::path TestRoot()
{
    fs::path root = fs::temp_directory_path() / "pakker_runtime_tests";
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

static void RuntimeHandleApi()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "runtime.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["textures/diffuse.dds"] = Bytes("gpu-ready-texture");
    files["scripts/config.txt"] = Bytes("name=value");
    files["empty.bin"] = {};
    files["compressed/raw.bin"] = std::vector<uint8_t>(4096, 7);

    PakOptions options;
    options.compress = true;
    options.alignment = 4096;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    {
        std::ifstream stream(pakPath, std::ios::binary);
        PakInternal::PakHeader header;
        assert(PakInternal::ReadPakHeader(stream, header));
        assert(header.version == PakInternal::PAK_VERSION_4);
    }

    PakReader reader;
    assert(reader.Open(pakPath.string()));
    assert(reader.GetFileCount() == files.size());

    PakFileHandle texture = reader.Find("textures/diffuse.dds");
    assert(texture);
    const PakFileInfo* textureInfo = reader.Info(texture);
    assert(textureInfo);
    assert(textureInfo->filename == "textures/diffuse.dds");
    assert(!textureInfo->compressed);

    PakView textureView;
    if (reader.IsMapped()) {
        assert(reader.View(texture, textureView) == PakStatus::Ok);
        assert(textureView.mapped);
        assert(textureView.size == files["textures/diffuse.dds"].size());
        assert(std::string_view(reinterpret_cast<const char*>(textureView.data),
                                static_cast<size_t>(textureView.size)) == "gpu-ready-texture");

        reader.Close();
        assert(textureView.data[0] == 'g');
    } else {
        assert(reader.View(texture, textureView) == PakStatus::Unsupported);
        reader.Close();
    }

    assert(reader.Open(pakPath.string()));
    PakFileHandle compressed = reader.Find("compressed/raw.bin");
    assert(compressed);
    const PakFileInfo* compressedInfo = reader.Info(compressed);
    assert(compressedInfo && compressedInfo->compressed);

    PakView compressedView;
    assert(reader.View(compressed, compressedView) == PakStatus::Unsupported);

    std::vector<uint8_t> tooSmall(8);
    uint64_t written = 123;
    assert(reader.Read(compressed, tooSmall, &written) == PakStatus::BufferTooSmall);
    assert(written == 0);

    std::vector<uint8_t> loaded;
    assert(reader.Load(compressed, loaded) == PakStatus::Ok);
    assert(loaded == files["compressed/raw.bin"]);

    PakFileHandle slashPath = reader.Find("\\scripts\\config.txt");
    assert(slashPath);
    std::vector<uint8_t> config;
    assert(reader.Load(slashPath, config) == PakStatus::Ok);
    assert(config == files["scripts/config.txt"]);

    PakFileHandle empty = reader.Find("empty.bin");
    assert(empty);
    PakView emptyView;
    if (reader.IsMapped()) {
        assert(reader.View(empty, emptyView) == PakStatus::Ok);
        assert(emptyView.size == 0);
    } else {
        assert(reader.View(empty, emptyView) == PakStatus::Unsupported);
    }
    std::vector<uint8_t> emptyData = {1, 2, 3};
    assert(reader.Load(empty, emptyData) == PakStatus::Ok);
    assert(emptyData.empty());

    std::string_view names[] = {
        "textures/diffuse.dds",
        "missing.asset",
        "compressed/raw.bin",
    };
    PakFileHandle handles[3];
    assert(reader.Resolve(names, handles) == 2);
    assert(handles[0]);
    assert(!handles[1]);
    assert(handles[2]);

    assert(!reader.Find("missing.asset"));
    assert(reader.Read(PakFileHandle{}, tooSmall) == PakStatus::InvalidHandle);
}

static void ExtensionDoesNotBlockCompression()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "extension_compression.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["textures/fake.png"] = std::vector<uint8_t>(4096, 3);

    PakOptions options;
    options.compress = true;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    assert(reader.Open(pakPath.string()));

    PakFileHandle handle = reader.Find("textures/fake.png");
    assert(handle);
    const PakFileInfo* info = reader.Info(handle);
    assert(info && info->compressed);
    assert(info->compressedSize < info->originalSize);

    std::vector<uint8_t> loaded;
    assert(reader.Load(handle, loaded) == PakStatus::Ok);
    assert(loaded == files["textures/fake.png"]);
}

static void OldVersionRejected()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "old_version.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["file.bin"] = Bytes("payload");

    Pakker pakker;
    PakOptions options;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    std::fstream stream(pakPath, std::ios::in | std::ios::out | std::ios::binary);
    assert(stream);
    stream.seekp(4, std::ios::beg);
    uint32_t oldVersion = 3;
    stream.write(reinterpret_cast<const char*>(&oldVersion), sizeof(oldVersion));
    stream.close();

    PakReader reader;
    assert(!reader.Open(pakPath.string()));
    assert(!pakker.ValidatePak(pakPath.string()));
}

static void CorruptArchiveFailsOpen()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "corrupt.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["file.bin"] = Bytes("payload");

    Pakker pakker;
    PakOptions options;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    std::fstream stream(pakPath, std::ios::in | std::ios::out | std::ios::binary);
    assert(stream);

    PakInternal::PakHeader header;
    assert(PakInternal::ReadPakHeader(stream, header));
    stream.seekp(header.fileTableOffset, std::ios::beg);

    uint16_t nameLength = 0;
    stream.read(reinterpret_cast<char*>(&nameLength), sizeof(nameLength));
    stream.seekp(nameLength, std::ios::cur);

    uint64_t badOffset = UINT64_MAX - 8;
    stream.write(reinterpret_cast<const char*>(&badOffset), sizeof(badOffset));
    stream.close();

    PakReader reader;
    assert(!reader.Open(pakPath.string()));
}

static void EmptyArchiveOpens()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "empty_archive.pak";

    Pakker pakker;
    std::map<std::string, std::vector<uint8_t>> files;
    PakOptions options;
    assert(pakker.CreatePak(pakPath.string(), files, options));
    assert(pakker.ValidatePak(pakPath.string()));

    PakReader reader;
    assert(reader.Open(pakPath.string()));
    assert(reader.GetFileCount() == 0);
    assert(!reader.Find("anything.bin"));
}

int main()
{
    RuntimeHandleApi();
    ExtensionDoesNotBlockCompression();
    OldVersionRejected();
    CorruptArchiveFailsOpen();
    EmptyArchiveOpens();
    return 0;
}
