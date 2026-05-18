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

#include <string_view>

namespace dftracer::utils::utilities::indexer::internal {

std::string get_logical_path(std::string_view path) {
    auto fs_path = fs::path(std::string(path));
    return fs_path.filename().string();
}

std::string normalize_index_root(std::string_view path) {
    fs::path input{std::string(path)};
    if (input.filename() == ".dftindex") {
        return input.string();
    }
    if (input.parent_path().filename() == ".dftindex") {
        return input.parent_path().string();
    }
    if (input.extension() == ".idx" || input.extension() == ".pidx" ||
        input.has_extension()) {
        return (input.parent_path() / ".dftindex").string();
    }
    return (input / ".dftindex").string();
}

time_t get_file_modification_time(const std::string &file_path) {
#if defined(DFTRACER_UTILS_USE_STD_FS)
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
    // Fast fingerprint: hash file_size + first 64KB + last 64KB.
    constexpr std::size_t SAMPLE_SIZE = 64 * 1024;

    int fd = ::open(file_path.c_str(), O_RDONLY);
    if (fd < 0) {
        DFTRACER_UTILS_LOG_ERROR("Cannot open file for hash calculation: %s",
                                 file_path.c_str());
        return 0;
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return 0;
    }
    auto file_size = static_cast<std::size_t>(st.st_size);

    static thread_local dftracer::utils::utilities::hash::HasherUtility hasher;
    hasher.reset();

    // Hash the file size itself
    char size_buf[32];
    int n = std::snprintf(size_buf, sizeof(size_buf), "%zu", file_size);
    hasher.update(std::string_view(size_buf, static_cast<std::size_t>(n)));

    unsigned char buf[SAMPLE_SIZE];

    // Hash first SAMPLE_SIZE bytes
    std::size_t head_bytes = std::min(file_size, SAMPLE_SIZE);
    ssize_t rd = ::pread(fd, buf, head_bytes, 0);
    if (rd > 0) {
        hasher.update(std::string_view(reinterpret_cast<const char *>(buf),
                                       static_cast<std::size_t>(rd)));
    }

    // Hash last SAMPLE_SIZE bytes, only if file is large enough such that they
    // don't overlap with the head
    if (file_size > SAMPLE_SIZE) {
        off_t tail_off =
            static_cast<off_t>(file_size - std::min(file_size, SAMPLE_SIZE));
        rd = ::pread(fd, buf, SAMPLE_SIZE, tail_off);
        if (rd > 0) {
            hasher.update(std::string_view(reinterpret_cast<const char *>(buf),
                                           static_cast<std::size_t>(rd)));
        }
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
}

bool index_exists_and_valid(const std::string &index_path) {
    return fs::exists(index_path) && fs::is_directory(index_path);
}

}  // namespace dftracer::utils::utilities::indexer::internal
