#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/object_pool.h>
#include <dftracer/utils/core/io/io_backend.h>
#include <dftracer/utils/core/io/io_thread_pool.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/rocksdb/filesystem.h>
#include <fcntl.h>
#include <rocksdb/env.h>
#include <rocksdb/file_system.h>
#include <rocksdb/io_status.h>
#include <rocksdb/slice.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace dftracer::utils::rocksdb {

namespace {

io::IoBackend* current_io_backend() {
    auto* executor = Executor::current();
    if (executor == nullptr || !executor->has_io_backend()) {
        return nullptr;
    }
    return &executor->io_backend();
}

class DfTracerFileSystem;

struct AsyncReadHandle {
    explicit AsyncReadHandle(DfTracerFileSystem* owner_) : owner(owner_) {}

    static void* operator new(std::size_t size) {
        return ObjectPool::instance().allocate(size);
    }

    static void operator delete(void* ptr, std::size_t size) noexcept {
        ObjectPool::instance().deallocate(ptr, size);
    }

    DfTracerFileSystem* owner;
    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false;
    bool callback_delivered = false;
    bool aborted = false;
    bool running = false;
    std::string path;
    std::uint64_t offset = 0;
    std::size_t len = 0;
    char* scratch = nullptr;
    ::rocksdb::Slice result;
    ::rocksdb::IOStatus status;
    std::function<void(::rocksdb::FSReadRequest&, void*)> callback;
    void* callback_arg = nullptr;
};

::rocksdb::IOStatus io_error(std::string_view op, std::string_view path) {
    return ::rocksdb::IOStatus::IOError(
        std::string(path), std::string(op) + ": " + std::strerror(errno));
}

ssize_t pread_sync(int fd, void* buf, std::size_t len, off_t offset) {
    if (auto* backend = current_io_backend(); backend != nullptr) {
        return backend->submit_read_sync(fd, buf, len, offset);
    }
    return ::pread(fd, buf, len, offset);
}

ssize_t pwrite_sync(int fd, const void* buf, std::size_t len, off_t offset) {
    if (auto* backend = current_io_backend(); backend != nullptr) {
        return backend->submit_write_sync(fd, buf, len, offset);
    }
    return ::pwrite(fd, buf, len, offset);
}

int fsync_sync(int fd) {
    if (auto* backend = current_io_backend(); backend != nullptr) {
        return backend->submit_fsync_sync(fd);
    }
    return ::fsync(fd);
}

int ftruncate_sync(int fd, off_t length) {
    if (auto* backend = current_io_backend(); backend != nullptr) {
        return backend->submit_ftruncate_sync(fd, length);
    }
    return ::ftruncate(fd, length);
}

int fstat_sync(int fd, struct stat* st) {
    if (auto* backend = current_io_backend(); backend != nullptr) {
        return backend->submit_fstat_sync(fd, st);
    }
    return ::fstat(fd, st);
}

class DfTracerSequentialFile final : public ::rocksdb::FSSequentialFile {
   public:
    DfTracerSequentialFile(std::string path, int fd)
        : path_(std::move(path)), fd_(fd) {}

    ~DfTracerSequentialFile() override {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    ::rocksdb::IOStatus Read(std::size_t n, const ::rocksdb::IOOptions&,
                             ::rocksdb::Slice* result, char* scratch,
                             ::rocksdb::IODebugContext*) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const ssize_t bytes =
            pread_sync(fd_, scratch, n, static_cast<off_t>(offset_));
        if (bytes < 0) {
            return io_error("read", path_);
        }
        offset_ += static_cast<std::uint64_t>(bytes);
        *result = ::rocksdb::Slice(scratch, static_cast<std::size_t>(bytes));
        return ::rocksdb::IOStatus::OK();
    }

    ::rocksdb::IOStatus Skip(std::uint64_t n) override {
        std::lock_guard<std::mutex> lock(mutex_);
        offset_ += n;
        return ::rocksdb::IOStatus::OK();
    }

    ::rocksdb::IOStatus InvalidateCache(std::size_t, std::size_t) override {
        return ::rocksdb::IOStatus::OK();
    }

   private:
    std::string path_;
    int fd_;
    std::uint64_t offset_ = 0;
    std::mutex mutex_;
};

class DfTracerRandomAccessFile final : public ::rocksdb::FSRandomAccessFile {
   public:
    DfTracerRandomAccessFile(DfTracerFileSystem* owner, std::string path,
                             int fd)
        : owner_(owner), path_(std::move(path)), fd_(fd) {}

    ~DfTracerRandomAccessFile() override {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    ::rocksdb::IOStatus Read(std::uint64_t offset, std::size_t n,
                             const ::rocksdb::IOOptions&,
                             ::rocksdb::Slice* result, char* scratch,
                             ::rocksdb::IODebugContext*) const override {
        const ssize_t bytes =
            pread_sync(fd_, scratch, n, static_cast<off_t>(offset));
        if (bytes < 0) {
            return io_error("pread", path_);
        }
        *result = ::rocksdb::Slice(scratch, static_cast<std::size_t>(bytes));
        return ::rocksdb::IOStatus::OK();
    }

    ::rocksdb::IOStatus Prefetch(std::uint64_t, std::size_t,
                                 const ::rocksdb::IOOptions&,
                                 ::rocksdb::IODebugContext*) override {
        return ::rocksdb::IOStatus::OK();
    }

    ::rocksdb::IOStatus ReadAsync(
        ::rocksdb::FSReadRequest& req, const ::rocksdb::IOOptions& opts,
        std::function<void(::rocksdb::FSReadRequest&, void*)> cb, void* cb_arg,
        void** io_handle, ::rocksdb::IOHandleDeleter* del_fn,
        ::rocksdb::IODebugContext* dbg) override;

    ::rocksdb::IOStatus InvalidateCache(std::size_t, std::size_t) override {
        return ::rocksdb::IOStatus::OK();
    }

    ::rocksdb::IOStatus GetFileSize(std::uint64_t* result) override {
        struct stat st{};
        if (fstat_sync(fd_, &st) != 0) {
            return io_error("fstat", path_);
        }
        *result = static_cast<std::uint64_t>(st.st_size);
        return ::rocksdb::IOStatus::OK();
    }

   private:
    DfTracerFileSystem* owner_;
    std::string path_;
    int fd_;
};

class DfTracerWritableFile final : public ::rocksdb::FSWritableFile {
   public:
    using ::rocksdb::FSWritableFile::Append;
    using ::rocksdb::FSWritableFile::PositionedAppend;

    DfTracerWritableFile(std::string path, int fd,
                         const ::rocksdb::FileOptions& options)
        : ::rocksdb::FSWritableFile(options), path_(std::move(path)), fd_(fd) {
        struct stat st{};
        if (fstat_sync(fd_, &st) == 0) {
            size_ = static_cast<std::uint64_t>(st.st_size);
        }
    }

    ~DfTracerWritableFile() override {
        if (fd_ >= 0) {
            static_cast<void>(close_fd());
        }
    }

    ::rocksdb::IOStatus Append(const ::rocksdb::Slice& data,
                               const ::rocksdb::IOOptions&,
                               ::rocksdb::IODebugContext*) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return write_at(data, size_);
    }

    ::rocksdb::IOStatus PositionedAppend(const ::rocksdb::Slice& data,
                                         std::uint64_t offset,
                                         const ::rocksdb::IOOptions&,
                                         ::rocksdb::IODebugContext*) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return write_at(data, offset);
    }

    ::rocksdb::IOStatus Truncate(std::uint64_t size,
                                 const ::rocksdb::IOOptions&,
                                 ::rocksdb::IODebugContext*) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ftruncate_sync(fd_, static_cast<off_t>(size)) != 0) {
            return io_error("ftruncate", path_);
        }
        size_ = size;
        return ::rocksdb::IOStatus::OK();
    }

    ::rocksdb::IOStatus Close(const ::rocksdb::IOOptions&,
                              ::rocksdb::IODebugContext*) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return close_fd();
    }

    ::rocksdb::IOStatus Flush(const ::rocksdb::IOOptions&,
                              ::rocksdb::IODebugContext*) override {
        return ::rocksdb::IOStatus::OK();
    }

    ::rocksdb::IOStatus Sync(const ::rocksdb::IOOptions&,
                             ::rocksdb::IODebugContext*) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fd_ < 0) {
            return ::rocksdb::IOStatus::OK();
        }
        if (fsync_sync(fd_) != 0) {
            return io_error("fsync", path_);
        }
        return ::rocksdb::IOStatus::OK();
    }

    bool IsSyncThreadSafe() const override { return true; }

    std::uint64_t GetFileSize(const ::rocksdb::IOOptions&,
                              ::rocksdb::IODebugContext*) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_;
    }

    ::rocksdb::IOStatus InvalidateCache(std::size_t, std::size_t) override {
        return ::rocksdb::IOStatus::OK();
    }

    ::rocksdb::IOStatus RangeSync(std::uint64_t, std::uint64_t,
                                  const ::rocksdb::IOOptions& options,
                                  ::rocksdb::IODebugContext* dbg) override {
        return Sync(options, dbg);
    }

   private:
    ::rocksdb::IOStatus write_at(const ::rocksdb::Slice& data,
                                 std::uint64_t offset) {
        const ssize_t bytes = pwrite_sync(fd_, data.data(), data.size(),
                                          static_cast<off_t>(offset));
        if (bytes < 0 || static_cast<std::size_t>(bytes) != data.size()) {
            return io_error("pwrite", path_);
        }
        size_ = std::max(size_, offset + static_cast<std::uint64_t>(bytes));
        return ::rocksdb::IOStatus::OK();
    }

    ::rocksdb::IOStatus close_fd() {
        if (fd_ < 0) {
            return ::rocksdb::IOStatus::OK();
        }
        if (::close(fd_) != 0) {
            return io_error("close", path_);
        }
        fd_ = -1;
        return ::rocksdb::IOStatus::OK();
    }

    std::string path_;
    int fd_;
    std::uint64_t size_ = 0;
    mutable std::mutex mutex_;
};

class LocalFileSystemWrapper : public ::rocksdb::FileSystem {
   public:
    explicit LocalFileSystemWrapper(
        const std::shared_ptr<::rocksdb::FileSystem>& target)
        : target_(target) {}

    ::rocksdb::FileSystem* target() const { return target_.get(); }

    ::rocksdb::IOStatus NewSequentialFile(
        const std::string& f, const ::rocksdb::FileOptions& file_opts,
        std::unique_ptr<::rocksdb::FSSequentialFile>* r,
        ::rocksdb::IODebugContext* dbg) override {
        return target_->NewSequentialFile(f, file_opts, r, dbg);
    }

    ::rocksdb::IOStatus NewRandomAccessFile(
        const std::string& f, const ::rocksdb::FileOptions& file_opts,
        std::unique_ptr<::rocksdb::FSRandomAccessFile>* r,
        ::rocksdb::IODebugContext* dbg) override {
        return target_->NewRandomAccessFile(f, file_opts, r, dbg);
    }

    ::rocksdb::IOStatus NewWritableFile(
        const std::string& f, const ::rocksdb::FileOptions& file_opts,
        std::unique_ptr<::rocksdb::FSWritableFile>* r,
        ::rocksdb::IODebugContext* dbg) override {
        return target_->NewWritableFile(f, file_opts, r, dbg);
    }

    ::rocksdb::IOStatus ReopenWritableFile(
        const std::string& fname, const ::rocksdb::FileOptions& file_opts,
        std::unique_ptr<::rocksdb::FSWritableFile>* result,
        ::rocksdb::IODebugContext* dbg) override {
        return target_->ReopenWritableFile(fname, file_opts, result, dbg);
    }

    ::rocksdb::IOStatus ReuseWritableFile(
        const std::string& fname, const std::string& old_fname,
        const ::rocksdb::FileOptions& file_opts,
        std::unique_ptr<::rocksdb::FSWritableFile>* r,
        ::rocksdb::IODebugContext* dbg) override {
        return target_->ReuseWritableFile(fname, old_fname, file_opts, r, dbg);
    }

    ::rocksdb::IOStatus NewRandomRWFile(
        const std::string& fname, const ::rocksdb::FileOptions& file_opts,
        std::unique_ptr<::rocksdb::FSRandomRWFile>* result,
        ::rocksdb::IODebugContext* dbg) override {
        return target_->NewRandomRWFile(fname, file_opts, result, dbg);
    }

    ::rocksdb::IOStatus NewMemoryMappedFileBuffer(
        const std::string& fname,
        std::unique_ptr<::rocksdb::MemoryMappedFileBuffer>* result) override {
        return target_->NewMemoryMappedFileBuffer(fname, result);
    }

    ::rocksdb::IOStatus NewDirectory(
        const std::string& name, const ::rocksdb::IOOptions& io_opts,
        std::unique_ptr<::rocksdb::FSDirectory>* result,
        ::rocksdb::IODebugContext* dbg) override {
        return target_->NewDirectory(name, io_opts, result, dbg);
    }

    ::rocksdb::IOStatus FileExists(const std::string& f,
                                   const ::rocksdb::IOOptions& io_opts,
                                   ::rocksdb::IODebugContext* dbg) override {
        return target_->FileExists(f, io_opts, dbg);
    }

    ::rocksdb::IOStatus GetChildren(const std::string& dir,
                                    const ::rocksdb::IOOptions& io_opts,
                                    std::vector<std::string>* r,
                                    ::rocksdb::IODebugContext* dbg) override {
        return target_->GetChildren(dir, io_opts, r, dbg);
    }

    ::rocksdb::IOStatus GetChildrenFileAttributes(
        const std::string& dir, const ::rocksdb::IOOptions& options,
        std::vector<::rocksdb::FileAttributes>* result,
        ::rocksdb::IODebugContext* dbg) override {
        return target_->GetChildrenFileAttributes(dir, options, result, dbg);
    }

    ::rocksdb::IOStatus DeleteFile(const std::string& f,
                                   const ::rocksdb::IOOptions& options,
                                   ::rocksdb::IODebugContext* dbg) override {
        return target_->DeleteFile(f, options, dbg);
    }

    ::rocksdb::IOStatus Truncate(const std::string& fname, size_t size,
                                 const ::rocksdb::IOOptions& options,
                                 ::rocksdb::IODebugContext* dbg) override {
        return target_->Truncate(fname, size, options, dbg);
    }

    ::rocksdb::IOStatus CreateDir(const std::string& d,
                                  const ::rocksdb::IOOptions& options,
                                  ::rocksdb::IODebugContext* dbg) override {
        return target_->CreateDir(d, options, dbg);
    }

    ::rocksdb::IOStatus CreateDirIfMissing(
        const std::string& d, const ::rocksdb::IOOptions& options,
        ::rocksdb::IODebugContext* dbg) override {
        return target_->CreateDirIfMissing(d, options, dbg);
    }

    ::rocksdb::IOStatus DeleteDir(const std::string& d,
                                  const ::rocksdb::IOOptions& options,
                                  ::rocksdb::IODebugContext* dbg) override {
        return target_->DeleteDir(d, options, dbg);
    }

    ::rocksdb::IOStatus GetFileSize(const std::string& f,
                                    const ::rocksdb::IOOptions& options,
                                    uint64_t* s,
                                    ::rocksdb::IODebugContext* dbg) override {
        return target_->GetFileSize(f, options, s, dbg);
    }

    ::rocksdb::IOStatus GetFileModificationTime(
        const std::string& fname, const ::rocksdb::IOOptions& options,
        uint64_t* file_mtime, ::rocksdb::IODebugContext* dbg) override {
        return target_->GetFileModificationTime(fname, options, file_mtime,
                                                dbg);
    }

    ::rocksdb::IOStatus GetAbsolutePath(
        const std::string& db_path, const ::rocksdb::IOOptions& options,
        std::string* output_path, ::rocksdb::IODebugContext* dbg) override {
        return target_->GetAbsolutePath(db_path, options, output_path, dbg);
    }

    ::rocksdb::IOStatus RenameFile(const std::string& s, const std::string& t,
                                   const ::rocksdb::IOOptions& options,
                                   ::rocksdb::IODebugContext* dbg) override {
        return target_->RenameFile(s, t, options, dbg);
    }

    ::rocksdb::IOStatus LinkFile(const std::string& s, const std::string& t,
                                 const ::rocksdb::IOOptions& options,
                                 ::rocksdb::IODebugContext* dbg) override {
        return target_->LinkFile(s, t, options, dbg);
    }

    ::rocksdb::IOStatus NumFileLinks(const std::string& fname,
                                     const ::rocksdb::IOOptions& options,
                                     uint64_t* count,
                                     ::rocksdb::IODebugContext* dbg) override {
        return target_->NumFileLinks(fname, options, count, dbg);
    }

    ::rocksdb::IOStatus AreFilesSame(const std::string& first,
                                     const std::string& second,
                                     const ::rocksdb::IOOptions& options,
                                     bool* res,
                                     ::rocksdb::IODebugContext* dbg) override {
        return target_->AreFilesSame(first, second, options, res, dbg);
    }

    ::rocksdb::IOStatus LockFile(const std::string& f,
                                 const ::rocksdb::IOOptions& options,
                                 ::rocksdb::FileLock** l,
                                 ::rocksdb::IODebugContext* dbg) override {
        return target_->LockFile(f, options, l, dbg);
    }

    ::rocksdb::IOStatus UnlockFile(::rocksdb::FileLock* l,
                                   const ::rocksdb::IOOptions& options,
                                   ::rocksdb::IODebugContext* dbg) override {
        return target_->UnlockFile(l, options, dbg);
    }

    ::rocksdb::IOStatus GetTestDirectory(
        const ::rocksdb::IOOptions& options, std::string* path,
        ::rocksdb::IODebugContext* dbg) override {
        return target_->GetTestDirectory(options, path, dbg);
    }

    ::rocksdb::IOStatus NewLogger(const std::string& fname,
                                  const ::rocksdb::IOOptions& options,
                                  std::shared_ptr<::rocksdb::Logger>* result,
                                  ::rocksdb::IODebugContext* dbg) override {
        return target_->NewLogger(fname, options, result, dbg);
    }

    void SanitizeFileOptions(::rocksdb::FileOptions* opts) const override {
        target_->SanitizeFileOptions(opts);
    }

    ::rocksdb::FileOptions OptimizeForLogRead(
        const ::rocksdb::FileOptions& file_options) const override {
        return target_->OptimizeForLogRead(file_options);
    }

    ::rocksdb::FileOptions OptimizeForManifestRead(
        const ::rocksdb::FileOptions& file_options) const override {
        return target_->OptimizeForManifestRead(file_options);
    }

    ::rocksdb::FileOptions OptimizeForLogWrite(
        const ::rocksdb::FileOptions& file_options,
        const ::rocksdb::DBOptions& db_options) const override {
        return target_->OptimizeForLogWrite(file_options, db_options);
    }

    ::rocksdb::FileOptions OptimizeForManifestWrite(
        const ::rocksdb::FileOptions& file_options) const override {
        return target_->OptimizeForManifestWrite(file_options);
    }

    ::rocksdb::FileOptions OptimizeForCompactionTableWrite(
        const ::rocksdb::FileOptions& file_options,
        const ::rocksdb::ImmutableDBOptions& immutable_opts) const override {
        return target_->OptimizeForCompactionTableWrite(file_options,
                                                        immutable_opts);
    }

    ::rocksdb::FileOptions OptimizeForCompactionTableRead(
        const ::rocksdb::FileOptions& file_options,
        const ::rocksdb::ImmutableDBOptions& db_options) const override {
        return target_->OptimizeForCompactionTableRead(file_options,
                                                       db_options);
    }

    ::rocksdb::FileOptions OptimizeForBlobFileRead(
        const ::rocksdb::FileOptions& file_options,
        const ::rocksdb::ImmutableDBOptions& db_options) const override {
        return target_->OptimizeForBlobFileRead(file_options, db_options);
    }

    ::rocksdb::IOStatus GetFreeSpace(const std::string& path,
                                     const ::rocksdb::IOOptions& options,
                                     uint64_t* diskfree,
                                     ::rocksdb::IODebugContext* dbg) override {
        return target_->GetFreeSpace(path, options, diskfree, dbg);
    }

    ::rocksdb::IOStatus IsDirectory(const std::string& path,
                                    const ::rocksdb::IOOptions& options,
                                    bool* is_dir,
                                    ::rocksdb::IODebugContext* dbg) override {
        return target_->IsDirectory(path, options, is_dir, dbg);
    }

    const ::rocksdb::Customizable* Inner() const override {
        return target_.get();
    }

    ::rocksdb::Status PrepareOptions(
        const ::rocksdb::ConfigOptions& options) override {
        return target_->PrepareOptions(options);
    }

    std::string SerializeOptions(const ::rocksdb::ConfigOptions& config_options,
                                 const std::string& header) const override {
        return ::rocksdb::FileSystem::SerializeOptions(config_options, header);
    }

    ::rocksdb::IOStatus Poll(std::vector<void*>& io_handles,
                             size_t min_completions) override {
        return target_->Poll(io_handles, min_completions);
    }

    ::rocksdb::IOStatus AbortIO(std::vector<void*>& io_handles) override {
        return target_->AbortIO(io_handles);
    }

    void DiscardCacheForDirectory(const std::string& path) override {
        target_->DiscardCacheForDirectory(path);
    }

    void SupportedOps(int64_t& supported_ops) override {
        target_->SupportedOps(supported_ops);
    }

   protected:
    std::shared_ptr<::rocksdb::FileSystem> target_;
};

class DfTracerFileSystem final : public LocalFileSystemWrapper {
   public:
    explicit DfTracerFileSystem(
        const std::shared_ptr<::rocksdb::FileSystem>& target)
        : LocalFileSystemWrapper(target), fallback_pool_(4) {
        // Started lazily on the first fallback read (see submit_async_read):
        // reads on a runtime worker take the io-backend path, so a cached DB's
        // filesystem that never hits the fallback never spawns pool threads.
    }

    ~DfTracerFileSystem() override { fallback_pool_.stop(); }

    static const char* class_name() { return "DfTracerFileSystem"; }

    const char* Name() const override { return class_name(); }

    bool IsInstanceOf(const std::string& name) const override {
        return name == class_name() ||
               LocalFileSystemWrapper::IsInstanceOf(name);
    }

    void SupportedOps(int64_t& supported_ops) override {
        supported_ops = 0;
#ifndef DFTRACER_UTILS_VALGRIND_MODE
        // Async prefetch through our io backend is not Valgrind-safe: under its
        // serialized scheduler a scan's in-flight ReadAsync can stall (the
        // server's /viz/density handler then never responds) and RocksDB's
        // prefetch context leaks at exit. Fall back to synchronous reads there,
        // matching how io_uring is already disabled under Valgrind.
        supported_ops |= (1 << ::rocksdb::FSSupportedOps::kAsyncIO);
        supported_ops |= (1 << ::rocksdb::FSSupportedOps::kFSPrefetch);
#endif
    }

    ::rocksdb::IOStatus NewSequentialFile(
        const std::string& fname, const ::rocksdb::FileOptions&,
        std::unique_ptr<::rocksdb::FSSequentialFile>* result,
        ::rocksdb::IODebugContext*) override {
        int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return io_error("open", fname);
        }
        result->reset(new DfTracerSequentialFile(fname, fd));
        return ::rocksdb::IOStatus::OK();
    }

    ::rocksdb::IOStatus NewRandomAccessFile(
        const std::string& fname, const ::rocksdb::FileOptions&,
        std::unique_ptr<::rocksdb::FSRandomAccessFile>* result,
        ::rocksdb::IODebugContext*) override {
        int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return io_error("open", fname);
        }
        result->reset(new DfTracerRandomAccessFile(this, fname, fd));
        return ::rocksdb::IOStatus::OK();
    }

    ::rocksdb::IOStatus NewWritableFile(
        const std::string& fname, const ::rocksdb::FileOptions& file_opts,
        std::unique_ptr<::rocksdb::FSWritableFile>* result,
        ::rocksdb::IODebugContext*) override {
        int fd =
            ::open(fname.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
        if (fd < 0) {
            return io_error("open", fname);
        }
        result->reset(new DfTracerWritableFile(fname, fd, file_opts));
        return ::rocksdb::IOStatus::OK();
    }

    ::rocksdb::IOStatus ReopenWritableFile(
        const std::string& fname, const ::rocksdb::FileOptions& file_opts,
        std::unique_ptr<::rocksdb::FSWritableFile>* result,
        ::rocksdb::IODebugContext*) override {
        int fd = ::open(fname.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (fd < 0) {
            return io_error("open", fname);
        }
        result->reset(new DfTracerWritableFile(fname, fd, file_opts));
        return ::rocksdb::IOStatus::OK();
    }

    ::rocksdb::IOStatus ReuseWritableFile(
        const std::string& fname, const std::string& old_fname,
        const ::rocksdb::FileOptions& file_opts,
        std::unique_ptr<::rocksdb::FSWritableFile>* result,
        ::rocksdb::IODebugContext*) override {
        ::unlink(fname.c_str());
        if (::rename(old_fname.c_str(), fname.c_str()) != 0) {
            return io_error("rename", old_fname);
        }
        return ReopenWritableFile(fname, file_opts, result, nullptr);
    }

    ::rocksdb::IOStatus Poll(std::vector<void*>& io_handles,
                             size_t min_completions) override {
        const size_t target = std::min(min_completions, io_handles.size());
        if (target == 0) {
            return ::rocksdb::IOStatus::OK();
        }
        std::unique_lock<std::mutex> lock(completions_mutex_);
        completions_cv_.wait(lock, [&] {
            size_t completed = 0;
            for (void* io_handle : io_handles) {
                auto* handle = static_cast<AsyncReadHandle*>(io_handle);
                std::lock_guard<std::mutex> handle_lock(handle->mutex);
                if (handle->finished && !handle->callback_delivered) {
                    ++completed;
                }
            }
            return completed >= target;
        });
        lock.unlock();

        for (void* io_handle : io_handles) {
            auto* handle = static_cast<AsyncReadHandle*>(io_handle);
            std::unique_lock<std::mutex> handle_lock(handle->mutex);
            if (!handle->finished || handle->callback_delivered ||
                handle->aborted) {
                continue;
            }
            handle->callback_delivered = true;
            auto callback = handle->callback;
            auto callback_arg = handle->callback_arg;
            ::rocksdb::FSReadRequest req;
            req.offset = handle->offset;
            req.len = handle->len;
            req.scratch = handle->scratch;
            req.result = handle->result;
            req.status = handle->status;
            handle_lock.unlock();
            callback(req, callback_arg);
        }
        return ::rocksdb::IOStatus::OK();
    }

    ::rocksdb::IOStatus AbortIO(std::vector<void*>& io_handles) override {
        for (void* io_handle : io_handles) {
            auto* handle = static_cast<AsyncReadHandle*>(io_handle);
            std::lock_guard<std::mutex> lock(handle->mutex);
            handle->aborted = true;
        }

        for (void* io_handle : io_handles) {
            auto* handle = static_cast<AsyncReadHandle*>(io_handle);
            std::unique_lock<std::mutex> lock(handle->mutex);
            handle->cv.wait(lock, [&] { return handle->finished; });
            handle->callback_delivered = true;
        }

        return ::rocksdb::IOStatus::OK();
    }

    void submit_async_read(AsyncReadHandle* handle, int fd, std::string path,
                           ::rocksdb::IODebugContext* dbg) {
        {
            std::lock_guard<std::mutex> lock(handle->mutex);
            handle->running = true;
            handle->path = path;
        }
        if (auto* backend = current_io_backend(); backend != nullptr) {
            backend->submit_pread_callback(fd, handle->scratch, handle->len,
                                           static_cast<off_t>(handle->offset),
                                           &DfTracerFileSystem::on_pread_done,
                                           handle);
            // RocksDB submits a read then blocks polling for it, so a read left
            // in the backend's submission batch would never dispatch. Flush now
            // rather than relying on some other thread to drain the batch.
            backend->flush();
            return;
        }

        std::call_once(fallback_started_, [this] { fallback_pool_.start(); });
        fallback_pool_.submit([this, handle, fd, path = std::move(path), dbg] {
            ::rocksdb::Slice result;
            auto status = read_async_impl(fd, path, handle->offset, handle->len,
                                          &result, handle->scratch, dbg);
            complete_async_read(handle, status, result);
        });
    }

    static ::rocksdb::IOStatus read_async_impl(
        int fd, std::string_view path, std::uint64_t offset, std::size_t n,
        ::rocksdb::Slice* result, char* scratch, ::rocksdb::IODebugContext*) {
        const ssize_t bytes =
            ::pread(fd, scratch, n, static_cast<off_t>(offset));
        if (bytes < 0) {
            return io_error("pread", path);
        }
        *result = ::rocksdb::Slice(scratch, static_cast<std::size_t>(bytes));
        return ::rocksdb::IOStatus::OK();
    }

    static void delete_async_read_handle(void* io_handle) {
        delete static_cast<AsyncReadHandle*>(io_handle);
    }

   private:
    void complete_async_read(AsyncReadHandle* handle,
                             const ::rocksdb::IOStatus& status,
                             const ::rocksdb::Slice& result) {
        {
            // Notify inside the lock: once finished is visible a waiter (Poll,
            // AbortIO) or RocksDB's handle deleter may free the handle, so we
            // must not touch it again after releasing the mutex.
            std::lock_guard<std::mutex> lock(handle->mutex);
            handle->result = result;
            handle->status = status;
            handle->running = false;
            handle->finished = true;
            handle->cv.notify_all();
        }
        // completions_mutex_/cv_ live in the FileSystem, not the handle.
        std::lock_guard<std::mutex> lock(completions_mutex_);
        completions_cv_.notify_all();
    }

    static void on_pread_done(void* context, ssize_t result) noexcept {
        auto* handle = static_cast<AsyncReadHandle*>(context);
        ::rocksdb::IOStatus status = ::rocksdb::IOStatus::OK();
        ::rocksdb::Slice slice;
        if (result < 0) {
            errno = static_cast<int>(-result);
            status = io_error("pread", handle->path);
        } else {
            slice = ::rocksdb::Slice(handle->scratch,
                                     static_cast<std::size_t>(result));
        }
        handle->owner->complete_async_read(handle, status, slice);
    }

    std::once_flag fallback_started_;
    io::IoThreadPool fallback_pool_;
    std::mutex completions_mutex_;
    std::condition_variable completions_cv_;
};

::rocksdb::IOStatus DfTracerRandomAccessFile::ReadAsync(
    ::rocksdb::FSReadRequest& req, const ::rocksdb::IOOptions&,
    std::function<void(::rocksdb::FSReadRequest&, void*)> cb, void* cb_arg,
    void** io_handle, ::rocksdb::IOHandleDeleter* del_fn,
    ::rocksdb::IODebugContext* dbg) {
    auto* handle = new AsyncReadHandle(owner_);
    handle->offset = req.offset;
    handle->len = req.len;
    handle->scratch = req.scratch;
    handle->callback = std::move(cb);
    handle->callback_arg = cb_arg;
    *io_handle = static_cast<void*>(handle);
    *del_fn = &DfTracerFileSystem::delete_async_read_handle;
    owner_->submit_async_read(handle, fd_, path_, dbg);
    return ::rocksdb::IOStatus::OK();
}

}  // namespace

std::shared_ptr<::rocksdb::FileSystem> make_dftracer_file_system() {
    return std::make_shared<DfTracerFileSystem>(
        ::rocksdb::FileSystem::Default());
}

std::unique_ptr<::rocksdb::Env> make_dftracer_env(
    const std::shared_ptr<::rocksdb::FileSystem>& file_system) {
    return ::rocksdb::NewCompositeEnv(file_system);
}

}  // namespace dftracer::utils::rocksdb
