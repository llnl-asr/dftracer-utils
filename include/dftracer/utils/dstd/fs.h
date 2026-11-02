#ifndef DFTRACER_UTILS_DSTD_FS_H
#define DFTRACER_UTILS_DSTD_FS_H

#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/function_ref.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/scoped_fd.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>

namespace dftracer::utils::dstd::fs {

enum class FileKind : std::uint8_t { Unknown, File, Directory, Symlink, Other };

/// What one metadata call reports. mtime_ns counts nanoseconds since the Unix
/// epoch. dev and ino identify the entry on this host.
struct FileStat {
    std::uint64_t size = 0;
    std::uint64_t mtime_ns = 0;
    std::uint64_t dev = 0;
    std::uint64_t ino = 0;
    std::uint32_t mode = 0;
    FileKind kind = FileKind::Unknown;
};

/// Bytes of a file system, from statvfs.
struct SpaceInfo {
    std::uint64_t capacity = 0;
    std::uint64_t free = 0;
    std::uint64_t available = 0;
};

enum class CopyMode : std::uint8_t { FailIfExists, Overwrite };

/// What walk() does after one visit. Continue descends into a directory,
/// SkipDir leaves it alone, Stop ends the whole walk.
enum class WalkAction : std::uint8_t { Continue, SkipDir, Stop };

/// Every call reports the errno text in the error message. A missing entry is
/// Condition::NotFound, EINVAL and ENAMETOOLONG are InvalidArgument, and every
/// other failure is Io.
Expected<FileStat, Error> stat(PathView path);
Expected<FileStat, Error> lstat(PathView path);
/// False only when the path does not exist. Any other failure is an error.
Expected<bool, Error> exists(PathView path);
/// True when the directory was created, false when it already existed.
Expected<bool, Error> create_directory(PathView path);
/// Creates every missing component. An existing directory is success, an
/// existing non-directory is an error.
Expected<bool, Error> create_directories(PathView path);
/// Removes a file, a symlink or an empty directory. False when it is missing.
Expected<bool, Error> remove(PathView path);
/// Removes the tree below path and path itself. A missing path is success with
/// 0. A symlink is removed, never followed, even with a trailing separator.
/// Children are removed by name relative to the held directory.
Expected<std::uint64_t, Error> remove_all(PathView path);
Expected<void, Error> rename(PathView from, PathView to);
Expected<void, Error> copy_file(PathView from, PathView to, CopyMode mode);
/// The path with the current working directory prepended when it is relative,
/// normalized lexically. No syscall touches the entry itself.
Expected<Path, Error> absolute(PathView path);
/// realpath: every symlink resolved, so the result names an existing entry.
Expected<Path, Error> canonical(PathView path);
/// TMPDIR when it is set and not empty, else /tmp. Names an existing directory.
Path temp_directory();
Expected<SpaceInfo, Error> space(PathView path);
Expected<std::uint64_t, Error> file_size(PathView path);
Expected<std::uint64_t, Error> mtime_ns(PathView path);

/// One entry of a listing, from readdir alone. name is a view of the entry name
/// only, and it stays valid until the iterator that produced it is incremented
/// or the DirEntries is destroyed. kind is Unknown when the platform reports no
/// d_type for it, so a caller that needs the kind must fstatat it.
struct DirEntry {
    StringView name;
    FileKind kind;
};

/// A directory handle. Every lookup by name is relative to it, so it works on a
/// directory the process cannot open by full path, and a rename of an ancestor
/// does not change what it names.
class Dir;

/// An input range over the entries of a directory, opened by Dir::entries. It
/// holds its own listing handle, so two of them, or one beside a still open
/// Dir, do not disturb each other.
class DirEntries {
   public:
    class iterator;

    DirEntries() noexcept = default;
    DirEntries(DirEntries&& other) noexcept;
    DirEntries& operator=(DirEntries&& other) noexcept;
    DirEntries(const DirEntries&) = delete;
    DirEntries& operator=(const DirEntries&) = delete;
    ~DirEntries();

    iterator begin();
    iterator end() noexcept;

    /// The errno that ended the listing early, 0 when it ended normally.
    int error() const noexcept { return error_; }

   private:
    friend class Dir;
    explicit DirEntries(void* listing) noexcept;
    void advance();

    void* listing_ = nullptr;
    DirEntry current_{};
    int error_ = 0;
};

class DirEntries::iterator {
   public:
    using iterator_category = std::input_iterator_tag;
    using value_type = DirEntry;
    using difference_type = std::ptrdiff_t;
    using pointer = const DirEntry*;
    using reference = const DirEntry&;

    iterator() noexcept = default;

    reference operator*() const noexcept { return owner_->current_; }
    pointer operator->() const noexcept { return &owner_->current_; }

    iterator& operator++() {
        owner_->advance();
        if (owner_->current_.name.data() == nullptr) owner_ = nullptr;
        return *this;
    }

    void operator++(int) { ++*this; }

    friend bool operator==(const iterator& a, const iterator& b) noexcept {
        return a.owner_ == b.owner_;
    }

   private:
    friend class DirEntries;
    explicit iterator(DirEntries* owner) noexcept : owner_(owner) {}

    DirEntries* owner_ = nullptr;
};

class Dir {
   public:
    Dir() noexcept = default;
    explicit Dir(ScopedFd fd) noexcept : fd_(std::move(fd)) {}
    Dir(Dir&&) noexcept = default;
    Dir& operator=(Dir&&) noexcept = default;
    Dir(const Dir&) = delete;
    Dir& operator=(const Dir&) = delete;

    static Expected<Dir, Error> open(PathView path);

    int fd() const noexcept { return fd_.get(); }
    bool valid() const noexcept { return fd_.valid(); }

    /// fstatat on name relative to this directory, following a symlink.
    Expected<FileStat, Error> stat(StringView name) const;
    /// fstatat on name relative to this directory, not following a symlink.
    Expected<FileStat, Error> lstat(StringView name) const;
    Expected<Dir, Error> open_dir(StringView name) const;
    Expected<ScopedFd, Error> open_file(StringView name, int flags,
                                        int mode = 0644) const;
    /// Opens a new directory description for the listing, so the caller may
    /// keep using *this and may list it again.
    Expected<DirEntries, Error> entries() const;

   private:
    ScopedFd fd_;
};

/// Depth-first, pre-order walk of the tree at root. fn is called for root
/// itself first, then for each descendant before its children. A symlinked
/// directory is reported and not followed. The path handed to fn is built in
/// one buffer and is valid only during that call. Returns how many entries were
/// visited. Opening a directory the walk cannot read is an error. A level's
/// entries are read and the listing closed before the walk descends, so one
/// descriptor is held per level of depth, and one more while a level is read.
Expected<std::uint64_t, Error> walk(
    PathView root, FunctionRef<WalkAction(PathView path, FileKind kind)> fn);

}  // namespace dftracer::utils::dstd::fs

#endif  // DFTRACER_UTILS_DSTD_FS_H
