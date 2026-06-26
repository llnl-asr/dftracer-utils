#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INTERNAL_UTILS_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INTERNAL_UTILS_H

#include <cstddef>
#include <string>
#include <string_view>

namespace dftracer::utils::utilities::composites::dft::internal {

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

/**
 * @brief Determine the provenance index file path for a given data file.
 *
 * Provenance now lives in the same root-local `.dftindex` database as
 * the regular index data.
 *
 * @param data_path Path to the data file
 * @param index_dir Optional directory. If empty, places next to data file.
 * @return Path to the owning `.dftindex` directory
 */
std::string determine_provenance_index_path(const std::string& data_path,
                                            const std::string& index_dir = "");

}  // namespace dftracer::utils::utilities::composites::dft::internal

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INTERNAL_UTILS_H
