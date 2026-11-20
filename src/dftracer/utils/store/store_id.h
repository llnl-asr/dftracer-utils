// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_STORE_STORE_ID_H
#define DFTRACER_UTILS_STORE_STORE_ID_H

#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::store::detail {

/// The file in a store directory that holds the id of the store, 16 hex
/// digits. It is made once, with O_EXCL, by the process that creates the
/// directory or by the first writer of an older store, and no process removes
/// it. The id names the marker `id-<id>.mark`, which a store handle stats to
/// know that the directory is still the one it opened. A directory made again
/// gets a new id.
inline constexpr dstd::StringView STORE_ID_FILE = "store.id";
inline constexpr std::size_t STORE_ID_DIGITS = 16;

dstd::String marker_file_name(std::uint64_t id);

/// True for an error that says the path is not there: a missing entry or a
/// path component that is not a directory. Any other failure, such as ESTALE,
/// EIO, EACCES or a timeout on a network file system, says nothing about
/// whether the directory was replaced.
bool path_is_gone(const dstd::Error& error);

enum class IdFile : std::uint8_t {
    /// There is no store.id.
    Missing,
    /// The file could not be read for a reason other than its absence.
    Unreadable,
    /// The content is not an id, and no creator is writing it.
    Invalid,
    Found
};

struct StoreIdRead {
    IdFile state = IdFile::Missing;
    std::uint64_t id = 0;
};

/// Reads store.id of `dir`. A file that holds the start of an id is one a
/// creator is still writing, so the read waits for it, with a growing pause,
/// until it is complete or a creator's time on this machine's clock has gone,
/// and then reports it Invalid. A file that holds anything else is Invalid at
/// once.
StoreIdRead read_store_id(const dstd::Path& dir);

/// The marker file of `id`, made when it is missing. False when it does not
/// exist after the call, as in a directory the caller cannot write.
bool ensure_marker_file(const dstd::Path& dir, std::uint64_t id);

/// The id of the store in `dir`, made first when the directory has none. Every
/// caller of one directory gets the same id, also when they race. 0 when there
/// is none and the caller cannot make it, as for a reader without write
/// access. `seen`, when given, gets what the last read of store.id found.
std::uint64_t ensure_store_id(const dstd::Path& dir, IdFile* seen = nullptr);

/// The id of the store in `dir` when it has one, with its marker file made if
/// missing and possible. 0 otherwise. Never makes the id. `seen` as for
/// @ref ensure_store_id.
std::uint64_t existing_store_id(const dstd::Path& dir, IdFile* seen = nullptr);

}  // namespace dftracer::utils::store::detail

#endif  // DFTRACER_UTILS_STORE_STORE_ID_H
