#ifndef PAK_LOOSE_OVERLAY_H
#define PAK_LOOSE_OVERLAY_H

#include "Pak.h"
#include <string>
#include <string_view>
#include <memory>

// ---------------------------------------------------------------------------
// PakLooseOverlay -- dev-only loose-file hot-reload override
//
// Wraps an already-open PakReader plus a loose directory on disk. Lookups
// check the loose directory first (by relative path) and serve raw bytes
// from disk when present, else fall back to the wrapped PakReader. This lets
// a developer edit an asset on disk and see the change without repacking.
//
// This is a DEV-ONLY convenience, not a shipping-path optimization: every
// lookup does a filesystem stat/exists check, which is fine for iteration
// but does not belong in a shipping hot path (unlike PakReader/PakMount,
// which are handle-based specifically to avoid this).
//
// Deliberately NOT handle-based: PakReader/PakMount use handles to avoid
// repeated string lookups on a hot path, but this class stats the
// filesystem on every call regardless (that is what makes hot-reload work --
// the loose/not-loose answer can change between calls as a file is saved),
// so a pre-resolved handle would just cache a stale answer.
//
// Deliberately has no View()/zero-copy equivalent: PakView's contract
// requires a live memory mapping to back it, which a loose file read from
// disk does not have. Faking that over a heap buffer would defeat the
// point of zero-copy, so this class only offers Read()/Load().
//
// Loose files are read raw: no compression, no encryption. If a loose file
// needs either, repack it into the archive instead.
//
// Thread safety: PakLooseOverlay has no mutable state after construction
// (the wrapped reader and loose directory are fixed for the object's
// lifetime), so all methods are safe to call concurrently from multiple
// threads, to the same extent the wrapped PakReader's own concurrent-read
// contract holds.
// ---------------------------------------------------------------------------

class PakLooseOverlay {
public:
    PakLooseOverlay(std::shared_ptr<PakReader> reader, std::string looseDirectory);

    // True if the file exists in the loose directory or the wrapped reader.
    bool FileExists(std::string_view filename) const;

    // Reads into a caller-provided buffer. Serves from the loose directory
    // when present (BufferTooSmall if destination is smaller than the file
    // on disk), else delegates to the wrapped reader's Read().
    PakStatus Read(std::string_view filename, std::span<uint8_t> destination,
                   uint64_t* bytesWritten = nullptr) const;

    // Reads into an owned, resized buffer. Serves from the loose directory
    // when present, else delegates to the wrapped reader's Load().
    PakStatus Load(std::string_view filename, std::vector<uint8_t>& outData) const;

    std::shared_ptr<PakReader> WrappedReader() const { return reader_; }
    const std::string& LooseDirectory() const { return looseDirectory_; }

private:
    // Resolves `filename` to a path inside looseDirectory_, rejecting
    // traversal outside it. Returns an empty string if the file doesn't
    // exist as a regular file in the loose directory, or if the resolved
    // path would escape it.
    std::string ResolveLooseFilePath(std::string_view filename) const;

    std::shared_ptr<PakReader> reader_;
    std::string looseDirectory_;
};

#endif // PAK_LOOSE_OVERLAY_H
