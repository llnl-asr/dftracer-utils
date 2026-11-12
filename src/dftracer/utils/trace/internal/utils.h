// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INTERNAL_UTILS_H
#define DFTRACER_UTILS_TRACE_INTERNAL_UTILS_H

#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dftracer::utils::trace::internal {

// Appends `v` as a stream with precision 17 would print it: integers in
// decimal, floating point as %.17g, a character as itself, anything else as
// text.
template <class T>
void append_value(dstd::String& out, const T& v) {
    if constexpr (std::is_floating_point_v<T>) {
        char buf[40];
        char* const end = dstd::to_chars_general(buf, buf + sizeof(buf),
                                                 static_cast<double>(v), 17);
        if (end) out.append(buf, end);
    } else if constexpr (std::is_same_v<T, char>) {
        out.push_back(v);
    } else if constexpr (std::is_integral_v<T>) {
        char buf[40];
        const auto r = dstd::to_chars(buf, buf + sizeof(buf), v);
        out.append(buf, r.ptr);
    } else {
        out.append(v);
    }
}

// Canonical POSIX/STDIO operation name groups.
// Used by is_data_transfer_op() and by query builders (name_in_query).
namespace posix_ops {

// File descriptor data-transfer ops; excludes socket/network ops.
constexpr dstd::StringView FILE_READ[] = {
    "read", "pread", "pread64", "readv", "preadv", "preadv2", "fread",
};
constexpr dstd::StringView FILE_WRITE[] = {
    "write", "pwrite", "pwrite64", "writev", "pwritev", "pwritev2", "fwrite",
};

// Full data-transfer ops including socket/network ops.
constexpr dstd::StringView READ[] = {
    "read",    "pread", "pread64", "readv",    "preadv",
    "preadv2", "fread", "recv",    "recvfrom", "recvmsg",
};
constexpr dstd::StringView WRITE[] = {
    "write",           "pwrite", "pwrite64", "writev",  "pwritev", "pwritev2",
    "fwrite",          "send",   "sendto",   "sendmsg", "splice",  "sendfile",
    "copy_file_range",
};
constexpr dstd::StringView METADATA[] = {
    "__fxstat",  "__fxstat64", "__lxstat", "__lxstat64", "__xstat",
    "__xstat64", "access",     "close",    "closedir",   "fclose",
    "fcntl",     "fopen",      "fopen64",  "fseek",      "fseeko",
    "fseeko64",  "fstat",      "fstat64",  "fstatat",    "fstatat64",
    "ftell",     "ftello",     "ftello64", "ftruncate",  "ftruncate64",
    "link",      "lseek",      "lseek64",  "mkdir",      "open",
    "open64",    "opendir",    "readdir",  "readdir64",  "readlink",
    "remove",    "rename",     "rmdir",    "seek",       "stat",
    "stat64",    "unlink",
};
constexpr dstd::StringView SYNC[] = {
    "fsync",
    "fdatasync",
    "msync",
    "sync",
};
constexpr dstd::StringView PCTL[] = {
    "exec", "exit", "fork", "kill", "pipe", "wait",
};
constexpr dstd::StringView IPC[] = {
    "msgctl", "msgget", "msgrcv", "msgsnd", "semctl", "semget",
    "semop",  "shmat",  "shmctl", "shmdt",  "shmget",
};

// Build a DSL query fragment: name in ["op1", "op2", ...]
template <std::size_t N>
inline dstd::String name_in_query(const dstd::StringView (&ops)[N]) {
    dstd::String q = "name in [";
    for (std::size_t i = 0; i < N; ++i) {
        if (i > 0) q += ", ";
        q += '"';
        q.append(ops[i]);
        q += '"';
    }
    q += ']';
    return q;
}

}  // namespace posix_ops

// True if `name` is a POSIX/STDIO data-transfer op (its `ret` is a byte count).
inline bool is_read_or_write(dstd::StringView name) {
    for (auto op : posix_ops::READ)
        if (op == name) return true;
    for (auto op : posix_ops::WRITE)
        if (op == name) return true;
    return false;
}

template <std::size_t N>
inline bool name_in(const dstd::StringView (&ops)[N], dstd::StringView name) {
    for (auto op : ops)
        if (op == name) return true;
    return false;
}

// dfanalyzer I/O category of a function, used as a group-by dimension. READ and
// WRITE are file-descriptor transfers only (socket ops fall through to OTHER).
// Int values match dfanalyzer's IOCategory enum.
enum class IOCategory : std::int8_t {
    Read = 1,
    Write = 2,
    Metadata = 3,
    Pctl = 4,
    Ipc = 5,
    Other = 6,
    Sync = 7,
};

inline IOCategory io_category(dstd::StringView name) {
    if (name_in(posix_ops::FILE_READ, name)) return IOCategory::Read;
    if (name_in(posix_ops::FILE_WRITE, name)) return IOCategory::Write;
    if (name_in(posix_ops::SYNC, name)) return IOCategory::Sync;
    if (name_in(posix_ops::METADATA, name)) return IOCategory::Metadata;
    if (name_in(posix_ops::PCTL, name)) return IOCategory::Pctl;
    if (name_in(posix_ops::IPC, name)) return IOCategory::Ipc;
    return IOCategory::Other;
}

// The byte-size metric for one event, matching the reader's normalize rule so
// the reader and the aggregator cannot drift: size_sum, else (posix/stdio
// read|write) ret, else image_size (non-open). Args are the raw values
// (nullopt if absent); returns nullopt when the event carries no size.
inline dstd::Optional<std::int64_t> derive_io_size(
    dstd::StringView cat, dstd::StringView name,
    dstd::Optional<std::int64_t> size_sum, dstd::Optional<std::int64_t> ret,
    dstd::Optional<std::int64_t> image_size) {
    if (size_sum) return size_sum;
    if (dstd::ascii_iequals(cat, "posix") ||
        dstd::ascii_iequals(cat, "stdio")) {
        if (ret && *ret > 0 && is_read_or_write(name)) return ret;
        return dstd::nullopt;
    }
    if (image_size && *image_size > 0) {
        bool has_open = false;  // case-insensitive substring "open"
        for (std::size_t i = 0; i + 4 <= name.size(); ++i) {
            if (dstd::ascii_iequals(name.substr(i, 4), "open")) {
                has_open = true;
                break;
            }
        }
        if (!has_open) return image_size;
    }
    return dstd::nullopt;
}

// Lowercase `s`; returns a view over `s` when already lowercase (no copy),
// else lowercases into `storage` (which must outlive the returned view).
dstd::StringView to_lower_ascii(dstd::StringView s, dstd::String& storage);

// True when the event's return value represents bytes transferred.
bool is_data_transfer_op(dstd::StringView cat, dstd::StringView name);

/**
 * @brief Determine the root-local index path for a given input path.
 *
 * When a custom index directory is provided, the index root is
 * `<index_dir>/.dftindex`. Otherwise, the index root is placed alongside the
 * input path:
 * - file path: `<file_dir>/.dftindex`
 * - directory path: `<directory>/.dftindex`
 *
 * @param path Path to a data file or directory
 * @param index_dir Optional custom directory for the index root.
 * @return Path to the owning `.dftindex` directory.
 */
dstd::String determine_index_path(dstd::CStringView path,
                                  dstd::StringView index_dir = {});

}  // namespace dftracer::utils::trace::internal

#endif  // DFTRACER_UTILS_TRACE_INTERNAL_UTILS_H
