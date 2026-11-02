// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/vector.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace dftracer::utils::dstd::fs {

namespace {

constexpr std::uint64_t NANOSECONDS_PER_SECOND = 1000000000ULL;
constexpr std::size_t COPY_BUFFER_SIZE = 64 * 1024;

Condition condition_from_errno(int error) noexcept {
    if (error == ENOENT || error == ENOTDIR) return Condition::NotFound;
    if (error == EINVAL || error == ENAMETOOLONG) {
        return Condition::InvalidArgument;
    }
    return Condition::Io;
}

String make_message(StringView first, StringView second, StringView text) {
    String message(first);
    if (!second.empty()) {
        message.append(" -> ");
        message.append(second);
    }
    message.append(": ");
    message.append(text);
    return message;
}

Error errno_error(int error, StringView first, StringView second = {}) {
    return Error(condition_from_errno(error),
                 make_message(first, second, std::strerror(error)));
}

Error not_a_directory_error(PathView path) {
    return Error(Condition::Io,
                 make_message(path.str(), {}, "exists and is not a directory"));
}

Error invalid_error(StringView what) {
    return Error(Condition::InvalidArgument, what);
}

/// A path as a syscall needs it, '\0'-terminated. A PathView promises no
/// terminator, so short paths are copied into one stack buffer and long ones
/// into one dstd::String.
class PathCString {
   public:
    explicit PathCString(StringView path) noexcept {
        if (path.size() < PATH_MAX) {
            if (!path.empty()) std::memcpy(stack_, path.data(), path.size());
            stack_[path.size()] = '\0';
        } else {
            heap_.assign(path);
        }
    }

    const char* c_str() const noexcept {
        return heap_.empty() ? stack_ : heap_.c_str();
    }

   private:
    String heap_;
    char stack_[PATH_MAX];
};

FileKind kind_from_mode(std::uint32_t mode) noexcept {
    if (S_ISREG(mode)) return FileKind::File;
    if (S_ISDIR(mode)) return FileKind::Directory;
    if (S_ISLNK(mode)) return FileKind::Symlink;
    return FileKind::Other;
}

FileKind kind_from_dtype(unsigned char type) noexcept {
    switch (type) {
        case DT_REG:
            return FileKind::File;
        case DT_DIR:
            return FileKind::Directory;
        case DT_LNK:
            return FileKind::Symlink;
        case DT_UNKNOWN:
            return FileKind::Unknown;
        default:
            return FileKind::Other;
    }
}

std::uint64_t mtime_nanoseconds(const struct stat& info) noexcept {
#if defined(__APPLE__)
    const auto& time = info.st_mtimespec;
#else
    const auto& time = info.st_mtim;
#endif
    return static_cast<std::uint64_t>(time.tv_sec) * NANOSECONDS_PER_SECOND +
           static_cast<std::uint64_t>(time.tv_nsec);
}

FileStat make_file_stat(const struct stat& info) noexcept {
    FileStat out;
    out.size = static_cast<std::uint64_t>(info.st_size);
    out.mtime_ns = mtime_nanoseconds(info);
    out.dev = static_cast<std::uint64_t>(info.st_dev);
    out.ino = static_cast<std::uint64_t>(info.st_ino);
    out.mode = static_cast<std::uint32_t>(info.st_mode);
    out.kind = kind_from_mode(out.mode);
    return out;
}

/// The path without trailing separators, so lstat names a symlink itself and
/// not its target. A bare root keeps its separator.
StringView strip_trailing_separators(StringView path) noexcept {
    const std::size_t root = path_detail::root_length(path, HOST_PATH_STYLE);
    std::size_t end = path.size();
    while (end > root && end > 1 &&
           path_detail::is_separator(path[end - 1], HOST_PATH_STYLE)) {
        --end;
    }
    return path.substr(0, end);
}

Expected<Dir, Error> open_dir_nofollow(int parent, StringView name,
                                       StringView shown) {
    const PathCString relative(name);
    const int fd = ::openat(parent, relative.c_str(),
                            O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return Unexpected(errno_error(errno, shown));
    return Dir(ScopedFd(fd));
}

struct NamedEntry {
    String name;
    FileKind kind;
};

/// Reads every entry of dir into names and closes the listing, so a caller
/// that descends holds one descriptor per level.
Expected<void, Error> list_entries(const Dir& dir, StringView shown,
                                   Vector<NamedEntry>& out) {
    Expected<DirEntries, Error> entries = dir.entries();
    if (!entries) return Unexpected(entries.error());
    for (const DirEntry& entry : *entries) {
        out.push_back(NamedEntry{String(entry.name), entry.kind});
    }
    if (entries->error() != 0) {
        return Unexpected(errno_error(entries->error(), shown));
    }
    return {};
}

struct WalkState {
    FunctionRef<WalkAction(PathView, FileKind)> fn;
    String* path;
    std::uint64_t count = 0;
    bool stopped = false;
};

Expected<void, Error> walk_children(Dir& dir, WalkState& state) {
    const std::size_t parent_size = state.path->size();
    Vector<NamedEntry> entries;
    const Expected<void, Error> listed =
        list_entries(dir, StringView(*state.path), entries);
    if (!listed) return listed;
    for (const NamedEntry& entry : entries) {
        if (state.stopped) break;
        FileKind kind = entry.kind;
        if (kind == FileKind::Unknown) {
            const Expected<FileStat, Error> found = dir.lstat(entry.name);
            if (!found) {
                if (found.error().is(Condition::NotFound)) continue;
                return Unexpected(found.error());
            }
            kind = found->kind;
        }
        state.path->push_back(PATH_SEPARATOR);
        state.path->append(entry.name);
        ++state.count;
        const WalkAction action = state.fn(PathView(*state.path), kind);
        if (action == WalkAction::Stop) {
            state.stopped = true;
        } else if (kind == FileKind::Directory &&
                   action != WalkAction::SkipDir) {
            Expected<Dir, Error> child = open_dir_nofollow(
                dir.fd(), entry.name, StringView(*state.path));
            if (!child) return Unexpected(child.error());
            const Expected<void, Error> below = walk_children(*child, state);
            if (!below) return below;
        }
        state.path->resize(parent_size);
    }
    return {};
}

/// Removes everything below dir by name relative to the held descriptor, so a
/// subdirectory swapped for a symlink during the run is never followed.
Expected<std::uint64_t, Error> remove_children(Dir& dir, String& shown) {
    Vector<NamedEntry> entries;
    const Expected<void, Error> listed =
        list_entries(dir, StringView(shown), entries);
    if (!listed) return Unexpected(listed.error());
    std::uint64_t count = 0;
    const std::size_t parent_size = shown.size();
    for (const NamedEntry& entry : entries) {
        shown.push_back(PATH_SEPARATOR);
        shown.append(entry.name);
        const PathCString relative{StringView(entry.name)};
        struct stat info;
        if (::fstatat(dir.fd(), relative.c_str(), &info, AT_SYMLINK_NOFOLLOW) !=
            0) {
            const int error = errno;
            if (error == ENOENT) {
                shown.resize(parent_size);
                continue;
            }
            return Unexpected(errno_error(error, StringView(shown)));
        }
        if (S_ISDIR(info.st_mode)) {
            Expected<Dir, Error> child =
                open_dir_nofollow(dir.fd(), entry.name, StringView(shown));
            if (!child) return Unexpected(child.error());
            const Expected<std::uint64_t, Error> below =
                remove_children(*child, shown);
            if (!below) return below;
            count += *below;
            if (::unlinkat(dir.fd(), relative.c_str(), AT_REMOVEDIR) != 0 &&
                errno != ENOENT) {
                return Unexpected(errno_error(errno, StringView(shown)));
            }
        } else if (::unlinkat(dir.fd(), relative.c_str(), 0) != 0 &&
                   errno != ENOENT) {
            return Unexpected(errno_error(errno, StringView(shown)));
        }
        ++count;
        shown.resize(parent_size);
    }
    return count;
}

}  // namespace

Expected<FileStat, Error> stat(PathView path) {
    const PathCString name(path.str());
    struct stat info;
    if (::stat(name.c_str(), &info) != 0) {
        return Unexpected(errno_error(errno, path.str()));
    }
    return make_file_stat(info);
}

Expected<FileStat, Error> lstat(PathView path) {
    const PathCString name(path.str());
    struct stat info;
    if (::lstat(name.c_str(), &info) != 0) {
        return Unexpected(errno_error(errno, path.str()));
    }
    return make_file_stat(info);
}

Expected<bool, Error> exists(PathView path) {
    const PathCString name(path.str());
    struct stat info;
    if (::stat(name.c_str(), &info) == 0) return true;
    const int error = errno;
    if (error == ENOENT || error == ENOTDIR) return false;
    return Unexpected(errno_error(error, path.str()));
}

Expected<bool, Error> create_directory(PathView path) {
    const PathCString name(path.str());
    if (::mkdir(name.c_str(), 0777) == 0) return true;
    const int error = errno;
    if (error != EEXIST) return Unexpected(errno_error(error, path.str()));
    const Expected<FileStat, Error> found = stat(path);
    if (!found) return Unexpected(found.error());
    if (found->kind != FileKind::Directory) {
        return Unexpected(not_a_directory_error(path));
    }
    return false;
}

Expected<bool, Error> create_directories(PathView path) {
    const StringView bytes = path.str();
    if (bytes.empty()) {
        return Unexpected(invalid_error("empty path, no directory to create"));
    }
    const std::size_t root = path_detail::root_length(bytes, HOST_PATH_STYLE);
    std::size_t end = bytes.size();
    while (end > root &&
           path_detail::is_separator(bytes[end - 1], HOST_PATH_STYLE)) {
        --end;
    }
    bool created = false;
    for (std::size_t at = root + 1; at <= end; ++at) {
        if (at < end &&
            !path_detail::is_separator(bytes[at], HOST_PATH_STYLE)) {
            continue;
        }
        const StringView prefix = bytes.substr(0, at);
        if (prefix.size() <= root) continue;
        const Expected<bool, Error> made = create_directory(PathView(prefix));
        if (!made) return Unexpected(made.error());
        created = created || *made;
    }
    return created;
}

Expected<bool, Error> remove(PathView path) {
    const StringView bytes = strip_trailing_separators(path.str());
    const Expected<FileStat, Error> found = lstat(PathView(bytes));
    if (!found) {
        if (found.error().is(Condition::NotFound)) return false;
        return Unexpected(found.error());
    }
    const PathCString name(bytes);
    const int failed = found->kind == FileKind::Directory
                           ? ::rmdir(name.c_str())
                           : ::unlink(name.c_str());
    if (failed == 0) return true;
    const int error = errno;
    if (error == ENOENT || error == ENOTDIR) return false;
    return Unexpected(errno_error(error, path.str()));
}

Expected<std::uint64_t, Error> remove_all(PathView path) {
    const StringView bytes = strip_trailing_separators(path.str());
    const Expected<FileStat, Error> found = lstat(PathView(bytes));
    if (!found) {
        if (found.error().is(Condition::NotFound)) return std::uint64_t{0};
        return Unexpected(found.error());
    }
    if (found->kind != FileKind::Directory) {
        const Expected<bool, Error> removed = remove(PathView(bytes));
        if (!removed) return Unexpected(removed.error());
        return static_cast<std::uint64_t>(*removed);
    }
    Expected<Dir, Error> dir = open_dir_nofollow(AT_FDCWD, bytes, bytes);
    if (!dir) {
        if (dir.error().is(Condition::NotFound)) return std::uint64_t{0};
        return Unexpected(dir.error());
    }
    String shown(bytes);
    const Expected<std::uint64_t, Error> below = remove_children(*dir, shown);
    if (!below) return below;
    const PathCString name(bytes);
    if (::rmdir(name.c_str()) != 0) {
        const int error = errno;
        if (error == ENOENT) return *below;
        return Unexpected(errno_error(error, bytes));
    }
    return *below + 1;
}

Expected<void, Error> rename(PathView from, PathView to) {
    const PathCString source(from.str());
    const PathCString target(to.str());
    if (::rename(source.c_str(), target.c_str()) != 0) {
        return Unexpected(errno_error(errno, from.str(), to.str()));
    }
    return {};
}

Expected<void, Error> copy_file(PathView from, PathView to, CopyMode mode) {
    const PathCString source(from.str());
    const PathCString target(to.str());
    ScopedFd in(::open(source.c_str(), O_RDONLY | O_CLOEXEC));
    if (!in.valid()) return Unexpected(errno_error(errno, from.str()));
    constexpr int WRITE_FLAGS = O_WRONLY | O_CLOEXEC;
    bool created = true;
    ScopedFd out;
    if (mode == CopyMode::FailIfExists) {
        out.reset(::open(target.c_str(), WRITE_FLAGS | O_CREAT | O_EXCL, 0644));
    } else {
        out.reset(::open(target.c_str(), WRITE_FLAGS));
        created = false;
        if (!out.valid() && errno == ENOENT) {
            out.reset(
                ::open(target.c_str(), WRITE_FLAGS | O_CREAT | O_EXCL, 0644));
            created = true;
        }
    }
    if (!out.valid()) return Unexpected(errno_error(errno, to.str()));
    const auto fail = [&](Error error) -> Expected<void, Error> {
        if (created) ::unlink(target.c_str());
        return Unexpected(std::move(error));
    };
    struct stat in_info;
    struct stat out_info;
    if (::fstat(in.get(), &in_info) != 0) {
        return fail(errno_error(errno, from.str()));
    }
    if (::fstat(out.get(), &out_info) != 0) {
        return fail(errno_error(errno, to.str()));
    }
    if (in_info.st_dev == out_info.st_dev &&
        in_info.st_ino == out_info.st_ino) {
        return fail(invalid_error("source and target are the same file"));
    }
    if (!created && ::ftruncate(out.get(), 0) != 0) {
        return fail(errno_error(errno, to.str()));
    }
    char buffer[COPY_BUFFER_SIZE];
    for (;;) {
        const ssize_t got = ::read(in.get(), buffer, sizeof buffer);
        if (got < 0) {
            if (errno == EINTR) continue;
            return fail(errno_error(errno, from.str()));
        }
        if (got == 0) break;
        std::size_t done = 0;
        const std::size_t total = static_cast<std::size_t>(got);
        while (done < total) {
            const ssize_t put = ::write(out.get(), buffer + done, total - done);
            if (put < 0) {
                if (errno == EINTR) continue;
                return fail(errno_error(errno, to.str()));
            }
            done += static_cast<std::size_t>(put);
        }
    }
    return {};
}

Expected<Path, Error> absolute(PathView path) {
    if (path_detail::is_absolute(path.str(), HOST_PATH_STYLE)) {
        return normalize(path);
    }
    char cwd[PATH_MAX];
    if (::getcwd(cwd, sizeof cwd) == nullptr) {
        return Unexpected(errno_error(errno, "getcwd"));
    }
    Path joined(cwd);
    joined /= path.str();
    return normalize(joined.view());
}

Expected<Path, Error> canonical(PathView path) {
    const PathCString name(path.str());
    char* resolved = ::realpath(name.c_str(), nullptr);
    if (resolved == nullptr) {
        return Unexpected(errno_error(errno, path.str()));
    }
    Path out(resolved);
    std::free(resolved);
    return out;
}

Path temp_directory() {
    // dstd sits below common, so TMPDIR is read here and not through Env.
    const char* dir = std::getenv("TMPDIR");
    if (dir != nullptr && dir[0] != '\0') return Path(StringView(dir));
    return Path(StringView("/tmp"));
}

Expected<SpaceInfo, Error> space(PathView path) {
    const PathCString name(path.str());
    struct statvfs info;
    if (::statvfs(name.c_str(), &info) != 0) {
        return Unexpected(errno_error(errno, path.str()));
    }
    const std::uint64_t unit =
        info.f_frsize != 0 ? info.f_frsize : info.f_bsize;
    SpaceInfo out;
    out.capacity = static_cast<std::uint64_t>(info.f_blocks) * unit;
    out.free = static_cast<std::uint64_t>(info.f_bfree) * unit;
    out.available = static_cast<std::uint64_t>(info.f_bavail) * unit;
    return out;
}

Expected<std::uint64_t, Error> file_size(PathView path) {
    const Expected<FileStat, Error> found = stat(path);
    if (!found) return Unexpected(found.error());
    return found->size;
}

Expected<std::uint64_t, Error> mtime_ns(PathView path) {
    const Expected<FileStat, Error> found = stat(path);
    if (!found) return Unexpected(found.error());
    return found->mtime_ns;
}

DirEntries::DirEntries(void* listing) noexcept : listing_(listing) {
    advance();
}

DirEntries::DirEntries(DirEntries&& other) noexcept
    : listing_(other.listing_), current_(other.current_), error_(other.error_) {
    other.listing_ = nullptr;
    other.current_ = {};
}

DirEntries& DirEntries::operator=(DirEntries&& other) noexcept {
    if (this != &other) {
        if (listing_ != nullptr) ::closedir(static_cast<DIR*>(listing_));
        listing_ = other.listing_;
        current_ = other.current_;
        error_ = other.error_;
        other.listing_ = nullptr;
        other.current_ = {};
    }
    return *this;
}

DirEntries::~DirEntries() {
    if (listing_ != nullptr) ::closedir(static_cast<DIR*>(listing_));
}

void DirEntries::advance() {
    current_ = {};
    error_ = 0;
    if (listing_ == nullptr) return;
    DIR* dir = static_cast<DIR*>(listing_);
    for (;;) {
        errno = 0;
        const struct dirent* entry = ::readdir(dir);
        if (entry == nullptr) {
            error_ = errno;
            return;
        }
        const StringView name(entry->d_name);
        if (name == "." || name == "..") continue;
        current_.name = name;
        current_.kind = kind_from_dtype(entry->d_type);
        return;
    }
}

DirEntries::iterator DirEntries::begin() {
    if (current_.name.data() == nullptr) return end();
    return iterator(this);
}

DirEntries::iterator DirEntries::end() noexcept { return iterator(); }

Expected<Dir, Error> Dir::open(PathView path) {
    const PathCString name(path.str());
    const int fd = ::open(name.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (fd < 0) return Unexpected(errno_error(errno, path.str()));
    return Dir(ScopedFd(fd));
}

Expected<FileStat, Error> Dir::stat(StringView name) const {
    const PathCString relative(name);
    struct stat info;
    if (::fstatat(fd_.get(), relative.c_str(), &info, 0) != 0) {
        return Unexpected(errno_error(errno, name));
    }
    return make_file_stat(info);
}

Expected<FileStat, Error> Dir::lstat(StringView name) const {
    const PathCString relative(name);
    struct stat info;
    if (::fstatat(fd_.get(), relative.c_str(), &info, AT_SYMLINK_NOFOLLOW) !=
        0) {
        return Unexpected(errno_error(errno, name));
    }
    return make_file_stat(info);
}

Expected<Dir, Error> Dir::open_dir(StringView name) const {
    const PathCString relative(name);
    const int fd = ::openat(fd_.get(), relative.c_str(),
                            O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (fd < 0) return Unexpected(errno_error(errno, name));
    return Dir(ScopedFd(fd));
}

Expected<ScopedFd, Error> Dir::open_file(StringView name, int flags,
                                         int mode) const {
    const PathCString relative(name);
    const int fd =
        ::openat(fd_.get(), relative.c_str(), flags | O_CLOEXEC, mode);
    if (fd < 0) return Unexpected(errno_error(errno, name));
    return ScopedFd(fd);
}

Expected<DirEntries, Error> Dir::entries() const {
    const int copy =
        ::openat(fd_.get(), ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (copy < 0) return Unexpected(errno_error(errno, "openat"));
    DIR* dir = ::fdopendir(copy);
    if (dir == nullptr) {
        const int error = errno;
        ::close(copy);
        return Unexpected(errno_error(error, "fdopendir"));
    }
    return DirEntries(static_cast<void*>(dir));
}

Expected<std::uint64_t, Error> walk(
    PathView root, FunctionRef<WalkAction(PathView path, FileKind kind)> fn) {
    const StringView bytes = strip_trailing_separators(root.str());
    const Expected<FileStat, Error> found = lstat(PathView(bytes));
    if (!found) return Unexpected(found.error());
    String path(bytes);
    WalkState state{fn, &path, 0, false};
    ++state.count;
    const WalkAction action = state.fn(PathView(path), found->kind);
    if (action == WalkAction::Stop) return state.count;
    if (found->kind != FileKind::Directory) return state.count;
    if (action == WalkAction::SkipDir) return state.count;
    Expected<Dir, Error> dir = open_dir_nofollow(AT_FDCWD, bytes, bytes);
    if (!dir) return Unexpected(dir.error());
    const Expected<void, Error> below = walk_children(*dir, state);
    if (!below) return Unexpected(below.error());
    return state.count;
}

}  // namespace dftracer::utils::dstd::fs
