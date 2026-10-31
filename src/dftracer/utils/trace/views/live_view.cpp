#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/exception_helpers.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/live_view.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace dftracer::utils::trace::views {

namespace {

constexpr std::size_t CHUNK = std::size_t{1} << 20;
constexpr std::size_t HEAD = 64;
constexpr std::array<unsigned char, 3> MAGIC = {0x1f, 0x8b, 0x08};

[[noreturn]] void throw_io(const std::string& what, const std::string& path) {
    throw DFTUtilsException::cat(ErrorCode::IO, what, " ", path, ": ",
                                 std::strerror(errno));
}

class Fd {
   public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0) ::close(fd_);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }

   private:
    int fd_;
};

/// Reads at most `n` bytes at `off`; 0 at end of file.
std::size_t read_at(int fd, const std::string& path, unsigned char* buf,
                    std::size_t n, std::uint64_t off) {
    for (;;) {
        const ssize_t got = ::pread(fd, buf, n, static_cast<off_t>(off));
        if (got >= 0) return static_cast<std::size_t>(got);
        if (errno != EINTR) throw_io("cannot read", path);
    }
}

class Walker {
   public:
    Walker(int fd, std::string path, std::uint64_t size)
        : fd_(fd),
          path_(std::move(path)),
          size_(size),
          in_(std::make_unique_for_overwrite<unsigned char[]>(CHUNK)),
          out_(std::make_unique_for_overwrite<unsigned char[]>(CHUNK)) {}

    /// End offset of the whole gzip member at `offset`, CRC verified, or
    /// nullopt when it is undecodable or not fully written.
    std::optional<std::uint64_t> member_end(std::uint64_t offset) {
        z_stream zs{};
        // 31: a gzip wrapper, whose trailer inflate checks.
        if (inflateInit2(&zs, 31) != Z_OK) {
            throw DFTUtilsException(ErrorCode::COMPRESSION,
                                    "failed to allocate inflate state");
        }
        struct End {
            z_stream* zs;
            ~End() { inflateEnd(zs); }
        } end{&zs};
        std::uint64_t pos = offset;
        while (pos < size_) {
            const std::size_t got =
                read_at(fd_, path_, in_.get(),
                        static_cast<std::size_t>(
                            std::min<std::uint64_t>(CHUNK, size_ - pos)),
                        pos);
            if (got == 0) return std::nullopt;
            pos += got;
            zs.next_in = in_.get();
            zs.avail_in = static_cast<uInt>(got);
            do {
                zs.next_out = out_.get();
                zs.avail_out = static_cast<uInt>(CHUNK);
                const int rc = inflate(&zs, Z_NO_FLUSH);
                if (rc == Z_STREAM_END) return pos - zs.avail_in;
                if (rc != Z_OK && rc != Z_BUF_ERROR) return std::nullopt;
            } while (zs.avail_in > 0 || zs.avail_out == 0);
        }
        return std::nullopt;
    }

    /// Offset of the first whole, decodable gzip member at or after `start`.
    std::optional<std::uint64_t> next_member(std::uint64_t start) {
        const auto buf = std::make_unique_for_overwrite<unsigned char[]>(
            CHUNK + MAGIC.size());
        std::size_t held = 0;
        std::uint64_t pos = start;
        while (pos < size_) {
            const std::size_t got =
                read_at(fd_, path_, buf.get() + held,
                        static_cast<std::size_t>(
                            std::min<std::uint64_t>(CHUNK, size_ - pos)),
                        pos);
            if (got == 0) break;
            const std::string_view hay(reinterpret_cast<const char*>(buf.get()),
                                       held + got);
            const std::string_view magic(
                reinterpret_cast<const char*>(MAGIC.data()), MAGIC.size());
            const std::uint64_t base = pos - held;
            for (std::size_t i = hay.find(magic); i != std::string_view::npos;
                 i = hay.find(magic, i + 1)) {
                if (member_end(base + i)) return base + i;
            }
            // A magic can straddle two reads.
            const std::size_t keep =
                std::min(hay.size(), MAGIC.size() - std::size_t{1});
            std::memmove(buf.get(), buf.get() + hay.size() - keep, keep);
            held = keep;
            pos += got;
        }
        return std::nullopt;
    }

   private:
    int fd_;
    std::string path_;
    std::uint64_t size_;
    std::unique_ptr<unsigned char[]> in_;
    std::unique_ptr<unsigned char[]> out_;
};

/// Directories to remove at process exit.
class OwnedDirs {
   public:
    void add(std::string dir) {
        std::lock_guard<std::mutex> lock(mu_);
        dirs_.push_back(std::move(dir));
    }
    ~OwnedDirs() {
        for (const std::string& d : dirs_) {
            std::error_code ec;
            fs::remove_all(d, ec);
        }
    }

   private:
    std::mutex mu_;
    std::vector<std::string> dirs_;
};

std::string make_private_dir() {
    static OwnedDirs owned;
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec) base = "/tmp";
    std::string dir = (base / "dftlive_XXXXXX").string();
    if (!::mkdtemp(dir.data())) throw_io("cannot create", dir);
    owned.add(dir);
    return dir;
}

std::vector<std::string> trace_files_under(const std::string& dir) {
    namespace ufs = utilities::filesystem;
    ufs::PatternDirectoryScannerUtility scanner;
    ufs::PatternDirectoryScannerUtilityInput input(dir,
                                                   ufs::trace_file_patterns(),
                                                   /*recursive=*/true,
                                                   /*populate_size=*/false);
    std::vector<ufs::FileEntry> entries;
    default_runtime().run_blocking(
        "live_view_scan", [&](CoroScope& scope) -> coro::CoroTask<void> {
            entries = co_await scanner(scope, input);
        });
    std::vector<std::string> out;
    out.reserve(entries.size());
    for (const auto& e : entries) {
        std::string path = e.path.string();
        if (path.ends_with(".gz")) out.push_back(std::move(path));
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

/// One live file and its append-only snapshot.
struct LiveView::Source {
    std::string path;
    std::string snapshot;
    std::uint64_t verified = 0;  // bytes of `path` copied and verified whole
    std::vector<unsigned char> head;  // to detect a replaced file
    std::uint64_t skipped_tail = 0;
    std::optional<LiveHole> hole;

    void update() {
        hole.reset();
        skipped_tail = 0;
        Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
        if (fd.get() < 0) {
            if (errno == ENOENT) {
                throw DFTUtilsException::cat(ErrorCode::NOT_FOUND,
                                             "no such trace file: ", path);
            }
            throw_io("cannot open", path);
        }
        const std::uint64_t size = fs::file_size(path);
        if (verified > 0 && replaced(fd.get(), size)) {
            // Dropped now: the new file may have no whole member yet.
            verified = 0;
            std::error_code ec;
            fs::remove(snapshot, ec);
        }
        Walker walker(fd.get(), path, size);
        std::uint64_t off = verified;
        while (off < size) {
            const std::optional<std::uint64_t> end = walker.member_end(off);
            if (!end) {
                if (const auto next = walker.next_member(off + 1)) {
                    hole = LiveHole{path, off, *next - off};
                    DFTRACER_UTILS_LOG_WARN(
                        "%s: hole of %llu bytes at offset %llu; reading the "
                        "members before it",
                        path.c_str(),
                        static_cast<unsigned long long>(*next - off),
                        static_cast<unsigned long long>(off));
                }
                skipped_tail = size - off;
                break;
            }
            off = *end;
        }
        if (off > verified) append(fd.get(), verified, off);
    }

   private:
    bool replaced(int fd, std::uint64_t size) const {
        if (size < verified) return true;
        std::vector<unsigned char> now(head.size());
        std::size_t have = 0;
        while (have < now.size()) {
            const std::size_t got =
                read_at(fd, path, now.data() + have, now.size() - have, have);
            if (got == 0) break;
            have += got;
        }
        return have != now.size() || now != head;
    }

    void append(int fd, std::uint64_t begin, std::uint64_t end) {
        const int flags =
            O_WRONLY | O_CREAT | O_CLOEXEC | (begin == 0 ? O_TRUNC : 0);
        Fd out(::open(snapshot.c_str(), flags, 0600));
        if (out.get() < 0) throw_io("cannot write", snapshot);
        const auto buf = std::make_unique_for_overwrite<unsigned char[]>(CHUNK);
        for (std::uint64_t pos = begin; pos < end;) {
            const std::size_t got =
                read_at(fd, path, buf.get(),
                        static_cast<std::size_t>(
                            std::min<std::uint64_t>(CHUNK, end - pos)),
                        pos);
            if (got == 0) {
                throw DFTUtilsException::cat(ErrorCode::IO, path,
                                             ": shrank while being read");
            }
            for (std::size_t done = 0; done < got;) {
                const ssize_t put =
                    ::pwrite(out.get(), buf.get() + done, got - done,
                             static_cast<off_t>(pos + done));
                if (put < 0) {
                    if (errno == EINTR) continue;
                    throw_io("cannot write", snapshot);
                }
                done += static_cast<std::size_t>(put);
            }
            pos += got;
        }
        if (begin == 0) {
            head.resize(HEAD);
            std::size_t have = 0;
            while (have < HEAD) {
                const std::size_t got =
                    read_at(fd, path, head.data() + have, HEAD - have, have);
                if (got == 0) break;
                have += got;
            }
            head.resize(have);
        }
        verified = end;
    }
};

LiveView::LiveView(std::vector<std::string> paths, std::string index_path,
                   std::string snapshot_dir)
    : paths_(std::move(paths)),
      index_path_(std::move(index_path)),
      snapshot_dir_(std::move(snapshot_dir)) {
    if (snapshot_dir_.empty()) {
        snapshot_dir_ = make_private_dir();
    } else {
        fs::create_directories(snapshot_dir_);
    }
}

LiveView::~LiveView() = default;

View LiveView::refresh() {
    std::set<std::string> known;
    for (const auto& s : sources_) known.insert(s->path);
    const std::string skip = snapshot_dir_ + "/";
    for (const std::string& p : paths_) {
        std::error_code ec;
        std::vector<std::string> files;
        if (fs::is_directory(p, ec)) {
            files = trace_files_under(p);
        } else {
            files.push_back(p);
        }
        for (std::string& f : files) {
            if (f.starts_with(skip) || !known.insert(f).second) continue;
            char prefix[16];
            std::snprintf(prefix, sizeof prefix, "%04zu_", sources_.size());
            auto src = std::make_unique<Source>();
            src->snapshot = (fs::path(snapshot_dir_) /
                             (prefix + fs::path(f).filename().string()))
                                .string();
            src->path = std::move(f);
            sources_.push_back(std::move(src));
        }
    }
    std::vector<ViewFile> files;
    files.reserve(sources_.size());
    for (const auto& s : sources_) {
        s->update();
        if (s->verified == 0) continue;  // nothing whole yet: no file to read
        files.push_back(ViewFile{s->snapshot, internal::determine_index_path(
                                                  s->snapshot, index_path_)});
    }
    return files.empty() ? View() : View::from_files(std::move(files));
}

std::uint64_t LiveView::verified_bytes() const {
    std::uint64_t n = 0;
    for (const auto& s : sources_) n += s->verified;
    return n;
}

std::uint64_t LiveView::skipped_tail_bytes() const {
    std::uint64_t n = 0;
    for (const auto& s : sources_) n += s->skipped_tail;
    return n;
}

std::vector<LiveHole> LiveView::holes() const {
    std::vector<LiveHole> out;
    for (const auto& s : sources_) {
        if (s->hole) out.push_back(*s->hole);
    }
    return out;
}

}  // namespace dftracer::utils::trace::views
