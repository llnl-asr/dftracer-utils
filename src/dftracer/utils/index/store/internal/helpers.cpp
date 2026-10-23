#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

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

namespace dftracer::utils::index::store::internal {

// Registry key: the canonical absolute path, so same-named files in different
// directories are distinct and the key locates the trace. Applied on both the
// write and read side so lookups stay consistent; relative inputs resolve
// against the cwd, so the index is not portable across an absolute-layout move.
std::string get_logical_path(std::string_view path) {
    std::error_code ec;
    fs::path abs = fs::absolute(fs::path(std::string(path)), ec);
    if (ec) return fs::path(std::string(path)).lexically_normal().string();
    return abs.lexically_normal().string();
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
    dftracer::utils::InputFileStat st;
    if (!dftracer::utils::stat_input(file_path, st)) return 0;
    return static_cast<time_t>(st.mtime);
}

std::uint64_t calculate_file_hash(const std::string &file_path) {
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
    const auto hash =
        calculate_file_hash(fd, static_cast<std::uint64_t>(st.st_size));
    ::close(fd);
    return hash;
}

std::uint64_t calculate_file_hash(int fd, std::uint64_t file_size_u64) {
    // Fast fingerprint: hash file_size + first 64KB + last 64KB.
    constexpr std::size_t SAMPLE_SIZE = 64 * 1024;
    const auto file_size = static_cast<std::size_t>(file_size_u64);

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

    return static_cast<std::uint64_t>(hasher.get_hash().value);
}

std::uint64_t file_size_bytes(const std::string &path) {
    dftracer::utils::InputFileStat st;
    if (dftracer::utils::stat_input(path, st) && st.is_regular_file)
        return st.size;

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    off_t pos = ::lseek(fd, 0, SEEK_END);
    ::close(fd);
    if (pos < 0) return 0;
    return static_cast<std::uint64_t>(pos);
}

}  // namespace dftracer::utils::index::store::internal
