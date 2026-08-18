#include "Pak.h"
#include "PakInternal.h"
#include "PakLooseOverlay.h"

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
        assert(header.version == PakInternal::PAK_VERSION_7);
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

    // v4 (no contentHash field), v5 (no Zstd flag bit) and v6
    // (variable-length file table) are all prior formats -- confirm each is
    // cleanly rejected now that v7 is the only accepted version, with the
    // same "rebuild from source" remediation for any of them.
    for (uint32_t oldVersion : {PakInternal::PAK_VERSION_4, PakInternal::PAK_VERSION_5,
                                PakInternal::PAK_VERSION_6}) {
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
    return 0;
}
