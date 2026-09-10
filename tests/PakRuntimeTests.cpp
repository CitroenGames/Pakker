#include "Pak.h"
#include "PakInternal.h"
#include "PakLooseOverlay.h"
#include "PakPlatform.h"
#include "PakCompression.h"

// This suite uses assert() to both exercise (call) and verify library
// behavior in the same expression, e.g. assert(reader.Open(path)). NDEBUG
// (defined by CMAKE_BUILD_TYPE=Release) makes assert() expand to nothing,
// which would silently skip those calls entirely rather than just skipping
// the check -- so force assert() to stay active in this translation unit
// regardless of the overall build configuration.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <string_view>
#include <thread>
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

static PakCacheOptions TestCacheOptions(const fs::path& root)
{
    PakCacheOptions options;
    options.persistentCacheDirectory = (root / "cache").string();
    return options;
}

// Reads back the sole entry of a single-file archive's file table.
static PakInternal::PakEntry ReadSingleEntry(const fs::path& pakPath)
{
    std::ifstream stream(pakPath, std::ios::binary);
    assert(stream);
    PakInternal::PakHeader header;
    assert(PakInternal::ReadPakHeader(stream, header));
    assert(header.numFiles == 1);
    std::vector<PakInternal::PakEntry> entries;
    assert(PakInternal::ReadFileTable(stream, header, entries));
    assert(entries.size() == 1);
    return entries[0];
}

// Reads back a named entry from a multi-entry archive's file table.
static PakInternal::PakEntry ReadEntryByName(const fs::path& pakPath, const std::string& name)
{
    std::ifstream stream(pakPath, std::ios::binary);
    assert(stream);
    PakInternal::PakHeader header;
    assert(PakInternal::ReadPakHeader(stream, header));
    std::vector<PakInternal::PakEntry> entries;
    assert(PakInternal::ReadFileTable(stream, header, entries));
    for (auto& e : entries) {
        if (e.filename == name) return e;
    }
    assert(false && "entry not found");
    return {};
}

// Flips one bit of the first on-disk byte of an entry, simulating bit-rot /
// a bad patch without touching the file table (so structural bounds checks
// still pass -- only content-hash verification should catch this).
static void FlipByteAtFileOffset(const fs::path& pakPath, uint64_t offset)
{
    std::fstream stream(pakPath, std::ios::in | std::ios::out | std::ios::binary);
    assert(stream);
    stream.seekg(offset, std::ios::beg);
    char byte = 0;
    stream.read(&byte, 1);
    assert(stream);
    byte = static_cast<char>(byte ^ 0xFF);
    stream.seekp(offset, std::ios::beg);
    stream.write(&byte, 1);
    assert(stream);
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
    options.compression = PakCompression::LZ4;
    options.alignment = 4096;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    {
        std::ifstream stream(pakPath, std::ios::binary);
        PakInternal::PakHeader header;
        assert(PakInternal::ReadPakHeader(stream, header));
        assert(header.version == PakInternal::PAK_VERSION_8);
    }

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
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
    options.compression = PakCompression::LZ4;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
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

    // v4 (no contentHash field), v5 (no Zstd flag bit), v6 (variable-length
    // file table) and v7 (no chunked-entry flag) are all prior formats --
    // confirm each is cleanly rejected now that v8 is the only accepted
    // version, with the same "rebuild from source" remediation for any.
    for (uint32_t oldVersion : {PakInternal::PAK_VERSION_4, PakInternal::PAK_VERSION_5,
                                PakInternal::PAK_VERSION_6, PakInternal::PAK_VERSION_7}) {
        std::fstream stream(pakPath, std::ios::in | std::ios::out | std::ios::binary);
        assert(stream);
        stream.seekp(4, std::ios::beg);
        stream.write(reinterpret_cast<const char*>(&oldVersion), sizeof(oldVersion));
        stream.close();

        PakReader reader;
        reader.SetCacheOptions(TestCacheOptions(root));
        assert(!reader.Open(pakPath.string()));
        assert(!pakker.ValidatePak(pakPath.string()));
    }
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

    // v7 records are fixed-size and lead with the data offset, so the first
    // eight bytes of the file table are the first entry's offset.
    stream.seekp(static_cast<std::streamoff>(header.fileTableOffset), std::ios::beg);
    uint64_t badOffset = UINT64_MAX - 8;
    stream.write(reinterpret_cast<const char*>(&badOffset), sizeof(badOffset));
    stream.close();

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
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
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));
    assert(reader.GetFileCount() == 0);
    assert(!reader.Find("anything.bin"));
}

static void MemoryCacheStoresDecodedEntries()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "memory_cache.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["compressed/repeated.bin"] = std::vector<uint8_t>(8192, 9);

    PakOptions pakOptions;
    pakOptions.compression = PakCompression::LZ4;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, pakOptions));

    PakCacheOptions cacheOptions = TestCacheOptions(root);
    cacheOptions.persistentCacheEnabled = false;
    cacheOptions.memoryBudgetBytes = 1024 * 1024;

    PakReader reader;
    reader.SetCacheOptions(cacheOptions);
    assert(reader.Open(pakPath.string()));

    PakFileHandle handle = reader.Find("compressed/repeated.bin");
    assert(handle);

    std::vector<uint8_t> loaded;
    assert(reader.Load(handle, loaded) == PakStatus::Ok);
    assert(loaded == files["compressed/repeated.bin"]);

    PakCacheStats afterFirst = reader.GetCacheStats();
    assert(afterFirst.sourceReads == 1);
    assert(afterFirst.memoryStores == 1);
    assert(afterFirst.memoryBytes == files["compressed/repeated.bin"].size());

    loaded.clear();
    assert(reader.Load(handle, loaded) == PakStatus::Ok);
    assert(loaded == files["compressed/repeated.bin"]);

    PakCacheStats afterSecond = reader.GetCacheStats();
    assert(afterSecond.memoryHits == 1);
    assert(afterSecond.sourceReads == 1);

    reader.ClearMemoryCache();
    assert(reader.GetCacheStats().memoryBytes == 0);
}

static void MemoryCacheEvictsLeastRecentlyUsedEntry()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "memory_cache_lru.pak";

    constexpr size_t entrySize = 4096;
    std::map<std::string, std::vector<uint8_t>> files;
    files["a.bin"] = std::vector<uint8_t>(entrySize, 1);
    files["b.bin"] = std::vector<uint8_t>(entrySize, 2);
    files["c.bin"] = std::vector<uint8_t>(entrySize, 3);

    PakOptions pakOptions;
    pakOptions.compression = PakCompression::LZ4;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, pakOptions));

    PakCacheOptions cacheOptions = TestCacheOptions(root);
    cacheOptions.persistentCacheEnabled = false;
    cacheOptions.memoryBudgetBytes = entrySize * 2;
    cacheOptions.maxSingleEntryBytes = entrySize;

    PakReader reader;
    reader.SetCacheOptions(cacheOptions);
    assert(reader.Open(pakPath.string()));

    const PakFileHandle a = reader.Find("a.bin");
    const PakFileHandle b = reader.Find("b.bin");
    const PakFileHandle c = reader.Find("c.bin");
    assert(a && b && c);

    std::vector<uint8_t> loaded;
    assert(reader.Load(a, loaded) == PakStatus::Ok);
    assert(reader.Load(b, loaded) == PakStatus::Ok);
    assert(reader.Load(a, loaded) == PakStatus::Ok); // a is now most recent
    assert(reader.Load(c, loaded) == PakStatus::Ok); // evicts b
    assert(reader.Load(a, loaded) == PakStatus::Ok); // must still be cached

    PakCacheStats beforeReloadingB = reader.GetCacheStats();
    assert(beforeReloadingB.memoryHits == 2);
    assert(beforeReloadingB.sourceReads == 3);
    assert(beforeReloadingB.memoryEvictions == 1);

    assert(reader.Load(b, loaded) == PakStatus::Ok);
    PakCacheStats afterReloadingB = reader.GetCacheStats();
    assert(afterReloadingB.sourceReads == 4);
    assert(afterReloadingB.memoryEvictions == 2);
}

static void PersistentCacheSurvivesReaderReopen()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "persistent_cache.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["compressed/payload.bin"] = std::vector<uint8_t>(8192, 5);

    PakOptions pakOptions;
    pakOptions.compression = PakCompression::LZ4;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, pakOptions));

    PakCacheOptions cacheOptions = TestCacheOptions(root);
    cacheOptions.memoryCacheEnabled = false;

    {
        PakReader reader;
        reader.SetCacheOptions(cacheOptions);
        assert(reader.Open(pakPath.string()));
        PakFileHandle handle = reader.Find("compressed/payload.bin");
        assert(handle);
        std::vector<uint8_t> loaded;
        assert(reader.Load(handle, loaded) == PakStatus::Ok);
        assert(loaded == files["compressed/payload.bin"]);
        PakCacheStats stats = reader.GetCacheStats();
        assert(stats.sourceReads == 1);
        assert(stats.persistentStores == 1);
    }

    {
        PakReader reader;
        reader.SetCacheOptions(cacheOptions);
        assert(reader.Open(pakPath.string()));
        PakFileHandle handle = reader.Find("compressed/payload.bin");
        assert(handle);
        std::vector<uint8_t> loaded;
        assert(reader.Load(handle, loaded) == PakStatus::Ok);
        assert(loaded == files["compressed/payload.bin"]);
        PakCacheStats stats = reader.GetCacheStats();
        assert(stats.persistentHits == 1);
        assert(stats.sourceReads == 0);
    }
}

static void PersistentCacheInvalidatesWhenSourceChanges()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "persistent_invalidation.pak";

    PakOptions pakOptions;
    pakOptions.compression = PakCompression::LZ4;

    PakCacheOptions cacheOptions = TestCacheOptions(root);
    cacheOptions.memoryCacheEnabled = false;

    Pakker pakker;
    std::map<std::string, std::vector<uint8_t>> firstFiles;
    firstFiles["compressed/value.bin"] = std::vector<uint8_t>(8192, 1);
    assert(pakker.CreatePak(pakPath.string(), firstFiles, pakOptions));

    {
        PakReader reader;
        reader.SetCacheOptions(cacheOptions);
        assert(reader.Open(pakPath.string()));
        PakFileHandle handle = reader.Find("compressed/value.bin");
        assert(handle);
        std::vector<uint8_t> loaded;
        assert(reader.Load(handle, loaded) == PakStatus::Ok);
        assert(loaded == firstFiles["compressed/value.bin"]);
    }

    std::map<std::string, std::vector<uint8_t>> secondFiles;
    secondFiles["compressed/value.bin"] = std::vector<uint8_t>(8192, 2);
    assert(pakker.CreatePak(pakPath.string(), secondFiles, pakOptions));

    PakReader reader;
    reader.SetCacheOptions(cacheOptions);
    assert(reader.Open(pakPath.string()));
    PakFileHandle handle = reader.Find("compressed/value.bin");
    assert(handle);
    std::vector<uint8_t> loaded;
    assert(reader.Load(handle, loaded) == PakStatus::Ok);
    assert(loaded == secondFiles["compressed/value.bin"]);

    PakCacheStats stats = reader.GetCacheStats();
    assert(stats.persistentHits == 0);
    assert(stats.sourceReads == 1);
}

static void ZeroCopyFallbackSpanKeepsCachedDataAlive()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "span_cache_lifetime.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["compressed/span.bin"] = std::vector<uint8_t>(4096, 11);

    PakOptions pakOptions;
    pakOptions.compression = PakCompression::LZ4;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, pakOptions));

    PakSpan span;
    {
        PakReader reader;
        reader.SetCacheOptions(TestCacheOptions(root));
        assert(reader.Open(pakPath.string()));
        span = reader.ReadFileZeroCopy("compressed/span.bin");
        assert(span);
        assert(span.ownsData);
        assert(span.size == files["compressed/span.bin"].size());
        assert(span.data[0] == 11);
        reader.Close();
    }

    assert(span.data[0] == 11);
    assert(span.data[span.size - 1] == 11);
}

static void ContentHashStoredAndRoundTrips()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "content_hash.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["plain.bin"] = Bytes("hash-me-please");
    files["empty.bin"] = {};

    PakOptions options; // compress = false: on-disk bytes == original bytes
    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    std::ifstream stream(pakPath, std::ios::binary);
    PakInternal::PakHeader header;
    assert(PakInternal::ReadPakHeader(stream, header));
    std::vector<PakInternal::PakEntry> entries;
    assert(PakInternal::ReadFileTable(stream, header, entries));
    assert(entries.size() == 2);

    for (const auto& entry : entries) {
        if (entry.filename == "plain.bin") {
            const auto& expected = files["plain.bin"];
            uint64_t expectedHash = PakInternal::HashBytesFast(expected.data(), expected.size());
            assert(entry.contentHash == expectedHash);
            assert(entry.contentHash != 0);
        } else {
            assert(entry.filename == "empty.bin");
            assert(entry.contentHash == PakInternal::HashBytesFast(nullptr, 0));
        }
    }
}

static void ValidatePakDeepVerifyCatchesCorruptedCompressedEntry()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "deep_verify_compressed.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["compressed/data.bin"] = std::vector<uint8_t>(4096, 42);

    PakOptions options;
    options.compression = PakCompression::LZ4;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));
    assert(pakker.ValidatePak(pakPath.string(), /*deepVerify=*/true));

    PakInternal::PakEntry entry = ReadSingleEntry(pakPath);
    assert(PakInternal::IsCompressed(entry.flags));
    FlipByteAtFileOffset(pakPath, entry.offset);

    // Structural validation alone doesn't inspect content bytes.
    assert(pakker.ValidatePak(pakPath.string(), /*deepVerify=*/false));
    // Deep verification catches the corruption.
    assert(!pakker.ValidatePak(pakPath.string(), /*deepVerify=*/true));
}

static void ValidatePakDeepVerifyCatchesCorruptedUncompressedEntry()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "deep_verify_uncompressed.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["plain/data.bin"] = Bytes("uncompressed-payload-bytes");

    PakOptions options; // compress = false
    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));
    assert(pakker.ValidatePak(pakPath.string(), /*deepVerify=*/true));

    PakInternal::PakEntry entry = ReadSingleEntry(pakPath);
    assert(!PakInternal::IsCompressed(entry.flags));
    FlipByteAtFileOffset(pakPath, entry.offset);

    assert(pakker.ValidatePak(pakPath.string(), /*deepVerify=*/false));
    assert(!pakker.ValidatePak(pakPath.string(), /*deepVerify=*/true));
}

static void ValidatePakDeepVerifyPassesOnCleanArchive()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "deep_verify_clean.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["a/compressed.bin"] = std::vector<uint8_t>(8192, 3);
    files["b/plain.bin"] = Bytes("plain-content");
    files["c/empty.bin"] = {};

    PakOptions options;
    options.compression = PakCompression::LZ4;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));
    assert(pakker.ValidatePak(pakPath.string(), /*deepVerify=*/true));
}

static void VerifyEntryDetectsCorruption()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "verify_entry.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["asset.bin"] = std::vector<uint8_t>(2048, 77);

    PakOptions options;
    options.compression = PakCompression::LZ4;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    {
        PakReader reader;
        reader.SetCacheOptions(TestCacheOptions(root));
        assert(reader.Open(pakPath.string()));
        PakFileHandle handle = reader.Find("asset.bin");
        assert(handle);
        assert(reader.VerifyEntry(handle) == PakStatus::Ok);
    }

    PakInternal::PakEntry entry = ReadSingleEntry(pakPath);
    FlipByteAtFileOffset(pakPath, entry.offset);

    {
        PakReader reader;
        reader.SetCacheOptions(TestCacheOptions(root));
        assert(reader.Open(pakPath.string()));
        PakFileHandle handle = reader.Find("asset.bin");
        assert(handle);
        assert(reader.VerifyEntry(handle) == PakStatus::HashMismatch);
    }
}

static void VerifyEntryPropagatesInvalidHandleAndNotOpen()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "verify_entry_invalid.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["asset.bin"] = Bytes("payload");

    Pakker pakker;
    PakOptions options;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.VerifyEntry(PakFileHandle{}) == PakStatus::NotOpen);

    assert(reader.Open(pakPath.string()));
    assert(reader.VerifyEntry(PakFileHandle{}) == PakStatus::InvalidHandle);
}

static void VerifyOnReadModeFailsClosedOnMismatch()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "verify_on_read_mismatch.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["asset.bin"] = std::vector<uint8_t>(2048, 55);

    PakOptions options;
    options.compression = PakCompression::LZ4;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakInternal::PakEntry entry = ReadSingleEntry(pakPath);
    FlipByteAtFileOffset(pakPath, entry.offset);

    PakOpenOptions openOptions;
    openOptions.verifyOnRead = true;

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string(), openOptions));

    PakFileHandle handle = reader.Find("asset.bin");
    assert(handle);

    std::vector<uint8_t> destination(files["asset.bin"].size());
    uint64_t written = 999;
    assert(reader.Read(handle, destination, &written) == PakStatus::HashMismatch);
    assert(written == 0);

    std::vector<uint8_t> loaded;
    assert(reader.Load(handle, loaded) == PakStatus::HashMismatch);
}

static void VerifyOnReadBypassesDecodedCache()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "verify_on_read_bypasses_cache.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["asset.bin"] = std::vector<uint8_t>(4096, 27);

    PakOptions options;
    options.compression = PakCompression::LZ4;
    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakOpenOptions openOptions;
    openOptions.verifyOnRead = true;

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string(), openOptions));
    PakFileHandle handle = reader.Find("asset.bin");
    assert(handle);

    std::vector<uint8_t> loaded;
    assert(reader.Load(handle, loaded) == PakStatus::Ok);
    assert(reader.Load(handle, loaded) == PakStatus::Ok);

    PakCacheStats stats = reader.GetCacheStats();
    assert(stats.sourceReads == 2);
    assert(stats.memoryHits == 0);
    assert(stats.memoryStores == 0);
    assert(stats.persistentHits == 0);
    assert(stats.persistentStores == 0);
}

static void VerifyOnReadModeOffByDefaultAllowsCorruptedReadThrough()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "verify_on_read_default_off.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["asset.bin"] = Bytes("uncorrupted-original-bytes");

    PakOptions options; // compress = false: corruption maps 1:1 to output bytes
    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakInternal::PakEntry entry = ReadSingleEntry(pakPath);
    FlipByteAtFileOffset(pakPath, entry.offset);

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string())); // verifyOnRead defaults to false

    PakFileHandle handle = reader.Find("asset.bin");
    assert(handle);

    std::vector<uint8_t> loaded;
    assert(reader.Load(handle, loaded) == PakStatus::Ok);
    assert(loaded != files["asset.bin"]); // corrupted content passed through unchecked
}

static void MountTwoLayersOverrideAndFallback()
{
    fs::path root = TestRoot();
    fs::path basePak = root / "mount_base.pak";
    fs::path patchPak = root / "mount_patch.pak";

    std::map<std::string, std::vector<uint8_t>> baseFiles;
    baseFiles["shared.txt"] = Bytes("base");
    baseFiles["unique_base.txt"] = Bytes("base-only");

    std::map<std::string, std::vector<uint8_t>> patchFiles;
    patchFiles["shared.txt"] = Bytes("patch");

    PakOptions options;
    Pakker pakker;
    assert(pakker.CreatePak(basePak.string(), baseFiles, options));
    assert(pakker.CreatePak(patchPak.string(), patchFiles, options));

    PakMount mount;
    assert(mount.Mount(basePak.string()));
    assert(mount.Mount(patchPak.string()));
    assert(mount.LayerCount() == 2);

    // Higher-priority (later-mounted) layer wins on conflicts.
    PakMountHandle shared = mount.Find("shared.txt");
    assert(shared);
    std::vector<uint8_t> loaded;
    assert(mount.Load(shared, loaded) == PakStatus::Ok);
    assert(loaded == patchFiles["shared.txt"]);

    // Files unique to the lower-priority layer still resolve.
    PakMountHandle uniqueBase = mount.Find("unique_base.txt");
    assert(uniqueBase);
    loaded.clear();
    assert(mount.Load(uniqueBase, loaded) == PakStatus::Ok);
    assert(loaded == baseFiles["unique_base.txt"]);

    assert(!mount.Find("nonexistent.bin"));
    assert(!mount.FileExists("nonexistent.bin"));
}

static void MountReaderSharesExternallyOwnedReader()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "mount_shared_reader.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["asset.bin"] = Bytes("shared-reader-content");

    PakOptions options;
    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    auto reader = std::make_shared<PakReader>();
    reader->SetCacheOptions(TestCacheOptions(root));
    assert(reader->Open(pakPath.string()));

    PakMount mount;
    assert(mount.MountReader(reader));

    // Direct access through the shared reader and access through the mount
    // see identical data.
    PakFileHandle directHandle = reader->Find("asset.bin");
    assert(directHandle);
    std::vector<uint8_t> direct;
    assert(reader->Load(directHandle, direct) == PakStatus::Ok);

    PakMountHandle mountedHandle = mount.Find("asset.bin");
    assert(mountedHandle);
    std::vector<uint8_t> viaMount;
    assert(mount.Load(mountedHandle, viaMount) == PakStatus::Ok);

    assert(direct == viaMount);
    assert(direct == files["asset.bin"]);
}

static void MountReaderRejectsUnopenedReader()
{
    auto reader = std::make_shared<PakReader>();
    PakMount mount;
    assert(!mount.MountReader(reader));
    assert(mount.LayerCount() == 0);
    assert(!mount.MountReader(nullptr));
}

static void MountThreeLayerPriorityAndLayerAccessors()
{
    fs::path root = TestRoot();
    fs::path basePak = root / "mount3_base.pak";
    fs::path patchPak = root / "mount3_patch.pak";
    fs::path dlcPak = root / "mount3_dlc.pak";

    std::map<std::string, std::vector<uint8_t>> baseFiles, patchFiles, dlcFiles;
    baseFiles["shared.txt"] = Bytes("base");
    patchFiles["shared.txt"] = Bytes("patch");
    dlcFiles["shared.txt"] = Bytes("dlc");

    PakOptions options;
    Pakker pakker;
    assert(pakker.CreatePak(basePak.string(), baseFiles, options));
    assert(pakker.CreatePak(patchPak.string(), patchFiles, options));
    assert(pakker.CreatePak(dlcPak.string(), dlcFiles, options));

    PakMount mount;
    assert(mount.Mount(basePak.string()));
    assert(mount.Mount(patchPak.string()));
    assert(mount.Mount(dlcPak.string()));
    assert(mount.LayerCount() == 3);

    PakMountHandle shared = mount.Find("shared.txt");
    assert(shared);
    std::vector<uint8_t> loaded;
    assert(mount.Load(shared, loaded) == PakStatus::Ok);
    assert(loaded == dlcFiles["shared.txt"]);

    // Layer 0 = first mounted (base), layer 2 = most recently mounted (dlc).
    std::shared_ptr<PakReader> layer0 = mount.GetLayerReader(0);
    std::shared_ptr<PakReader> layer2 = mount.GetLayerReader(2);
    assert(layer0 && layer2);
    std::vector<uint8_t> layer0Direct, layer2Direct;
    assert(layer0->Load(layer0->Find("shared.txt"), layer0Direct) == PakStatus::Ok);
    assert(layer2->Load(layer2->Find("shared.txt"), layer2Direct) == PakStatus::Ok);
    assert(layer0Direct == baseFiles["shared.txt"]);
    assert(layer2Direct == dlcFiles["shared.txt"]);
    assert(!mount.GetLayerReader(3));
}

static void MountClearRemovesAllLayers()
{
    fs::path root = TestRoot();
    fs::path basePak = root / "mount_clear_base.pak";
    fs::path patchPak = root / "mount_clear_patch.pak";

    std::map<std::string, std::vector<uint8_t>> baseFiles, patchFiles;
    baseFiles["a.txt"] = Bytes("a");
    patchFiles["b.txt"] = Bytes("b");

    PakOptions options;
    Pakker pakker;
    assert(pakker.CreatePak(basePak.string(), baseFiles, options));
    assert(pakker.CreatePak(patchPak.string(), patchFiles, options));

    PakMount mount;
    assert(mount.Mount(basePak.string()));
    assert(mount.Mount(patchPak.string()));
    assert(mount.Find("a.txt"));
    assert(mount.GetFileCount() == 2);

    PakMountHandle handleBeforeClear = mount.Find("a.txt");
    assert(handleBeforeClear);

    mount.Clear();
    assert(mount.LayerCount() == 0);
    assert(mount.GetFileCount() == 0);
    assert(!mount.Find("a.txt"));
    assert(!mount.FileExists("b.txt"));

    // A handle resolved before Clear() degrades safely (bounds-checked),
    // rather than dereferencing a dangling layer.
    std::vector<uint8_t> loaded;
    assert(mount.Load(handleBeforeClear, loaded) == PakStatus::InvalidHandle);
    assert(mount.Info(handleBeforeClear) == nullptr);
}

static void MountEnumerationDeduplicatesAcrossLayers()
{
    fs::path root = TestRoot();
    fs::path basePak = root / "mount_enum_base.pak";
    fs::path patchPak = root / "mount_enum_patch.pak";

    std::map<std::string, std::vector<uint8_t>> baseFiles;
    baseFiles["a.txt"] = Bytes("a");
    baseFiles["shared.txt"] = Bytes("base-shared");

    std::map<std::string, std::vector<uint8_t>> patchFiles;
    patchFiles["shared.txt"] = Bytes("patch-shared");
    patchFiles["b.txt"] = Bytes("b");

    PakOptions options;
    Pakker pakker;
    assert(pakker.CreatePak(basePak.string(), baseFiles, options));
    assert(pakker.CreatePak(patchPak.string(), patchFiles, options));

    PakMount mount;
    assert(mount.Mount(basePak.string()));
    assert(mount.Mount(patchPak.string()));

    std::vector<std::string> allFiles = mount.ListFiles();
    assert(allFiles.size() == 3);
    assert(std::is_sorted(allFiles.begin(), allFiles.end()));
    assert(std::find(allFiles.begin(), allFiles.end(), "a.txt") != allFiles.end());
    assert(std::find(allFiles.begin(), allFiles.end(), "b.txt") != allFiles.end());
    assert(std::find(allFiles.begin(), allFiles.end(), "shared.txt") != allFiles.end());

    PakMountHandle shared = mount.Find("shared.txt");
    std::vector<uint8_t> loaded;
    assert(mount.Load(shared, loaded) == PakStatus::Ok);
    assert(loaded == patchFiles["shared.txt"]);

    std::vector<std::string> prefixed = mount.ListFilesWithPrefix("sh");
    assert(prefixed.size() == 1);
    assert(prefixed[0] == "shared.txt");

    assert(mount.ListFilesWithPrefix("nomatch").empty());
}

static void MountConcurrentReadsAcrossLayers()
{
    fs::path root = TestRoot();
    fs::path basePak = root / "mount_concurrent_base.pak";
    fs::path patchPak = root / "mount_concurrent_patch.pak";

    std::map<std::string, std::vector<uint8_t>> baseFiles;
    baseFiles["base_only.bin"] = std::vector<uint8_t>(1024, 1);
    baseFiles["shared.bin"] = std::vector<uint8_t>(1024, 2);

    std::map<std::string, std::vector<uint8_t>> patchFiles;
    patchFiles["shared.bin"] = std::vector<uint8_t>(1024, 3);
    patchFiles["patch_only.bin"] = std::vector<uint8_t>(1024, 4);

    PakOptions options;
    options.compression = PakCompression::LZ4;
    Pakker pakker;
    assert(pakker.CreatePak(basePak.string(), baseFiles, options));
    assert(pakker.CreatePak(patchPak.string(), patchFiles, options));

    PakMount mount;
    assert(mount.Mount(basePak.string()));
    assert(mount.Mount(patchPak.string()));

    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&]() {
            for (int i = 0; i < 200; ++i) {
                PakMountHandle baseOnly = mount.Find("base_only.bin");
                PakMountHandle patchOnly = mount.Find("patch_only.bin");
                PakMountHandle shared = mount.Find("shared.bin");
                if (!baseOnly || !patchOnly || !shared) { failed = true; return; }

                std::vector<uint8_t> a, b, c;
                if (mount.Load(baseOnly, a) != PakStatus::Ok || a != baseFiles["base_only.bin"]) {
                    failed = true; return;
                }
                if (mount.Load(patchOnly, b) != PakStatus::Ok || b != patchFiles["patch_only.bin"]) {
                    failed = true; return;
                }
                if (mount.Load(shared, c) != PakStatus::Ok || c != patchFiles["shared.bin"]) {
                    failed = true; return;
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    assert(!failed.load());
}

// ---------------------------------------------------------------------------
// Zstd compression
// ---------------------------------------------------------------------------

static void ZstdCompressionRoundTrips()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "zstd_round_trip.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["compressed/data.bin"] = std::vector<uint8_t>(8192, 42);

    PakOptions options;
    options.compression = PakCompression::Zstd;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakInternal::PakEntry entry = ReadSingleEntry(pakPath);
    assert(entry.flags & PakInternal::PAK_FLAG_ZSTD_COMPRESSED);
    assert(!(entry.flags & PakInternal::PAK_FLAG_LZ4_COMPRESSED));
    assert(entry.compressedSize < entry.originalSize);

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));
    PakFileHandle handle = reader.Find("compressed/data.bin");
    assert(handle);
    const PakFileInfo* info = reader.Info(handle);
    assert(info && info->compressed);

    std::vector<uint8_t> loaded;
    assert(reader.Load(handle, loaded) == PakStatus::Ok);
    assert(loaded == files["compressed/data.bin"]);
}

static void MixedLz4AndZstdEntriesInSameArchive()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "mixed_codecs.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["lz4.bin"] = std::vector<uint8_t>(4096, 11);

    PakOptions options;
    options.compression = PakCompression::LZ4;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    std::vector<uint8_t> zstdData(4096, 22);
    assert(pakker.AddFileToPak(pakPath.string(), "zstd.bin", zstdData, PakCompression::Zstd));

    PakInternal::PakEntry lz4Entry = ReadEntryByName(pakPath, "lz4.bin");
    assert(lz4Entry.flags & PakInternal::PAK_FLAG_LZ4_COMPRESSED);
    PakInternal::PakEntry zstdEntry = ReadEntryByName(pakPath, "zstd.bin");
    assert(zstdEntry.flags & PakInternal::PAK_FLAG_ZSTD_COMPRESSED);

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));

    auto loadedLz4 = reader.LoadFile("lz4.bin");
    assert(loadedLz4 && *loadedLz4 == files["lz4.bin"]);
    auto loadedZstd = reader.LoadFile("zstd.bin");
    assert(loadedZstd && *loadedZstd == zstdData);
}

static void ValidatePakDeepVerifyCatchesCorruptedZstdEntry()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "deep_verify_zstd.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["zstd/data.bin"] = std::vector<uint8_t>(4096, 9);

    PakOptions options;
    options.compression = PakCompression::Zstd;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));
    assert(pakker.ValidatePak(pakPath.string(), /*deepVerify=*/true));

    PakInternal::PakEntry entry = ReadSingleEntry(pakPath);
    assert(entry.flags & PakInternal::PAK_FLAG_ZSTD_COMPRESSED);
    FlipByteAtFileOffset(pakPath, entry.offset);

    assert(pakker.ValidatePak(pakPath.string(), /*deepVerify=*/false));
    assert(!pakker.ValidatePak(pakPath.string(), /*deepVerify=*/true));
}

static void VerifyOnReadModeFailsClosedOnZstdMismatch()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "verify_on_read_zstd_mismatch.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["asset.bin"] = std::vector<uint8_t>(2048, 66);

    PakOptions options;
    options.compression = PakCompression::Zstd;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakInternal::PakEntry entry = ReadSingleEntry(pakPath);
    FlipByteAtFileOffset(pakPath, entry.offset);

    PakOpenOptions openOptions;
    openOptions.verifyOnRead = true;

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string(), openOptions));

    PakFileHandle handle = reader.Find("asset.bin");
    assert(handle);

    std::vector<uint8_t> loaded;
    assert(reader.Load(handle, loaded) == PakStatus::HashMismatch);
}

// ---------------------------------------------------------------------------
// PakReader enumeration
// ---------------------------------------------------------------------------

static void ReaderListFilesEnumeratesAllEntries()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "reader_list_files.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["a/one.bin"] = Bytes("one");
    files["a/two.bin"] = Bytes("two");
    files["b/three.bin"] = Bytes("three");

    Pakker pakker;
    PakOptions options;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));

    std::vector<std::string> listed = reader.ListFiles();
    std::vector<std::string> expected = pakker.ListFiles(pakPath.string());
    assert(listed == expected);
    assert(listed.size() == 3);
}

static void ReaderListFilesWithPrefixFilters()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "reader_list_files_prefix.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["a/one.bin"] = Bytes("one");
    files["a/two.bin"] = Bytes("two");
    files["b/three.bin"] = Bytes("three");

    Pakker pakker;
    PakOptions options;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));

    std::vector<std::string> aFiles = reader.ListFilesWithPrefix("a/");
    assert(aFiles.size() == 2);
    for (const auto& f : aFiles) assert(f.starts_with("a/"));

    std::vector<std::string> none = reader.ListFilesWithPrefix("missing/");
    assert(none.empty());
}

// ---------------------------------------------------------------------------
// PakReader::ReadRange
// ---------------------------------------------------------------------------

static void ReadRangeUncompressedMatchesFullReadSlice()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "read_range_uncompressed.pak";

    std::vector<uint8_t> content(4096);
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<uint8_t>(i & 0xff);

    std::map<std::string, std::vector<uint8_t>> files;
    files["asset.bin"] = content;

    PakOptions options; // compression = None
    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));
    PakFileHandle handle = reader.Find("asset.bin");
    assert(handle);

    const uint64_t rangeOffset = 100;
    const size_t rangeLength = 256;
    std::vector<uint8_t> slice(rangeLength);
    uint64_t written = 0;
    assert(reader.ReadRange(handle, rangeOffset, slice, &written) == PakStatus::Ok);
    assert(written == rangeLength);
    assert(std::equal(slice.begin(), slice.end(), content.begin() + rangeOffset));
}

static void ReadRangeCompressedReturnsUnsupported()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "read_range_compressed.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["asset.bin"] = std::vector<uint8_t>(4096, 5);

    PakOptions options;
    options.compression = PakCompression::LZ4;
    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));
    PakFileHandle handle = reader.Find("asset.bin");
    assert(handle);

    std::vector<uint8_t> slice(16);
    assert(reader.ReadRange(handle, 0, slice) == PakStatus::Unsupported);
}

static void ReadRangeEncryptedMatchesFullDecryptSlice()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "read_range_encrypted.pak";
    const std::string key = "range-read-secret-key";

    std::vector<uint8_t> content(2048);
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<uint8_t>((i * 7) & 0xff);

    std::map<std::string, std::vector<uint8_t>> files;
    files["asset.bin"] = content;

    PakOptions options; // compression = None -- encrypted-but-uncompressed range reads are supported
    Pakker pakker(key);
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader(key);
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));
    PakFileHandle handle = reader.Find("asset.bin");
    assert(handle);

    const uint64_t rangeOffset = 513; // deliberately not key-length-aligned
    const size_t rangeLength = 300;
    std::vector<uint8_t> slice(rangeLength);
    uint64_t written = 0;
    assert(reader.ReadRange(handle, rangeOffset, slice, &written) == PakStatus::Ok);
    assert(written == rangeLength);
    assert(std::equal(slice.begin(), slice.end(), content.begin() + rangeOffset));
}

static void ReadRangeOutOfBoundsRejected()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "read_range_oob.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["asset.bin"] = std::vector<uint8_t>(64, 1);

    PakOptions options;
    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));
    PakFileHandle handle = reader.Find("asset.bin");
    assert(handle);

    std::vector<uint8_t> slice(32);
    assert(reader.ReadRange(handle, 48, slice) == PakStatus::InvalidArgument); // 48+32 > 64
    assert(reader.ReadRange(handle, 1000, slice) == PakStatus::InvalidArgument);
}

// ---------------------------------------------------------------------------
// PakLooseOverlay
// ---------------------------------------------------------------------------

static void LooseOverlayPrefersLooseFileOverArchive()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "loose_overlay_prefers.pak";
    fs::path looseDir = root / "loose";
    fs::create_directories(looseDir / "textures");

    std::map<std::string, std::vector<uint8_t>> files;
    files["textures/diffuse.dds"] = Bytes("archive-content");

    Pakker pakker;
    PakOptions options;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    std::ofstream looseFile(looseDir / "textures" / "diffuse.dds", std::ios::binary);
    looseFile << "loose-content";
    looseFile.close();

    auto reader = std::make_shared<PakReader>();
    reader->SetCacheOptions(TestCacheOptions(root));
    assert(reader->Open(pakPath.string()));

    PakLooseOverlay overlay(reader, looseDir.string());
    assert(overlay.FileExists("textures/diffuse.dds"));

    std::vector<uint8_t> loaded;
    assert(overlay.Load("textures/diffuse.dds", loaded) == PakStatus::Ok);
    assert(loaded == Bytes("loose-content"));
}

static void LooseOverlayFallsBackToWrappedReader()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "loose_overlay_fallback.pak";
    fs::path looseDir = root / "loose_empty";
    fs::create_directories(looseDir);

    std::map<std::string, std::vector<uint8_t>> files;
    files["archive_only.bin"] = Bytes("archive-only-content");

    Pakker pakker;
    PakOptions options;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    auto reader = std::make_shared<PakReader>();
    reader->SetCacheOptions(TestCacheOptions(root));
    assert(reader->Open(pakPath.string()));

    PakLooseOverlay overlay(reader, looseDir.string());
    assert(overlay.FileExists("archive_only.bin"));

    std::vector<uint8_t> loaded;
    assert(overlay.Load("archive_only.bin", loaded) == PakStatus::Ok);
    assert(loaded == Bytes("archive-only-content"));

    assert(!overlay.FileExists("missing.bin"));
    std::vector<uint8_t> missing;
    assert(overlay.Load("missing.bin", missing) == PakStatus::NotFound);
}

static void LooseOverlayFileExistsChecksBoth()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "loose_overlay_exists.pak";
    fs::path looseDir = root / "loose_exists";
    fs::create_directories(looseDir);

    std::map<std::string, std::vector<uint8_t>> files;
    files["archive_only.bin"] = Bytes("archive");

    Pakker pakker;
    PakOptions options;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    std::ofstream looseFile(looseDir / "loose_only.bin", std::ios::binary);
    looseFile << "loose";
    looseFile.close();

    auto reader = std::make_shared<PakReader>();
    reader->SetCacheOptions(TestCacheOptions(root));
    assert(reader->Open(pakPath.string()));

    PakLooseOverlay overlay(reader, looseDir.string());
    assert(overlay.FileExists("archive_only.bin"));
    assert(overlay.FileExists("loose_only.bin"));
    assert(!overlay.FileExists("neither.bin"));
}

static void LooseOverlayRejectsPathTraversal()
{
    fs::path root = TestRoot();
    fs::path looseDir = root / "loose_traversal" / "inner";
    fs::create_directories(looseDir);

    // A real file one level above the loose directory -- if the traversal
    // guard were absent, "../secret.bin" would resolve straight to it.
    std::ofstream outsideFile(looseDir.parent_path() / "secret.bin", std::ios::binary);
    outsideFile << "should-not-be-readable";
    outsideFile.close();

    auto reader = std::make_shared<PakReader>(); // not open -- no wrapped-reader fallback possible
    PakLooseOverlay overlay(reader, looseDir.string());

    assert(!overlay.FileExists("../secret.bin"));
    std::vector<uint8_t> loaded;
    assert(overlay.Load("../secret.bin", loaded) == PakStatus::NotFound);
}

// ---------------------------------------------------------------------------
// v7: hash-keyed lookup and optional name residency
// ---------------------------------------------------------------------------

static void FindByHashMatchesFindByName()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "hash_lookup.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["textures/albedo.dds"] = Bytes("albedo");
    files["meshes/lod0.bin"] = Bytes("mesh");
    files["audio/gunshot.wav"] = Bytes("bang");

    Pakker pakker;
    PakOptions options;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));

    for (const auto& [name, data] : files) {
        PakFileHandle byName = reader.Find(name);
        PakFileHandle byHash = reader.FindByHash(PakPathHash(name));
        assert(byName);
        assert(byHash);
        assert(byName == byHash);

        const PakFileInfo* info = reader.Info(byHash);
        assert(info);
        assert(info->pathHash == PakPathHash(name));
    }

    // A path that isn't present must not resolve to anything.
    assert(!reader.FindByHash(PakPathHash("nope/missing.bin")));
}

static void FindNormalizesBeforeHashing()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "hash_normalize.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["ui/fonts/body.ttf"] = Bytes("font");

    Pakker pakker;
    PakOptions options;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));

    // Backslashes and a leading slash must normalize to the stored form
    // before hashing, or they would hash to unrelated values.
    PakFileHandle direct = reader.Find("ui/fonts/body.ttf");
    assert(direct);
    assert(reader.Find("ui\\fonts\\body.ttf") == direct);
    assert(reader.Find("/ui/fonts/body.ttf") == direct);
}

static void OpenWithoutNamesStillReadsByHash()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "no_names.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["plain.bin"] = std::vector<uint8_t>(4096, 7);
    files["compressed.bin"] = std::vector<uint8_t>(8192, 3);

    Pakker pakker;
    PakOptions options;
    options.compression = PakCompression::LZ4;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    PakOpenOptions openOptions;
    openOptions.loadNames = false;
    assert(reader.Open(pakPath.string(), openOptions));
    assert(reader.GetFileCount() == 2);

    for (const auto& [name, expected] : files) {
        PakFileHandle handle = reader.FindByHash(PakPathHash(name));
        assert(handle);

        const PakFileInfo* info = reader.Info(handle);
        assert(info);
        // The name blob was never read, so no name is available...
        assert(info->filename.empty());
        // ...but everything needed to actually serve the read still is.
        assert(info->originalSize == expected.size());
        assert(info->pathHash == PakPathHash(name));

        std::vector<uint8_t> data;
        assert(reader.Load(handle, data) == PakStatus::Ok);
        assert(data == expected);

        // Integrity verification does not depend on names either.
        assert(reader.VerifyEntry(handle) == PakStatus::Ok);
    }

    // Find() by name still resolves, because it hashes the path it is given.
    assert(reader.Find("plain.bin"));

    // Enumeration has nothing to report rather than a run of empty strings.
    assert(reader.ListFiles().empty());
    assert(reader.ListFilesWithPrefix("").empty());
}

static void MountComposesLayersOpenedWithoutNames()
{
    fs::path root = TestRoot();
    fs::path basePak = root / "no_names_base.pak";
    fs::path patchPak = root / "no_names_patch.pak";

    std::map<std::string, std::vector<uint8_t>> baseFiles;
    baseFiles["base_only.bin"] = std::vector<uint8_t>(512, 1);
    baseFiles["shared.bin"] = std::vector<uint8_t>(512, 2);

    std::map<std::string, std::vector<uint8_t>> patchFiles;
    patchFiles["shared.bin"] = std::vector<uint8_t>(512, 3);

    Pakker pakker;
    PakOptions options;
    assert(pakker.CreatePak(basePak.string(), baseFiles, options));
    assert(pakker.CreatePak(patchPak.string(), patchFiles, options));

    // The merged index is keyed on stored path hashes, so layers still
    // compose (and still override) with no names resident anywhere.
    PakOpenOptions openOptions;
    openOptions.loadNames = false;

    PakMount mount;
    for (const auto& path : {basePak, patchPak}) {
        auto reader = std::make_shared<PakReader>();
        assert(reader->Open(path.string(), openOptions));
        assert(mount.MountReader(reader));
    }

    assert(mount.GetFileCount() == 2);

    PakMountHandle shared = mount.Find("shared.bin");
    assert(shared);
    std::vector<uint8_t> data;
    assert(mount.Load(shared, data) == PakStatus::Ok);
    assert(data == patchFiles["shared.bin"]);

    PakMountHandle baseOnly = mount.Find("base_only.bin");
    assert(baseOnly);
    assert(mount.Load(baseOnly, data) == PakStatus::Ok);
    assert(data == baseFiles["base_only.bin"]);
}

static void PathHashIsStableAndCaseSensitive()
{
    // The hash is part of the on-disk format, so drift would silently
    // invalidate every shipped archive. Pin a couple of known relationships
    // rather than the literal values, which vary with the xxHash build.
    assert(PakPathHash("a/b.bin") == PakPathHash("a/b.bin"));
    assert(PakPathHash("a/b.bin") != PakPathHash("a/B.bin"));
    assert(PakPathHash("a/b.bin") != PakPathHash("a/b.bin "));
    assert(PakPathHash("") == PakPathHash(""));
}


// ---------------------------------------------------------------------------
// Parallel build pipeline
// ---------------------------------------------------------------------------

static std::vector<uint8_t> ReadWholeFile(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    assert(stream);
    const auto size = stream.tellg();
    stream.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size > 0) stream.read(reinterpret_cast<char*>(data.data()), size);
    assert(stream);
    return data;
}

// Entries are encoded on worker threads but written in input order, so worker
// count must not be observable in the output at all.
static void ParallelBuildMatchesSequentialByteForByte()
{
    fs::path root = TestRoot();

    std::map<std::string, std::vector<uint8_t>> files;
    for (int i = 0; i < 64; ++i) {
        // Mixed sizes and compressibility, so entries finish out of order.
        std::vector<uint8_t> data(static_cast<size_t>(256 + i * 733));
        for (size_t j = 0; j < data.size(); ++j) {
            data[j] = static_cast<uint8_t>((i * 31 + j / (1 + (i % 5))) & 0xff);
        }
        files["assets/set_" + std::to_string(i % 7) + "/item_" + std::to_string(i) + ".bin"] =
            std::move(data);
    }

    for (PakCompression compression : {PakCompression::None, PakCompression::LZ4,
                                       PakCompression::Zstd}) {
        fs::path sequentialPak = root / "seq.pak";
        fs::path parallelPak = root / "par.pak";

        PakOptions sequentialOptions;
        sequentialOptions.compression = compression;
        sequentialOptions.zstdLevel = 3;
        sequentialOptions.alignment = 64;
        sequentialOptions.workerThreads = 1;

        PakOptions parallelOptions = sequentialOptions;
        parallelOptions.workerThreads = 8;

        Pakker pakker;
        assert(pakker.CreatePak(sequentialPak.string(), files, sequentialOptions));
        assert(pakker.CreatePak(parallelPak.string(), files, parallelOptions));
        assert(ReadWholeFile(sequentialPak) == ReadWholeFile(parallelPak));

        // And the parallel archive must actually read back correctly.
        PakReader reader;
        reader.SetCacheOptions(TestCacheOptions(root));
        assert(reader.Open(parallelPak.string()));
        assert(reader.GetFileCount() == files.size());
        for (const auto& [name, expected] : files) {
            PakFileHandle handle = reader.Find(name);
            assert(handle);
            std::vector<uint8_t> loaded;
            assert(reader.Load(handle, loaded) == PakStatus::Ok);
            assert(loaded == expected);
            assert(reader.VerifyEntry(handle) == PakStatus::Ok);
        }
        reader.Close();

        fs::remove(sequentialPak);
        fs::remove(parallelPak);
    }
}

static void ParallelFolderBuildMatchesSequential()
{
    fs::path root = TestRoot();
    fs::path contentRoot = root / "content";

    std::map<std::string, std::vector<uint8_t>> expected;
    for (int i = 0; i < 48; ++i) {
        std::vector<uint8_t> data(static_cast<size_t>(128 + i * 521), static_cast<uint8_t>(i));
        const std::string name = "tree/branch_" + std::to_string(i % 5) + "/leaf_" +
                                 std::to_string(i) + ".bin";
        fs::path filePath = contentRoot / name;
        fs::create_directories(filePath.parent_path());
        std::ofstream out(filePath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        out.close();
        expected[name] = std::move(data);
    }

    fs::path sequentialPak = root / "folder_seq.pak";
    fs::path parallelPak = root / "folder_par.pak";

    PakOptions sequentialOptions;
    sequentialOptions.compression = PakCompression::Zstd;
    sequentialOptions.zstdLevel = 3;
    sequentialOptions.workerThreads = 1;

    PakOptions parallelOptions = sequentialOptions;
    parallelOptions.workerThreads = 8;

    Pakker pakker;
    assert(pakker.CreatePakFromFolder(sequentialPak.string(), contentRoot.string(),
                                      sequentialOptions));
    assert(pakker.CreatePakFromFolder(parallelPak.string(), contentRoot.string(),
                                      parallelOptions));
    assert(ReadWholeFile(sequentialPak) == ReadWholeFile(parallelPak));

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(parallelPak.string()));
    assert(reader.GetFileCount() == expected.size());
    for (const auto& [name, contents] : expected) {
        PakFileHandle handle = reader.Find(name);
        assert(handle);
        std::vector<uint8_t> loaded;
        assert(reader.Load(handle, loaded) == PakStatus::Ok);
        assert(loaded == contents);
    }
}


// ---------------------------------------------------------------------------
// Chunked compressed entries
// ---------------------------------------------------------------------------

// Mixed compressible and incompressible runs, so some chunks compress and
// some are stored raw -- both paths need exercising.
static std::vector<uint8_t> ChunkTestPayload(size_t bytes, uint64_t seed)
{
    std::vector<uint8_t> data(bytes);
    std::mt19937_64 rng(seed);
    size_t i = 0;
    while (i < bytes) {
        const size_t run = (std::min)(static_cast<size_t>(1024), bytes - i);
        if ((i / 1024) % 2 == 0) {
            for (size_t j = 0; j < run; ++j) data[i + j] = static_cast<uint8_t>(rng() & 0xff);
        } else {
            for (size_t j = 0; j < run; ++j) data[i + j] = static_cast<uint8_t>((i + j) % 7);
        }
        i += run;
    }
    return data;
}

static void ChunkedEntriesRoundTripAndReportChunkSize()
{
    fs::path root = TestRoot();

    for (PakCompression compression : {PakCompression::LZ4, PakCompression::Zstd}) {
        fs::path pakPath = root / "chunked.pak";

        std::map<std::string, std::vector<uint8_t>> files;
        // Larger than one chunk, so it gets chunked.
        files["big.bin"] = ChunkTestPayload(300 * 1024, 1);
        // Exactly one chunk boundary.
        files["exact.bin"] = ChunkTestPayload(8192, 2);
        // Smaller than the chunk size: must NOT be chunked.
        files["small.bin"] = ChunkTestPayload(1024, 3);

        PakOptions options;
        options.compression = compression;
        options.zstdLevel = 3;
        options.compressionChunkSize = 8192;

        Pakker pakker;
        assert(pakker.CreatePak(pakPath.string(), files, options));

        PakReader reader;
        reader.SetCacheOptions(TestCacheOptions(root));
        assert(reader.Open(pakPath.string()));

        for (const auto& [name, expected] : files) {
            PakFileHandle handle = reader.Find(name);
            assert(handle);

            std::vector<uint8_t> loaded;
            assert(reader.Load(handle, loaded) == PakStatus::Ok);
            assert(loaded == expected);
            assert(reader.VerifyEntry(handle) == PakStatus::Ok);
        }

        const PakFileInfo* big = reader.Info(reader.Find("big.bin"));
        assert(big);
        assert(big->chunkSize == 8192);

        const PakFileInfo* small = reader.Info(reader.Find("small.bin"));
        assert(small);
        // Below the chunk size, so chunking would only cost ratio.
        assert(small->chunkSize == 0);

        reader.Close();
        fs::remove(pakPath);
    }
}

static void ChunkedReadRangeMatchesFullRead()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "chunked_range.pak";

    const size_t payloadSize = 300 * 1024;
    std::map<std::string, std::vector<uint8_t>> files;
    files["video/clip.bin"] = ChunkTestPayload(payloadSize, 42);
    const std::vector<uint8_t>& expected = files["video/clip.bin"];

    PakOptions options;
    options.compression = PakCompression::Zstd;
    options.zstdLevel = 3;
    options.compressionChunkSize = 8192;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));

    PakFileHandle handle = reader.Find("video/clip.bin");
    assert(handle);

    // Ranges that start and end mid-chunk, span chunk boundaries, cover a
    // single whole chunk, and run to the very end of the entry.
    const std::pair<uint64_t, uint64_t> ranges[] = {
        {0, 1},
        {0, 64},
        {0, 8192},
        {1, 8191},
        {8192, 8192},
        {100, 20000},
        {8191, 2},
        {payloadSize - 1, 1},
        {payloadSize - 9000, 9000},
        {0, payloadSize},
    };

    for (const auto& [offset, size] : ranges) {
        std::vector<uint8_t> slice(static_cast<size_t>(size));
        uint64_t written = 0;
        assert(reader.ReadRange(handle, offset, slice, &written) == PakStatus::Ok);
        assert(written == size);
        assert(std::equal(slice.begin(), slice.end(),
                          expected.begin() + static_cast<ptrdiff_t>(offset)));
    }

    // Out-of-bounds ranges are still rejected.
    std::vector<uint8_t> overrun(16);
    assert(reader.ReadRange(handle, payloadSize - 8, overrun) == PakStatus::InvalidArgument);
    assert(reader.ReadRange(handle, payloadSize + 1, overrun) == PakStatus::InvalidArgument);
}

static void UnchunkedCompressedReadRangeStillUnsupported()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "unchunked_range.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["blob.bin"] = ChunkTestPayload(100 * 1024, 7);

    PakOptions options;
    options.compression = PakCompression::Zstd;
    options.zstdLevel = 3;
    options.compressionChunkSize = 0; // chunking off

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));

    PakFileHandle handle = reader.Find("blob.bin");
    assert(handle);
    const PakFileInfo* info = reader.Info(handle);
    assert(info && info->chunkSize == 0);

    // A whole-entry frame has no index to seek into, so this must keep
    // failing closed rather than quietly decoding the entire entry.
    std::vector<uint8_t> slice(64);
    assert(reader.ReadRange(handle, 0, slice) == PakStatus::Unsupported);
}

static void ChunkedEncryptedEntriesRoundTrip()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "chunked_encrypted.pak";
    const std::string key = "chunk-key";

    std::map<std::string, std::vector<uint8_t>> files;
    files["secret.bin"] = ChunkTestPayload(70 * 1024, 11);
    const std::vector<uint8_t>& expected = files["secret.bin"];

    PakOptions options;
    options.compression = PakCompression::LZ4;
    options.compressionChunkSize = 8192;

    Pakker pakker(key);
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader(key);
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));

    PakFileHandle handle = reader.Find("secret.bin");
    assert(handle);

    std::vector<uint8_t> loaded;
    assert(reader.Load(handle, loaded) == PakStatus::Ok);
    assert(loaded == expected);

    // Encryption forces the payload to be materialized and decrypted before
    // the chunk table can be read; the range result must still be identical.
    std::vector<uint8_t> slice(5000);
    assert(reader.ReadRange(handle, 12345, slice) == PakStatus::Ok);
    assert(std::equal(slice.begin(), slice.end(), expected.begin() + 12345));
}

static void ChunkedArchiveRejectsCorruptChunkTable()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "chunked_corrupt.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["blob.bin"] = ChunkTestPayload(64 * 1024, 13);

    PakOptions options;
    options.compression = PakCompression::Zstd;
    options.zstdLevel = 3;
    options.compressionChunkSize = 8192;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    const PakInternal::PakEntry entry = ReadSingleEntry(pakPath);
    assert(PakInternal::IsChunked(entry.flags));

    // Corrupt the first chunk's recorded compressed size. The table must not
    // be trusted to steer a decode past the end of the payload.
    {
        std::fstream stream(pakPath, std::ios::in | std::ios::out | std::ios::binary);
        assert(stream);
        stream.seekp(static_cast<std::streamoff>(entry.offset + sizeof(PakInternal::PakChunkHeader)),
                     std::ios::beg);
        uint32_t bogus = 0xFFFFFF00u;
        stream.write(reinterpret_cast<const char*>(&bogus), sizeof(bogus));
    }

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));

    PakFileHandle handle = reader.Find("blob.bin");
    assert(handle);

    std::vector<uint8_t> loaded;
    assert(reader.Load(handle, loaded) != PakStatus::Ok);

    std::vector<uint8_t> slice(64);
    assert(reader.ReadRange(handle, 0, slice) != PakStatus::Ok);
}


// ---------------------------------------------------------------------------
// Scatter reads
// ---------------------------------------------------------------------------

static void ReadBatchMatchesIndividualReads()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "batch.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    for (int i = 0; i < 64; ++i) {
        std::vector<uint8_t> data(static_cast<size_t>(512 + i * 97),
                                  static_cast<uint8_t>(i * 3));
        files["batch/asset_" + std::to_string(i) + ".bin"] = std::move(data);
    }

    PakOptions options;
    options.compression = PakCompression::LZ4;
    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));

    // Deliberately request in reverse order: ReadBatch reorders internally by
    // archive offset, and must still write each result into the right buffer.
    std::vector<std::string> names;
    for (const auto& [name, data] : files) names.push_back(name);
    std::reverse(names.begin(), names.end());

    std::vector<std::vector<uint8_t>> buffers(names.size());
    std::vector<PakReadRequest> requests(names.size());
    for (size_t i = 0; i < names.size(); ++i) {
        PakFileHandle handle = reader.Find(names[i]);
        assert(handle);
        buffers[i].resize(files[names[i]].size());
        requests[i].handle = handle;
        requests[i].destination = buffers[i];
    }

    const size_t succeeded = reader.ReadBatch(requests);
    assert(succeeded == requests.size());

    for (size_t i = 0; i < names.size(); ++i) {
        assert(requests[i].status == PakStatus::Ok);
        assert(requests[i].bytesWritten == files[names[i]].size());
        assert(buffers[i] == files[names[i]]);
        // The caller's array must come back in the order it was built.
        assert(requests[i].handle == reader.Find(names[i]));
    }
}

static void ReadBatchReportsPerRequestFailures()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "batch_errors.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["ok.bin"] = std::vector<uint8_t>(256, 9);

    Pakker pakker;
    PakOptions options;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));

    std::vector<uint8_t> good(256);
    std::vector<uint8_t> tooSmall(8);

    std::vector<PakReadRequest> requests(3);
    requests[0].handle = reader.Find("ok.bin");
    requests[0].destination = good;
    requests[1].handle = PakFileHandle{};                 // never resolved
    requests[1].destination = good;
    requests[2].handle = reader.Find("ok.bin");
    requests[2].destination = tooSmall;                   // undersized buffer

    const size_t succeeded = reader.ReadBatch(requests);
    assert(succeeded == 1);
    assert(requests[0].status == PakStatus::Ok);
    assert(requests[0].bytesWritten == 256);
    assert(requests[1].status == PakStatus::InvalidHandle);
    assert(requests[1].bytesWritten == 0);
    assert(requests[2].status == PakStatus::BufferTooSmall);
    assert(requests[2].bytesWritten == 0);

    // A closed reader fails every request rather than reporting success.
    reader.Close();
    const size_t afterClose = reader.ReadBatch(requests);
    assert(afterClose == 0);
    for (const auto& request : requests) {
        assert(request.status == PakStatus::NotOpen);
    }

    // An empty batch is not an error.
    std::vector<PakReadRequest> none;
    assert(reader.ReadBatch(none) == 0);
}

static void ReadBatchIsSafeFromMultipleThreads()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "batch_threads.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    for (int i = 0; i < 128; ++i) {
        files["mt/asset_" + std::to_string(i) + ".bin"] =
            std::vector<uint8_t>(1024, static_cast<uint8_t>(i));
    }

    PakOptions options;
    options.compression = PakCompression::LZ4;
    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    reader.SetCacheOptions(TestCacheOptions(root));
    assert(reader.Open(pakPath.string()));

    // The documented usage: the engine splits a frame's requests across its
    // own workers, each calling ReadBatch on a disjoint slice.
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&, t]() {
            for (int pass = 0; pass < 16; ++pass) {
                std::vector<std::vector<uint8_t>> buffers(16);
                std::vector<PakReadRequest> requests(16);
                for (int i = 0; i < 16; ++i) {
                    const int index = (t * 16 + i) % 128;
                    const std::string name = "mt/asset_" + std::to_string(index) + ".bin";
                    PakFileHandle handle = reader.Find(name);
                    if (!handle) { failed = true; return; }
                    buffers[i].assign(1024, 0);
                    requests[i].handle = handle;
                    requests[i].destination = buffers[i];
                }
                if (reader.ReadBatch(requests) != requests.size()) { failed = true; return; }
                for (int i = 0; i < 16; ++i) {
                    const int index = (t * 16 + i) % 128;
                    if (buffers[i] != std::vector<uint8_t>(1024, static_cast<uint8_t>(index))) {
                        failed = true;
                        return;
                    }
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    assert(!failed.load());
}


// ---------------------------------------------------------------------------
// Additional comprehensive tests
// ---------------------------------------------------------------------------

static void PakkerToolingAndExtraction()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "tooling.pak";
    fs::path extractDir = root / "extracted";

    std::map<std::string, std::vector<uint8_t>> files;
    files["plain.txt"] = Bytes("plain-text-content");
    files["compressed/lz4.bin"] = std::vector<uint8_t>(4096, 0xAA);
    files["nested/folder/deep.bin"] = Bytes("nested-content");
    files["empty.bin"] = {};

    PakOptions options;
    options.compression = PakCompression::LZ4;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));
    assert(pakker.ValidatePak(pakPath.string(), /*deepVerify=*/true));

    // FileExists and GetFileInfo
    assert(pakker.FileExists(pakPath.string(), "plain.txt"));
    assert(pakker.FileExists(pakPath.string(), "/plain.txt"));
    assert(pakker.FileExists(pakPath.string(), "compressed\\lz4.bin"));
    assert(!pakker.FileExists(pakPath.string(), "missing.txt"));

    Pakker::FileInfo info = pakker.GetFileInfo(pakPath.string(), "plain.txt");
    assert(info.found);
    assert(info.filename == "plain.txt");
    assert(info.size == files["plain.txt"].size());

    Pakker::FileInfo missingInfo = pakker.GetFileInfo(pakPath.string(), "nonexistent.bin");
    assert(!missingInfo.found);

    // ReadFileFromPak
    std::vector<uint8_t> plainRead = pakker.ReadFileFromPak(pakPath.string(), "plain.txt");
    assert(plainRead == files["plain.txt"]);
    std::vector<uint8_t> lz4Read = pakker.ReadFileFromPak(pakPath.string(), "compressed/lz4.bin");
    assert(lz4Read == files["compressed/lz4.bin"]);
    std::vector<uint8_t> emptyRead = pakker.ReadFileFromPak(pakPath.string(), "empty.bin");
    assert(emptyRead.empty());
    std::vector<uint8_t> missingRead = pakker.ReadFileFromPak(pakPath.string(), "not_there.bin");
    assert(missingRead.empty());

    // LoadFile
    auto plainLoad = pakker.LoadFile(pakPath.string(), "plain.txt");
    assert(plainLoad && *plainLoad == files["plain.txt"]);
    auto missingLoad = pakker.LoadFile(pakPath.string(), "not_there.bin");
    assert(missingLoad == nullptr);

    // ListFiles and ListFilesWithPrefix
    std::vector<std::string> allFiles = pakker.ListFiles(pakPath.string());
    assert(allFiles.size() == 4);
    std::vector<std::string> prefixed = pakker.ListFilesWithPrefix(pakPath.string(), "compressed");
    assert(prefixed.size() == 1 && prefixed[0] == "compressed/lz4.bin");
    std::vector<std::string> noMatch = pakker.ListFilesWithPrefix(pakPath.string(), "unknown/");
    assert(noMatch.empty());

    // ListPak
    assert(pakker.ListPak(pakPath.string()));

    // ExtractPak
    assert(pakker.ExtractPak(pakPath.string(), extractDir.string()));
    assert(fs::exists(extractDir / "plain.txt"));
    assert(fs::exists(extractDir / "compressed" / "lz4.bin"));
    assert(fs::exists(extractDir / "nested" / "folder" / "deep.bin"));
    assert(fs::exists(extractDir / "empty.bin"));
    assert(ReadWholeFile(extractDir / "plain.txt") == files["plain.txt"]);
    assert(ReadWholeFile(extractDir / "compressed" / "lz4.bin") == files["compressed/lz4.bin"]);
    assert(ReadWholeFile(extractDir / "nested" / "folder" / "deep.bin") == files["nested/folder/deep.bin"]);
    assert(ReadWholeFile(extractDir / "empty.bin").empty());

    // ExtractSingleFile
    fs::path singleExtractPath = root / "single_extract.txt";
    assert(pakker.ExtractSingleFile(pakPath.string(), "plain.txt", singleExtractPath.string()));
    assert(ReadWholeFile(singleExtractPath) == files["plain.txt"]);

    fs::path singleEmptyPath = root / "single_empty.bin";
    assert(pakker.ExtractSingleFile(pakPath.string(), "empty.bin", singleEmptyPath.string()));
    assert(fs::exists(singleEmptyPath));
    assert(ReadWholeFile(singleEmptyPath).empty());

    fs::path missingExtractPath = root / "should_not_exist.bin";
    assert(!pakker.ExtractSingleFile(pakPath.string(), "missing.bin", missingExtractPath.string()));
    assert(!fs::exists(missingExtractPath));

    // Non-existent pak operations
    assert(pakker.GetFileCount((root / "nonexistent.pak").string()) == 0);
    assert(!pakker.FileExists((root / "nonexistent.pak").string(), "any.bin"));
    assert(!pakker.ExtractPak((root / "nonexistent.pak").string(), extractDir.string()));
    assert(!pakker.ListPak((root / "nonexistent.pak").string()));
    assert(pakker.ListFiles((root / "nonexistent.pak").string()).empty());
    assert(pakker.ReadFileFromPak((root / "nonexistent.pak").string(), "any.bin").empty());
}

static void PakkerEncryptedWorkflow()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "encrypted_workflow.pak";
    fs::path extractDir = root / "enc_extracted";
    const std::string key = "super-secret-passphrase";
    const std::string wrongKey = "wrong-passphrase";

    std::map<std::string, std::vector<uint8_t>> files;
    files["secret.txt"] = Bytes("classified information");
    files["data.bin"] = std::vector<uint8_t>(2048, 0x5A);

    PakOptions options;
    options.compression = PakCompression::LZ4;
    Pakker pakker(key);
    assert(pakker.CreatePak(pakPath.string(), files, options));

    // AddFileToPak to encrypted archive
    std::vector<uint8_t> addedData = Bytes("appended encrypted content");
    assert(pakker.AddFileToPak(pakPath.string(), "added.txt", addedData));

    // Verify ReadFileFromPak with correct key
    assert(pakker.ReadFileFromPak(pakPath.string(), "secret.txt") == files["secret.txt"]);
    assert(pakker.ReadFileFromPak(pakPath.string(), "added.txt") == addedData);

    // Verify ExtractPak with correct key
    assert(pakker.ExtractPak(pakPath.string(), extractDir.string()));
    assert(ReadWholeFile(extractDir / "secret.txt") == files["secret.txt"]);
    assert(ReadWholeFile(extractDir / "added.txt") == addedData);

    // PakReader with correct key
    PakReader correctReader(key);
    assert(correctReader.Open(pakPath.string()));
    assert(correctReader.ReadFile("secret.txt") == files["secret.txt"]);
    assert(correctReader.ReadFile("added.txt") == addedData);

    // PakReader with wrong key fails to decompress compressed payload
    PakReader wrongReader(wrongKey);
    assert(wrongReader.Open(pakPath.string()));
    PakFileHandle dataHandle = wrongReader.Find("data.bin");
    assert(dataHandle);
    std::vector<uint8_t> wrongLoaded;
    PakStatus status = wrongReader.Load(dataHandle, wrongLoaded);
    assert(status == PakStatus::DecompressionFailed);

    // Mount with encrypted archive
    PakMount mount;
    assert(mount.Mount(pakPath.string(), key));
    assert(mount.ReadFile("secret.txt") == files["secret.txt"]);
    assert(mount.ReadFile("added.txt") == addedData);
}

static void PakkerValidationAndErrorHandling()
{
    fs::path root = TestRoot();
    Pakker pakker;

    // Invalid alignment (must be power of 2)
    {
        fs::path pakPath = root / "bad_align.pak";
        std::map<std::string, std::vector<uint8_t>> files;
        files["test.txt"] = Bytes("abc");
        PakOptions options;
        options.alignment = 3;
        assert(!pakker.CreatePak(pakPath.string(), files, options));
    }

    // Invalid chunk size (not power of 2, too small, too large)
    {
        fs::path pakPath = root / "bad_chunk.pak";
        std::map<std::string, std::vector<uint8_t>> files;
        files["test.txt"] = Bytes("abc");
        PakOptions options;
        options.compression = PakCompression::LZ4;

        options.compressionChunkSize = 3000;
        assert(!pakker.CreatePak(pakPath.string(), files, options));

        options.compressionChunkSize = 2048;
        assert(!pakker.CreatePak(pakPath.string(), files, options));

        options.compressionChunkSize = 32u * 1024 * 1024;
        assert(!pakker.CreatePak(pakPath.string(), files, options));
    }

    // Duplicate normalized names in CreatePak
    {
        fs::path pakPath = root / "dup_names.pak";
        std::map<std::string, std::vector<uint8_t>> files;
        files["folder/file.txt"] = Bytes("one");
        files["folder\\file.txt"] = Bytes("two");
        PakOptions options;
        assert(!pakker.CreatePak(pakPath.string(), files, options));
    }

    // Invalid filenames
    {
        const std::string badNames[] = {
            "has<angle.bin", "has>angle.bin", "has:colon.bin", "has\"quote.bin",
            "has|pipe.bin", "has?question.bin", "has*star.bin", "path/../traversal.bin",
            "", std::string("has\0null.bin", 12)
        };
        for (const auto& badName : badNames) {
            fs::path pakPath = root / "invalid_name.pak";
            std::map<std::string, std::vector<uint8_t>> files;
            files[badName] = Bytes("data");
            PakOptions options;
            assert(!pakker.CreatePak(pakPath.string(), files, options));
        }
    }

    // AddFileToPak duplicate and error checks
    {
        fs::path pakPath = root / "add_dup.pak";
        std::map<std::string, std::vector<uint8_t>> files;
        files["original.txt"] = Bytes("hello");
        PakOptions options;
        assert(pakker.CreatePak(pakPath.string(), files, options));

        assert(!pakker.AddFileToPak(pakPath.string(), "original.txt", Bytes("duplicate")));
        assert(!pakker.AddFileToPak(pakPath.string(), "bad:name.txt", Bytes("bad")));
        assert(!pakker.AddFileToPak((root / "missing.pak").string(), "new.txt", Bytes("new")));
    }

    // Corrupt header magic
    {
        fs::path pakPath = root / "corrupt_magic.pak";
        std::map<std::string, std::vector<uint8_t>> files;
        files["file.bin"] = Bytes("content");
        PakOptions options;
        assert(pakker.CreatePak(pakPath.string(), files, options));

        std::fstream stream(pakPath, std::ios::in | std::ios::out | std::ios::binary);
        assert(stream);
        char badMagic[4] = {'B', 'A', 'D', '0'};
        stream.write(badMagic, 4);
        stream.close();

        assert(!pakker.ValidatePak(pakPath.string()));
        PakReader reader;
        assert(!reader.Open(pakPath.string()));
    }
}

static void PakReaderComprehensiveApi()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "reader_api.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["assets/texture.bin"] = std::vector<uint8_t>(2048, 0x11);
    files["scripts/init.lua"] = Bytes("print('hello')");
    files["empty.dat"] = {};

    PakOptions options;
    options.compression = PakCompression::LZ4;

    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    assert(!reader.IsOpen());
    assert(reader.GetFileCount() == 0);
    assert(reader.InfoByIndex(0) == nullptr);
    assert(reader.Prefetch(PakFileHandle{0}) == PakStatus::NotOpen);

    assert(reader.Open(pakPath.string()));
    assert(reader.IsOpen());
    assert(reader.GetFileCount() == 3);

    // InfoByIndex
    for (uint32_t i = 0; i < reader.GetFileCount(); ++i) {
        const PakFileInfo* info = reader.InfoByIndex(i);
        assert(info != nullptr);
        assert(!info->filename.empty());
        assert(info->pathHash == PakPathHash(info->filename));
    }
    assert(reader.InfoByIndex(3) == nullptr);
    assert(reader.InfoByIndex(9999) == nullptr);

    // GetFileInfo
    PakReader::FileInfo texInfo = reader.GetFileInfo("assets/texture.bin");
    assert(texInfo.found);
    assert(texInfo.filename == "assets/texture.bin");
    assert(texInfo.originalSize == 2048);
    assert(texInfo.compressed);

    PakReader::FileInfo emptyInfo = reader.GetFileInfo("empty.dat");
    assert(emptyInfo.found);
    assert(emptyInfo.originalSize == 0);

    PakReader::FileInfo notFoundInfo = reader.GetFileInfo("missing.dat");
    assert(!notFoundInfo.found);

    // ReadFile
    std::vector<uint8_t> scriptData = reader.ReadFile("scripts/init.lua");
    assert(scriptData == files["scripts/init.lua"]);
    std::vector<uint8_t> missingData = reader.ReadFile("nonexistent.dat");
    assert(missingData.empty());

    // LoadFile
    auto scriptLoad = reader.LoadFile("scripts/init.lua");
    assert(scriptLoad && *scriptLoad == files["scripts/init.lua"]);
    auto missingLoad = reader.LoadFile("nonexistent.dat");
    assert(missingLoad == nullptr);

    // ReadFiles batch convenience wrapper
    std::vector<std::string> batchNames = {
        "assets/texture.bin",
        "scripts/init.lua",
        "missing.dat",
        "empty.dat"
    };
    auto batchResults = reader.ReadFiles(batchNames);
    assert(batchResults.size() == 4);
    assert(batchResults[0].first == "assets/texture.bin" && batchResults[0].second == files["assets/texture.bin"]);
    assert(batchResults[1].first == "scripts/init.lua" && batchResults[1].second == files["scripts/init.lua"]);
    assert(batchResults[2].first == "missing.dat" && batchResults[2].second.empty());
    assert(batchResults[3].first == "empty.dat" && batchResults[3].second.empty());

    // Prefetch
    assert(reader.Prefetch(reader.Find("assets/texture.bin")) == PakStatus::Ok);
    assert(reader.Prefetch(reader.Find("empty.dat")) == PakStatus::Ok);
    assert(reader.Prefetch(PakFileHandle{}) == PakStatus::InvalidHandle);
    assert(reader.Prefetch(PakFileHandle{999}) == PakStatus::InvalidHandle);

    // Move constructor
    PakReader movedReader(std::move(reader));
    assert(!reader.IsOpen());
    assert(movedReader.IsOpen());
    assert(movedReader.GetFileCount() == 3);
    assert(movedReader.ReadFile("scripts/init.lua") == files["scripts/init.lua"]);

    // Move assignment
    PakReader assignedReader;
    assignedReader = std::move(movedReader);
    assert(!movedReader.IsOpen());
    assert(assignedReader.IsOpen());
    assert(assignedReader.GetFileCount() == 3);
    assert(assignedReader.ReadFile("scripts/init.lua") == files["scripts/init.lua"]);

    assignedReader.Close();
    assert(!assignedReader.IsOpen());
    assert(assignedReader.GetFileCount() == 0);
}

static void PakReaderCacheManagement()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "cache_mgmt.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["c1.bin"] = std::vector<uint8_t>(4096, 1);
    files["c2.bin"] = std::vector<uint8_t>(4096, 2);

    PakOptions pakOptions;
    pakOptions.compression = PakCompression::LZ4;
    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, pakOptions));

    PakCacheOptions options = TestCacheOptions(root);
    options.memoryBudgetBytes = 16 * 1024 * 1024;
    options.persistentBudgetBytes = 32 * 1024 * 1024;

    PakReader reader;
    reader.SetCacheOptions(options);

    PakCacheOptions retrieved = reader.GetCacheOptions();
    assert(retrieved.memoryBudgetBytes == options.memoryBudgetBytes);
    assert(retrieved.persistentBudgetBytes == options.persistentBudgetBytes);
    assert(retrieved.persistentCacheDirectory == options.persistentCacheDirectory);

    assert(reader.Open(pakPath.string()));
    assert(reader.ReadFile("c1.bin") == files["c1.bin"]);
    assert(reader.ReadFile("c2.bin") == files["c2.bin"]);

    // ClearPersistentCache and ClearCache
    assert(reader.ClearPersistentCache());
    assert(reader.ClearCache());
    assert(reader.GetCacheStats().memoryBytes == 0);

    // ReadBatch with PakBatchOptions (prefetch = false and prefetch = true)
    PakBatchOptions noPrefetch;
    noPrefetch.prefetch = false;

    std::vector<uint8_t> buf1(files["c1.bin"].size());
    std::vector<uint8_t> buf2(files["c2.bin"].size());
    std::vector<PakReadRequest> requests(2);
    requests[0].handle = reader.Find("c1.bin");
    requests[0].destination = buf1;
    requests[1].handle = reader.Find("c2.bin");
    requests[1].destination = buf2;

    assert(reader.ReadBatch(requests, noPrefetch) == 2);
    assert(requests[0].status == PakStatus::Ok && buf1 == files["c1.bin"]);
    assert(requests[1].status == PakStatus::Ok && buf2 == files["c2.bin"]);
}

static void PakMountComprehensiveApi()
{
    fs::path root = TestRoot();
    fs::path basePak = root / "mount_api_base.pak";
    fs::path patchPak = root / "mount_api_patch.pak";

    std::map<std::string, std::vector<uint8_t>> baseFiles;
    baseFiles["raw.txt"] = Bytes("base-raw-text");
    baseFiles["shader.bin"] = std::vector<uint8_t>(1024, 0x11);
    baseFiles["override.txt"] = Bytes("base-override");

    std::map<std::string, std::vector<uint8_t>> patchFiles;
    patchFiles["override.txt"] = Bytes("patch-override");
    patchFiles["new.txt"] = Bytes("patch-new");

    PakOptions options;
    Pakker pakker;
    assert(pakker.CreatePak(basePak.string(), baseFiles, options));
    assert(pakker.CreatePak(patchPak.string(), patchFiles, options));

    PakMount mount;
    assert(mount.Mount(basePak.string()));
    assert(mount.Mount(patchPak.string()));
    assert(mount.LayerCount() == 2);
    assert(mount.GetFileCount() == 4);

    // Find and Info
    PakMountHandle overrideHandle = mount.Find("override.txt");
    assert(overrideHandle);
    assert(overrideHandle.layerIndex == 1);
    const PakFileInfo* info = mount.Info(overrideHandle);
    assert(info != nullptr);
    assert(info->filename == "override.txt");
    assert(info->originalSize == patchFiles["override.txt"].size());

    // Resolve
    std::string_view lookupNames[] = {"raw.txt", "missing.bin", "override.txt", "new.txt"};
    PakMountHandle handles[4];
    assert(mount.Resolve(lookupNames, handles) == 3);
    assert(handles[0] && handles[0].layerIndex == 0);
    assert(!handles[1]);
    assert(handles[2] && handles[2].layerIndex == 1);
    assert(handles[3] && handles[3].layerIndex == 1);

    // View
    PakView view;
    PakMountHandle rawHandle = mount.Find("raw.txt");
    assert(rawHandle);
    PakStatus viewStatus = mount.View(rawHandle, view);
    if (viewStatus == PakStatus::Ok) {
        assert(view.mapped);
        assert(view.size == baseFiles["raw.txt"].size());
        assert(std::string_view(reinterpret_cast<const char*>(view.data), static_cast<size_t>(view.size)) == "base-raw-text");
    }

    // Read with buffer size checking
    std::vector<uint8_t> tooSmall(2);
    uint64_t written = 999;
    assert(mount.Read(rawHandle, tooSmall, &written) == PakStatus::BufferTooSmall);
    assert(written == 0);

    std::vector<uint8_t> exact(baseFiles["raw.txt"].size());
    assert(mount.Read(rawHandle, exact, &written) == PakStatus::Ok);
    assert(written == baseFiles["raw.txt"].size());
    assert(exact == baseFiles["raw.txt"]);

    // Read with invalid handle
    assert(mount.Read(PakMountHandle{}, exact) == PakStatus::InvalidHandle);
    assert(mount.View(PakMountHandle{}, view) == PakStatus::InvalidHandle);
    assert(mount.Load(PakMountHandle{}, exact) == PakStatus::InvalidHandle);
    assert(mount.Prefetch(PakMountHandle{}) == PakStatus::InvalidHandle);

    // Prefetch
    assert(mount.Prefetch(rawHandle) == PakStatus::Ok);

    // ReadFile and LoadFile
    assert(mount.ReadFile("override.txt") == patchFiles["override.txt"]);
    assert(mount.ReadFile("raw.txt") == baseFiles["raw.txt"]);
    assert(mount.ReadFile("missing.txt").empty());

    auto loaded = mount.LoadFile("override.txt");
    assert(loaded && *loaded == patchFiles["override.txt"]);
    assert(mount.LoadFile("missing.txt") == nullptr);

    // ReadFileZeroCopy
    PakSpan span = mount.ReadFileZeroCopy("raw.txt");
    assert(span);
    assert(span.size == baseFiles["raw.txt"].size());
    assert(std::string_view(reinterpret_cast<const char*>(span.data), static_cast<size_t>(span.size)) == "base-raw-text");
    assert(!mount.ReadFileZeroCopy("missing.txt"));

    // Move constructor
    PakMount movedMount(std::move(mount));
    assert(mount.LayerCount() == 0);
    assert(movedMount.LayerCount() == 2);
    assert(movedMount.ReadFile("override.txt") == patchFiles["override.txt"]);

    // Move assignment
    PakMount assignedMount;
    assignedMount = std::move(movedMount);
    assert(movedMount.LayerCount() == 0);
    assert(assignedMount.LayerCount() == 2);
    assert(assignedMount.ReadFile("override.txt") == patchFiles["override.txt"]);
}

static void PakLooseOverlayComprehensiveApi()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "loose_api.pak";
    fs::path looseDir = root / "loose_api_dir";
    fs::create_directories(looseDir / "sub");

    std::map<std::string, std::vector<uint8_t>> files;
    files["archive_file.txt"] = Bytes("from-archive");
    files["shared.txt"] = Bytes("archive-version");

    PakOptions options;
    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    // Create loose files
    std::ofstream f1(looseDir / "shared.txt", std::ios::binary);
    f1 << "loose-version";
    f1.close();

    std::ofstream f2(looseDir / "sub" / "empty_loose.bin", std::ios::binary);
    f2.close();

    auto reader = std::make_shared<PakReader>();
    assert(reader->Open(pakPath.string()));

    PakLooseOverlay overlay(reader, looseDir.string());
    assert(overlay.WrappedReader() == reader);
    assert(overlay.LooseDirectory() == looseDir.string());

    // FileExists
    assert(overlay.FileExists("shared.txt"));
    assert(overlay.FileExists("sub/empty_loose.bin"));
    assert(overlay.FileExists("archive_file.txt"));
    assert(!overlay.FileExists("nowhere.txt"));

    // Read: loose file
    std::vector<uint8_t> buf(32);
    uint64_t written = 0;
    assert(overlay.Read("shared.txt", buf, &written) == PakStatus::Ok);
    assert(written == 13);
    assert(std::string_view(reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(written)) == "loose-version");

    // Read: buffer too small
    std::vector<uint8_t> tiny(4);
    assert(overlay.Read("shared.txt", tiny, &written) == PakStatus::BufferTooSmall);

    // Read: archive fallback
    buf.assign(32, 0);
    assert(overlay.Read("archive_file.txt", buf, &written) == PakStatus::Ok);
    assert(written == 12);
    assert(std::string_view(reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(written)) == "from-archive");

    // Read: empty loose file
    written = 999;
    assert(overlay.Read("sub/empty_loose.bin", buf, &written) == PakStatus::Ok);
    assert(written == 0);

    // Load: empty loose file
    std::vector<uint8_t> loadedData;
    assert(overlay.Load("sub/empty_loose.bin", loadedData) == PakStatus::Ok);
    assert(loadedData.empty());

    // Read: non-existent file
    assert(overlay.Read("nowhere.txt", buf) == PakStatus::NotFound);
    assert(overlay.Load("nowhere.txt", loadedData) == PakStatus::NotFound);

    // Overlay with null reader
    PakLooseOverlay nullReaderOverlay(nullptr, looseDir.string());
    assert(nullReaderOverlay.FileExists("shared.txt"));
    assert(!nullReaderOverlay.FileExists("archive_file.txt"));
    assert(nullReaderOverlay.Load("archive_file.txt", loadedData) == PakStatus::NotFound);
    assert(nullReaderOverlay.Load("shared.txt", loadedData) == PakStatus::Ok);
    assert(loadedData == Bytes("loose-version"));
}

static void PakPlatformDirectTests()
{
    fs::path root = TestRoot();
    fs::path filePath = root / "platform_test.bin";

    std::vector<uint8_t> testBytes(8192);
    for (size_t i = 0; i < testBytes.size(); ++i) {
        testBytes[i] = static_cast<uint8_t>(i % 251);
    }
    {
        std::ofstream out(filePath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(testBytes.data()), static_cast<std::streamsize>(testBytes.size()));
    }

    // Default cache directory
    std::string cacheDir = PakPlatform::GetDefaultCacheDirectory();
#if defined(_WIN32)
    assert(!cacheDir.empty());
    assert(cacheDir.find("Pakker") != std::string::npos);
#endif

    // MapFileReadOnly
    PakPlatform::MappedFile mf = PakPlatform::MapFileReadOnly(filePath.string().c_str());
#ifndef PAK_NO_MMAP
    assert(mf.data != nullptr);
    assert(mf.size == testBytes.size());
    assert(std::memcmp(mf.data, testBytes.data(), testBytes.size()) == 0);

    // PrefetchMappedRange
    assert(PakPlatform::PrefetchMappedRange(mf, 0, 4096));
    assert(PakPlatform::PrefetchMappedRange(mf, 4096, 4096));
    assert(PakPlatform::PrefetchMappedRange(mf, 0, 0));
    assert(!PakPlatform::PrefetchMappedRange(mf, 9000, 100));

    // PrefetchMappedRanges
    PakPlatform::PrefetchRange ranges[] = {
        {0, 1024},
        {2048, 1024},
        {4096, 1024}
    };
    assert(PakPlatform::PrefetchMappedRanges(mf, ranges, 3));
    assert(PakPlatform::PrefetchMappedRanges(mf, nullptr, 0));

    // Unmap
    PakPlatform::UnmapFile(mf);
    assert(mf.data == nullptr);
    assert(mf.size == 0);
#endif

    // MapFileReadOnly on non-existent file
    PakPlatform::MappedFile badMf = PakPlatform::MapFileReadOnly((root / "nonexistent.bin").string().c_str());
    assert(badMf.data == nullptr);
    assert(badMf.size == 0);

    // MapFileReadOnly on 0-byte file
    fs::path emptyPath = root / "empty_file.bin";
    {
        std::ofstream out(emptyPath, std::ios::binary);
    }
    PakPlatform::MappedFile emptyMf = PakPlatform::MapFileReadOnly(emptyPath.string().c_str());
    assert(emptyMf.data == nullptr);
    assert(emptyMf.size == 0);

    // PrefetchFileRange
    assert(PakPlatform::PrefetchFileRange(filePath.string().c_str(), 0, 4096));
    assert(PakPlatform::PrefetchFileRange(nullptr, 0, 0));
}

static void LoggingAndDiagnostics()
{
    assert(std::string_view(PakStatusToString(PakStatus::Ok)) == "Ok");
    assert(std::string_view(PakStatusToString(PakStatus::NotOpen)) == "NotOpen");
    assert(std::string_view(PakStatusToString(PakStatus::NotFound)) == "NotFound");
    assert(std::string_view(PakStatusToString(PakStatus::InvalidHandle)) == "InvalidHandle");
    assert(std::string_view(PakStatusToString(PakStatus::InvalidArgument)) == "InvalidArgument");
    assert(std::string_view(PakStatusToString(PakStatus::BufferTooSmall)) == "BufferTooSmall");
    assert(std::string_view(PakStatusToString(PakStatus::Unsupported)) == "Unsupported");
    assert(std::string_view(PakStatusToString(PakStatus::CorruptArchive)) == "CorruptArchive");
    assert(std::string_view(PakStatusToString(PakStatus::IoError)) == "IoError");
    assert(std::string_view(PakStatusToString(PakStatus::DecompressionFailed)) == "DecompressionFailed");
    assert(std::string_view(PakStatusToString(PakStatus::HashMismatch)) == "HashMismatch");
    assert(std::string_view(PakStatusToString(static_cast<PakStatus>(9999))) == "Unknown");

    static std::atomic<int> logCount{0};
    static std::atomic<PakLogLevel> lastLevel{PakLogLevel::Info};
    auto callback = [](PakLogLevel level, const char* msg) {
        (void)msg;
        lastLevel.store(level);
        logCount.fetch_add(1);
    };

    PakSetLogCallback(callback);

    PakInternal::Log(PakLogLevel::Warning, "Test warning log");
    assert(logCount.load() >= 1);
    assert(lastLevel.load() == PakLogLevel::Warning);

    PakInternal::Log(PakLogLevel::Error, "Test error log");
    assert(lastLevel.load() == PakLogLevel::Error);

    PakSetLogCallback(nullptr);
    const int countAfterReset = logCount.load();
    PakInternal::Log(PakLogLevel::Error, "Should not increment counter");
    assert(logCount.load() == countAfterReset);
}

static void InternalUtilitiesRobustness()
{
    using namespace PakInternal;

    // NormalizePathSeparators
    assert(NormalizePathSeparators("") == "");
    assert(NormalizePathSeparators("///") == "");
    assert(NormalizePathSeparators("\\\\\\") == "");
    assert(NormalizePathSeparators("/dir/file.txt") == "dir/file.txt");
    assert(NormalizePathSeparators("///dir/sub/file.txt") == "dir/sub/file.txt");
    assert(NormalizePathSeparators("dir\\sub\\file.txt") == "dir/sub/file.txt");
    assert(NormalizePathSeparators("\\\\dir\\sub\\file.txt") == "dir/sub/file.txt");

    // IsValidFilename
    assert(!IsValidFilename(""));
    assert(!IsValidFilename(".."));
    assert(!IsValidFilename("../test.txt"));
    assert(!IsValidFilename("test/../test.txt"));
    assert(!IsValidFilename("bad:name"));
    assert(!IsValidFilename("bad*name"));
    assert(!IsValidFilename("bad?name"));
    assert(!IsValidFilename("bad\"name"));
    assert(!IsValidFilename("bad<name"));
    assert(!IsValidFilename("bad>name"));
    assert(!IsValidFilename("bad|name"));
    assert(!IsValidFilename(std::string("null\0byte", 9)));
    assert(!IsValidFilename(std::string(MAX_FILENAME_LENGTH + 1, 'a')));
    assert(IsValidFilename("valid_file-123.dat"));
    assert(IsValidFilename("folder/subfolder/file.ext"));
    assert(IsValidFilename(".dotfile"));
    assert(IsValidFilename("name.with.many.dots.txt"));

    // ValidateEntry
    PakEntry validEntry("valid.txt", 100, 50, 50);
    assert(ValidateEntry(validEntry, 1000));

    // Entry offset beyond file size
    PakEntry outOfBoundsOffset("valid.txt", 1001, 50, 50);
    assert(!ValidateEntry(outOfBoundsOffset, 1000));

    // Disk size beyond file size
    PakEntry outOfBoundsSize("valid.txt", 100, 1500, 1500);
    assert(!ValidateEntry(outOfBoundsSize, 1000));

    // Offset + disk size exceeds file size
    PakEntry exceedsSum("valid.txt", 800, 300, 300);
    assert(!ValidateEntry(exceedsSum, 1000));

    // Integer overflow in offset + diskSize
    PakEntry overflowEntry("valid.txt", UINT64_MAX - 10, 50, 50);
    assert(!ValidateEntry(overflowEntry, 1000));

    // Invalid filename in entry
    PakEntry badFilenameEntry("invalid:name.txt", 100, 50, 50);
    assert(!ValidateEntry(badFilenameEntry, 1000));

    // DecompressBuffer error paths and uncompressed defensive fallback
    std::vector<uint8_t> bogusData = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    std::vector<uint8_t> decompressed(64);
    assert(DecompressBuffer(PAK_FLAG_LZ4_COMPRESSED, bogusData.data(), bogusData.size(),
                            decompressed.data(), decompressed.size()) == PakStatus::DecompressionFailed);
    assert(DecompressBuffer(PAK_FLAG_ZSTD_COMPRESSED, bogusData.data(), bogusData.size(),
                            decompressed.data(), decompressed.size()) == PakStatus::DecompressionFailed);
    // flags == 0 with mismatched sizes returns CorruptArchive
    assert(DecompressBuffer(0, bogusData.data(), bogusData.size(),
                            decompressed.data(), decompressed.size()) == PakStatus::CorruptArchive);
    // flags == 0 with matching sizes acts as a raw copy
    std::vector<uint8_t> rawCopied(bogusData.size());
    assert(DecompressBuffer(0, bogusData.data(), bogusData.size(),
                            rawCopied.data(), rawCopied.size()) == PakStatus::Ok);
    assert(rawCopied == bogusData);
    // Size exceeding MAX_COMPRESSIBLE_ENTRY_SIZE returns CorruptArchive
    assert(DecompressBuffer(0, bogusData.data(), MAX_COMPRESSIBLE_ENTRY_SIZE + 1,
                            rawCopied.data(), rawCopied.size()) == PakStatus::CorruptArchive);

    // EncryptDecrypt corner cases
    std::vector<uint8_t> data = Bytes("quick brown fox");
    std::vector<uint8_t> copy = data;
    EncryptDecrypt(copy, "");
    assert(copy == data);

    std::vector<uint8_t> emptyVec;
    EncryptDecrypt(emptyVec, "key");
    assert(emptyVec.empty());

    EncryptDecrypt(copy, "k");
    assert(copy != data);
    EncryptDecrypt(copy, "k");
    assert(copy == data);

    EncryptDecrypt(copy, "long-key-longer-than-data-itself-1234567890");
    assert(copy != data);
    EncryptDecrypt(copy, "long-key-longer-than-data-itself-1234567890");
    assert(copy == data);

    // HashBytesFast
    assert(HashBytesFast(nullptr, 0) == HashBytesFast("", 0));
    assert(HashBytesFast("abc", 3) == HashBytesFast("abc", 3));
    assert(HashBytesFast("abc", 3) != HashBytesFast("abd", 3));
}

static void PakSpanMoveAndLifetimeTests()
{
    fs::path root = TestRoot();
    fs::path pakPath = root / "span_move.pak";

    std::map<std::string, std::vector<uint8_t>> files;
    files["raw.bin"] = Bytes("span-move-raw-content");
    files["compressed.bin"] = std::vector<uint8_t>(4096, 0x33);

    PakOptions options;
    options.compression = PakCompression::LZ4;
    Pakker pakker;
    assert(pakker.CreatePak(pakPath.string(), files, options));

    PakReader reader;
    assert(reader.Open(pakPath.string()));

    // Non-owning span move
    {
        PakSpan span1 = reader.ReadFileZeroCopy("raw.bin");
        assert(span1);
        const uint8_t* origData = span1.data;
        const uint64_t origSize = span1.size;

        PakSpan span2(std::move(span1));
        assert(!span1);
        assert(span2);
        assert(span2.data == origData);
        assert(span2.size == origSize);

        PakSpan span3;
        span3 = std::move(span2);
        assert(!span2);
        assert(span3);
        assert(span3.data == origData);
        assert(span3.size == origSize);
    }

    // Owning span move
    {
        PakSpan span1 = reader.ReadFileZeroCopy("compressed.bin");
        assert(span1);
        assert(span1.ownsData);
        const uint8_t* origData = span1.data;
        const uint64_t origSize = span1.size;

        PakSpan span2(std::move(span1));
        assert(!span1);
        assert(span2);
        assert(span2.data == origData);
        assert(span2.size == origSize);
        assert(span2.ownsData);

        PakSpan span3;
        span3 = std::move(span2);
        assert(!span2);
        assert(span3);
        assert(span3.data == origData);
        assert(span3.ownsData);
    }
}


int main()
{
    RuntimeHandleApi();
    ExtensionDoesNotBlockCompression();
    OldVersionRejected();
    CorruptArchiveFailsOpen();
    EmptyArchiveOpens();
    MemoryCacheStoresDecodedEntries();
    MemoryCacheEvictsLeastRecentlyUsedEntry();
    PersistentCacheSurvivesReaderReopen();
    PersistentCacheInvalidatesWhenSourceChanges();
    ZeroCopyFallbackSpanKeepsCachedDataAlive();
    ContentHashStoredAndRoundTrips();
    ValidatePakDeepVerifyCatchesCorruptedCompressedEntry();
    ValidatePakDeepVerifyCatchesCorruptedUncompressedEntry();
    ValidatePakDeepVerifyPassesOnCleanArchive();
    VerifyEntryDetectsCorruption();
    VerifyEntryPropagatesInvalidHandleAndNotOpen();
    VerifyOnReadModeFailsClosedOnMismatch();
    VerifyOnReadBypassesDecodedCache();
    VerifyOnReadModeOffByDefaultAllowsCorruptedReadThrough();
    MountTwoLayersOverrideAndFallback();
    MountReaderSharesExternallyOwnedReader();
    MountReaderRejectsUnopenedReader();
    MountThreeLayerPriorityAndLayerAccessors();
    MountClearRemovesAllLayers();
    MountEnumerationDeduplicatesAcrossLayers();
    MountConcurrentReadsAcrossLayers();
    ZstdCompressionRoundTrips();
    MixedLz4AndZstdEntriesInSameArchive();
    ValidatePakDeepVerifyCatchesCorruptedZstdEntry();
    VerifyOnReadModeFailsClosedOnZstdMismatch();
    ReaderListFilesEnumeratesAllEntries();
    ReaderListFilesWithPrefixFilters();
    ReadRangeUncompressedMatchesFullReadSlice();
    ReadRangeCompressedReturnsUnsupported();
    ReadRangeEncryptedMatchesFullDecryptSlice();
    ReadRangeOutOfBoundsRejected();
    LooseOverlayPrefersLooseFileOverArchive();
    LooseOverlayFallsBackToWrappedReader();
    LooseOverlayFileExistsChecksBoth();
    LooseOverlayRejectsPathTraversal();
    FindByHashMatchesFindByName();
    FindNormalizesBeforeHashing();
    OpenWithoutNamesStillReadsByHash();
    MountComposesLayersOpenedWithoutNames();
    PathHashIsStableAndCaseSensitive();
    ParallelBuildMatchesSequentialByteForByte();
    ParallelFolderBuildMatchesSequential();
    ChunkedEntriesRoundTripAndReportChunkSize();
    ChunkedReadRangeMatchesFullRead();
    UnchunkedCompressedReadRangeStillUnsupported();
    ChunkedEncryptedEntriesRoundTrip();
    ChunkedArchiveRejectsCorruptChunkTable();
    ReadBatchMatchesIndividualReads();
    ReadBatchReportsPerRequestFailures();
    ReadBatchIsSafeFromMultipleThreads();
    PakkerToolingAndExtraction();
    PakkerEncryptedWorkflow();
    PakkerValidationAndErrorHandling();
    PakReaderComprehensiveApi();
    PakReaderCacheManagement();
    PakMountComprehensiveApi();
    PakLooseOverlayComprehensiveApi();
    PakPlatformDirectTests();
    LoggingAndDiagnostics();
    InternalUtilitiesRobustness();
    PakSpanMoveAndLifetimeTests();
    return 0;
}
