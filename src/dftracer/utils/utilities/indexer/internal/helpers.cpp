#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>

// Platform-specific includes for file stats
#ifdef _WIN32
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::indexer::internal {

std::string get_logical_path(const std::string &path) {
    auto fs_path = fs::path(path);
    return fs_path.filename().string();
}

time_t get_file_modification_time(const std::string &file_path) {
#if defined(DFTRACER_UTILS_USE_STD_FS)
    // Use std::filesystem when available and working
    auto ftime = fs::last_write_time(file_path);
    auto sctp =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - fs::file_time_type::clock::now() +
            std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(sctp);
#else
    // Fallback to platform-specific stat
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(file_path.c_str(), &st) == 0) {
        return st.st_mtime;
    }
#else
    struct stat st;
    if (stat(file_path.c_str(), &st) == 0) {
        return st.st_mtime;
    }
#endif
    return 0;
#endif
}

std::uint64_t calculate_file_hash(const std::string &file_path) {
    // Use much larger buffer for better I/O performance on large files
    constexpr size_t HASH_BUFFER_SIZE = 1024 * 1024;  // 1MB buffer

    int fd = ::open(file_path.c_str(), O_RDONLY);
    if (fd < 0) {
        DFTRACER_UTILS_LOG_ERROR("Cannot open file for hash calculation: %s",
                                 file_path.c_str());
        return 0;
    }

    dftracer::utils::utilities::hash::HasherUtility hasher;
    std::vector<unsigned char> buffer(HASH_BUFFER_SIZE);

    ssize_t bytes_read = 0;
    while ((bytes_read = ::read(fd, buffer.data(), buffer.size())) > 0) {
        std::string_view chunk(reinterpret_cast<const char *>(buffer.data()),
                               static_cast<std::size_t>(bytes_read));
        hasher.update(chunk);
    }
    ::close(fd);

    return static_cast<std::uint64_t>(hasher.get_hash().value);
}

std::uint64_t file_size_bytes(const std::string &path) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
#if defined(_WIN32)
        if ((st.st_mode & _S_IFREG) != 0)
            return static_cast<std::uint64_t>(st.st_size);
#else
        if (S_ISREG(st.st_mode)) return static_cast<std::uint64_t>(st.st_size);
#endif
    }

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    off_t pos = ::lseek(fd, 0, SEEK_END);
    ::close(fd);
    if (pos < 0) return 0;
    return static_cast<std::uint64_t>(pos);
    if (pos < 0) return 0;
    return static_cast<std::uint64_t>(pos);
}

bool index_exists_and_valid(const std::string &idx_path) {
    return fs::exists(idx_path) && fs::is_regular_file(idx_path);
}

}  // namespace dftracer::utils::utilities::indexer::internal
