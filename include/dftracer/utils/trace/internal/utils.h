#ifndef DFTRACER_UTILS_TRACE_INTERNAL_UTILS_H
#define DFTRACER_UTILS_TRACE_INTERNAL_UTILS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dftracer::utils::trace::internal {

// Canonical POSIX/STDIO operation name groups.
// Used by is_data_transfer_op() and by query builders (name_in_query).
namespace posix_ops {

// File descriptor data-transfer ops; excludes socket/network ops.
constexpr std::string_view FILE_READ[] = {
    "read", "pread", "pread64", "readv", "preadv", "preadv2", "fread",
};
constexpr std::string_view FILE_WRITE[] = {
    "write", "pwrite", "pwrite64", "writev", "pwritev", "pwritev2", "fwrite",
};

// Full data-transfer ops including socket/network ops.
constexpr std::string_view READ[] = {
    "read",    "pread", "pread64", "readv",    "preadv",
    "preadv2", "fread", "recv",    "recvfrom", "recvmsg",
};
constexpr std::string_view WRITE[] = {
    "write",           "pwrite", "pwrite64", "writev",  "pwritev", "pwritev2",
    "fwrite",          "send",   "sendto",   "sendmsg", "splice",  "sendfile",
    "copy_file_range",
};
constexpr std::string_view METADATA[] = {
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
constexpr std::string_view SYNC[] = {
    "fsync",
    "fdatasync",
    "msync",
    "sync",
};
constexpr std::string_view PCTL[] = {
    "exec", "exit", "fork", "kill", "pipe", "wait",
};
constexpr std::string_view IPC[] = {
    "msgctl", "msgget", "msgrcv", "msgsnd", "semctl", "semget",
    "semop",  "shmat",  "shmctl", "shmdt",  "shmget",
};

// Build a DSL query fragment: name in ["op1", "op2", ...]
template <std::size_t N>
inline std::string name_in_query(const std::string_view (&ops)[N]) {
    std::string q = "name in [";
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
inline bool is_read_or_write(std::string_view name) {
    for (auto op : posix_ops::READ)
        if (op == name) return true;
    for (auto op : posix_ops::WRITE)
        if (op == name) return true;
    return false;
}

template <std::size_t N>
inline bool name_in(const std::string_view (&ops)[N], std::string_view name) {
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

inline IOCategory io_category(std::string_view name) {
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
inline std::optional<std::int64_t> derive_io_size(
    std::string_view cat, std::string_view name,
    std::optional<std::int64_t> size_sum, std::optional<std::int64_t> ret,
    std::optional<std::int64_t> image_size) {
    if (size_sum) return size_sum;
    auto ieq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            char x = a[i], y = b[i];
            if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
            if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
            if (x != y) return false;
        }
        return true;
    };
    if (ieq(cat, "posix") || ieq(cat, "stdio")) {
        if (ret && *ret > 0 && is_read_or_write(name)) return ret;
        return std::nullopt;
    }
    if (image_size && *image_size > 0) {
        bool has_open = false;  // case-insensitive substring "open"
        for (std::size_t i = 0; i + 4 <= name.size(); ++i) {
            auto lc = [](char c) {
                return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
            };
            if (lc(name[i]) == 'o' && lc(name[i + 1]) == 'p' &&
                lc(name[i + 2]) == 'e' && lc(name[i + 3]) == 'n') {
                has_open = true;
                break;
            }
        }
        if (!has_open) return image_size;
    }
    return std::nullopt;
}

// Lowercase `s`; returns a view over `s` when already lowercase (no copy),
// else lowercases into `storage` (which must outlive the returned view).
std::string_view to_lower_ascii(std::string_view s, std::string& storage);

bool ascii_iequals(std::string_view a, std::string_view b);

// True when the event's return value represents bytes transferred.
bool is_data_transfer_op(std::string_view cat, std::string_view name);

/**
 * @brief Determine the root-local RocksDB index path for a given input path.
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
std::string determine_index_path(const std::string& path,
                                 const std::string& index_dir = "");

}  // namespace dftracer::utils::trace::internal

#endif  // DFTRACER_UTILS_TRACE_INTERNAL_UTILS_H
