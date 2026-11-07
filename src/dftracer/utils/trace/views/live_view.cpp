// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/live_view.h>
#include <dftracer/utils/utilities/fileio/compress/zng_stream.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib-ng.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

namespace dftracer::utils::trace::views {

namespace {

constexpr std::size_t CHUNK = std::size_t{1} << 20;
constexpr std::size_t HEAD = 64;
constexpr std::array<unsigned char, 3> MAGIC = {0x1f, 0x8b, 0x08};

[[noreturn]] void throw_io(dstd::StringView what, dstd::StringView path) {
    throw common::DFTUtilsException::cat(common::ErrorCode::Io, what, " ", path,
                                         ": ", std::strerror(errno));
}

[[noreturn]] void throw_error(const dstd::Error& error) {
    throw common::DFTUtilsException(error);
}

/// Reads at most `n` bytes at `off`; 0 at end of file.
std::size_t read_at(int fd, const dstd::String& path, unsigned char* buf,
                    std::size_t n, std::uint64_t off) {
    for (;;) {
        const ssize_t got = ::pread(fd, buf, n, static_cast<off_t>(off));
        if (got >= 0) return static_cast<std::size_t>(got);
        if (errno != EINTR) throw_io("cannot read", path);
    }
}

class Walker {
   public:
    Walker(int fd, dstd::String path, std::uint64_t size)
        : fd_(fd),
          path_(std::move(path)),
          size_(size),
          in_(dstd::make_unique_for_overwrite<unsigned char[]>(CHUNK)),
          out_(dstd::make_unique_for_overwrite<unsigned char[]>(CHUNK)) {}

    /// End offset of the whole gzip member at `offset`, CRC verified, or
    /// nullopt when it is undecodable or not fully written.
    dstd::Optional<std::uint64_t> member_end(std::uint64_t offset) {
        zng_stream zs{};
        // 31: a gzip wrapper, whose trailer inflate checks.
        if (utilities::fileio::compress::open_inflate_stream(zs, 31) != Z_OK) {
            throw common::DFTUtilsException(common::ErrorCode::Compression,
                                            "failed to allocate inflate state");
        }
        struct End {
            zng_stream* zs;
            ~End() { zng_inflateEnd(zs); }
        } end{&zs};
        std::uint64_t pos = offset;
        while (pos < size_) {
            const std::size_t got =
                read_at(fd_, path_, in_.get(),
                        static_cast<std::size_t>(
                            std::min<std::uint64_t>(CHUNK, size_ - pos)),
                        pos);
            if (got == 0) return dstd::nullopt;
            pos += got;
            zs.next_in = in_.get();
            zs.avail_in = static_cast<std::uint32_t>(got);
            do {
                zs.next_out = out_.get();
                zs.avail_out = static_cast<std::uint32_t>(CHUNK);
                const int rc = zng_inflate(&zs, Z_NO_FLUSH);
                if (rc == Z_STREAM_END) return pos - zs.avail_in;
                if (rc != Z_OK && rc != Z_BUF_ERROR) return dstd::nullopt;
            } while (zs.avail_in > 0 || zs.avail_out == 0);
        }
        return dstd::nullopt;
    }

    /// Offset of the first whole, decodable gzip member at or after `start`.
    dstd::Optional<std::uint64_t> next_member(std::uint64_t start) {
        const auto buf = dstd::make_unique_for_overwrite<unsigned char[]>(
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
            const dstd::StringView hay(reinterpret_cast<const char*>(buf.get()),
                                       held + got);
            const dstd::StringView magic(
                reinterpret_cast<const char*>(MAGIC.data()), MAGIC.size());
            const std::uint64_t base = pos - held;
            for (std::size_t i = hay.find(magic); i != dstd::StringView::npos;
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
        return dstd::nullopt;
    }

   private:
    int fd_;
    dstd::String path_;
    std::uint64_t size_;
    dstd::UniquePtr<unsigned char[]> in_;
    dstd::UniquePtr<unsigned char[]> out_;
};

/// Directories to remove at process exit.
class OwnedDirs {
   public:
    void add(dstd::String dir) {
        std::lock_guard<std::mutex> lock(mu_);
        dirs_.push_back(std::move(dir));
    }
    ~OwnedDirs() {
        for (const dstd::String& d : dirs_) (void)dstd::fs::remove_all(d);
    }

   private:
    std::mutex mu_;
    dstd::Vector<dstd::String> dirs_;
};

dstd::String make_private_dir() {
    static OwnedDirs owned;
    const dstd::Path path = dstd::fs::temp_directory() / "dftlive_XXXXXX";
    dstd::String dir(path.str());
    if (!::mkdtemp(dir.data())) throw_io("cannot create", dir);
    owned.add(dir);
    return dir;
}

dstd::Vector<dstd::String> trace_files_under(const dstd::String& dir) {
    runtime::fs::FindOptions options;
    options.patterns =
        runtime::fs::names_of(common::constants::indexer::INPUT_FILE_PATTERNS);
    options.with_stat = false;
    options.skip_unreadable = true;

    auto scoped = coro::with_scope([&](coro::Scope& scope) {
        return runtime::fs::find_all(scope, dstd::PathView(dir), options);
    });
    auto found = default_runtime().block_on(std::move(scoped));
    if (!found) throw_error(found.error());

    dstd::Vector<dstd::String> out;
    out.reserve(found->size());
    for (const auto& e : *found) out.emplace_back(e.path.str());
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

/// One live file and its append-only snapshot.
struct LiveView::Source {
    dstd::String path;
    dstd::String snapshot;
    std::uint64_t verified = 0;  // bytes of `path` copied and verified whole
    dstd::Vector<unsigned char> head;  // to detect a replaced file
    std::uint64_t skipped_tail = 0;
    dstd::Optional<LiveHole> hole;

    void update() {
        hole.reset();
        skipped_tail = 0;
        dstd::ScopedFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
        if (!fd.valid()) {
            if (errno == ENOENT) {
                throw common::DFTUtilsException::cat(
                    common::ErrorCode::NotFound, "no such trace file: ", path);
            }
            throw_io("cannot open", path);
        }
        const auto sized = dstd::fs::file_size(path);
        if (!sized) throw_error(sized.error());
        const std::uint64_t size = *sized;
        if (verified > 0 && replaced(fd.get(), size)) {
            // Dropped now: the new file may have no whole member yet.
            verified = 0;
            (void)dstd::fs::remove(snapshot);
        }
        Walker walker(fd.get(), path, size);
        std::uint64_t off = verified;
        while (off < size) {
            const dstd::Optional<std::uint64_t> end = walker.member_end(off);
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
        dstd::Vector<unsigned char> now(head.size());
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
        dstd::ScopedFd out(::open(snapshot.c_str(), flags, 0600));
        if (!out.valid()) throw_io("cannot write", snapshot);
        const auto buf =
            dstd::make_unique_for_overwrite<unsigned char[]>(CHUNK);
        for (std::uint64_t pos = begin; pos < end;) {
            const std::size_t got =
                read_at(fd, path, buf.get(),
                        static_cast<std::size_t>(
                            std::min<std::uint64_t>(CHUNK, end - pos)),
                        pos);
            if (got == 0) {
                throw common::DFTUtilsException::cat(
                    common::ErrorCode::Io, path, ": shrank while being read");
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

LiveView::LiveView(dstd::Vector<dstd::String> paths, dstd::String index_path,
                   dstd::String snapshot_dir)
    : paths_(std::move(paths)),
      index_path_(std::move(index_path)),
      snapshot_dir_(std::move(snapshot_dir)) {
    if (snapshot_dir_.empty()) {
        snapshot_dir_ = make_private_dir();
    } else {
        const auto made = dstd::fs::create_directories(snapshot_dir_);
        if (!made) throw_error(made.error());
    }
}

LiveView::~LiveView() = default;

View LiveView::refresh() {
    dstd::Set<dstd::String> known;
    for (const auto& s : sources_) known.insert(s->path);
    const dstd::String skip = snapshot_dir_ + "/";
    for (const dstd::String& p : paths_) {
        dstd::Vector<dstd::String> files;
        if (dstd::fs::is_directory(p).value_or(false)) {
            files = trace_files_under(p);
        } else {
            files.push_back(p);
        }
        for (dstd::String& f : files) {
            if (f.starts_with(skip) || !known.insert(f).second) continue;
            char prefix[16];
            std::snprintf(prefix, sizeof prefix, "%04zu_", sources_.size());
            auto src = dstd::make_unique<Source>();
            src->snapshot =
                dstd::String((dstd::Path(snapshot_dir_) /
                              (dstd::String(prefix) +
                               dstd::String(dstd::PathView(f).filename())))
                                 .str());
            src->path = std::move(f);
            sources_.push_back(std::move(src));
        }
    }
    dstd::Vector<ViewFile> files;
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

dstd::Vector<LiveHole> LiveView::holes() const {
    dstd::Vector<LiveHole> out;
    for (const auto& s : sources_) {
        if (s->hole) out.push_back(*s->hole);
    }
    return out;
}

}  // namespace dftracer::utils::trace::views
