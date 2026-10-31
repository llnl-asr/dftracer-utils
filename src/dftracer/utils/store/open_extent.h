#ifndef DFTRACER_UTILS_STORE_OPEN_EXTENT_H
#define DFTRACER_UTILS_STORE_OPEN_EXTENT_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/io/io_backend.h>
#include <dftracer/utils/store/data_file.h>
#include <dftracer/utils/store/extent_codec.h>
#include <dftracer/utils/store/record.h>
#include <dftracer/utils/store/segment.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dftracer::utils {

class Executor;

}  // namespace dftracer::utils

namespace dftracer::utils::store::engine {

/// The raw page bytes a namespace collects before it trains a dictionary,
/// read at each page. A test lowers it so its peaks measure our code, not
/// this buffer, which fills with what a namespace writes up to this size.
inline std::atomic<std::size_t> g_dict_sample_bytes{std::size_t{8} << 20};

/// One written block of a data file: an extent of segments, or the run of a
/// segment too big for an extent, in its stored form. `length` is the stored
/// length rounded up to 4096. `crc` covers all `length` bytes. Segment offsets
/// count from the raw bytes. `contexts` runs parallel to `segments`.
struct WrittenExtent {
    std::uint64_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t crc = 0;
    dstd::Vector<ExtentSegment> segments;
    dstd::Vector<void*> contexts;
};

/// Receives an extent after its write completed. Runs on an I/O backend thread,
/// so `context` must outlive the writer.
using WrittenExtentFn = void (*)(void* context, WrittenExtent&& extent);

/// Receives the context of a segment no WrittenExtent will carry: the write of
/// its extent failed, or its extent was still open when the writer was
/// destroyed. Runs on the appending thread or on an I/O backend thread, so it
/// must free nothing the caller still uses.
using DiscardedContextFn = void (*)(void* context);

/// Called when the last extent write in flight completes, after its
/// WrittenExtentFn, with the WrittenExtentFn context. Runs on an I/O backend
/// thread while the writer holds its idle lock, so it must not call back into
/// the ExtentWriter.
using IdleFn = void (*)(void* context);

/// Fills the open extent of each namespace with sealed segments and writes it
/// through the runtime I/O backend once sealed. append() and seal_all() are
/// safe from many threads. wait_idle() must not run on the backend's executor.
/// Destroy only after in-flight writes reached zero. Unwritten contexts are
/// discarded.
class ExtentWriter {
   public:
    ExtentWriter(DataFileWriter& file, io::IoBackend& backend,
                 Executor& executor, std::uint32_t extent_size,
                 WrittenExtentFn on_written, void* context,
                 DiscardedContextFn on_discarded = nullptr,
                 IdleFn on_idle = nullptr);
    ExtentWriter(const ExtentWriter&) = delete;
    ExtentWriter& operator=(const ExtentWriter&) = delete;
    ExtentWriter(ExtentWriter&&) = delete;
    ExtentWriter& operator=(ExtentWriter&&) = delete;
    ~ExtentWriter();

    /// Copies a sealed segment into the open extent of `namespace_id`, or into
    /// a run of its own when it is an extent long or longer. The bytes are
    /// copied before this returns, and `context` comes back in
    /// WrittenExtent::contexts. An empty segment is InvalidArgument. Once a
    /// write failed, returns its error.
    dstd::Expected<void, dstd::Error> append(std::uint32_t namespace_id,
                                             dstd::ByteView segment_bytes,
                                             ExtentSegment info, void* context,
                                             PageCodec codec = PageCodec::Zstd);

    using SegmentWriteFn = void (*)(void* context, std::uint8_t* out);

    /// append() for a segment that `write` writes straight into the open
    /// extent, or into the buffer of its run, before this returns: `length`
    /// bytes at `out`, called once.
    dstd::Expected<void, dstd::Error> append_with(
        std::uint32_t namespace_id, std::size_t length, ExtentSegment info,
        void* context, PageCodec codec, SegmentWriteFn write,
        void* write_context);

    /// Seals every open extent that holds bytes and starts its write, leaving a
    /// fresh open extent per namespace for later appends. An open extent with
    /// no bytes is left as it is, so repeated seals allocate nothing. No-op
    /// once a write failed, which it then reports.
    dstd::Expected<void, dstd::Error> seal_all();

    /// Blocks until no extent write is in flight and returns the first write
    /// error, if any. Must not run on a thread of the owning executor.
    dstd::Expected<void, dstd::Error> wait_idle();

    /// The same wait for a coroutine: suspends until no extent write is in
    /// flight and resumes on the owning executor, returning the first write
    /// error. Call it from a thread of that executor.
    coro::CoroTask<dstd::Expected<void, dstd::Error>> wait_idle_async();

    std::uint32_t extent_size() const noexcept { return extent_size_; }

    /// True while an extent write is in flight, whose completion releases
    /// bytes, so a caller under back pressure need not seal a partial extent.
    bool writing();

   private:
    friend struct ExtentWriterTestHook;

    struct OpenExtent;
    struct NamespaceSlot;
    struct PendingWrite;
    struct OpenSegment;
    struct IdleWait;

    NamespaceSlot* find_slot(std::uint32_t namespace_id, PageCodec codec);
    OpenExtent* publish_extent(NamespaceSlot& slot, OpenExtent* expected);
    /// The dictionary an extent of `slot` compresses with, or null. Adds the
    /// pages to the namespace sample and starts training as an executor task
    /// once it is full. After DICT_WAIT_BYTES sealed while it trained, the seal
    /// finishes it. A failed training or dictionary write ends it for the
    /// namespace.
    dstd::SharedPtr<const PageDict> page_dict(
        NamespaceSlot& slot, const std::uint8_t* raw, std::uint32_t used,
        dstd::Span<const std::uint32_t> starts);
    static void push_segment(OpenExtent* extent, OpenSegment segment);
    void leave_extent(OpenExtent* extent);
    bool seal_extent(OpenExtent* extent);
    void start_extent_write(OpenExtent* extent);
    /// Retires a sealed extent whose writers have all left. The extent is
    /// deleted with the writer.
    void retire(OpenExtent* extent);
    dstd::Expected<void, dstd::Error> append_run(std::uint32_t namespace_id,
                                                 dstd::ByteView segment_bytes,
                                                 ExtentSegment info,
                                                 void* context,
                                                 PageCodec codec);
    void submit(PendingWrite* pending);
    void resubmit(PendingWrite* pending);
    /// Counts one write in flight. Called before a seal becomes visible, and
    /// by each write that starts without a seal, so that no wait can return in
    /// the window between the seal of an extent and the submit of its write.
    void begin_write();
    void release_in_flight();
    void discard(OpenExtent* extent);
    void latch_error(int error_number);
    dstd::Error latched_error() const;
    static void on_write_done(void* context, ssize_t result) noexcept;

    DataFileWriter& file_;
    io::IoBackend& backend_;
    Executor& executor_;
    std::uint32_t extent_size_ = 0;
    WrittenExtentFn on_written_ = nullptr;
    void* on_written_context_ = nullptr;
    DiscardedContextFn on_discarded_ = nullptr;
    IdleFn on_idle_ = nullptr;

    std::atomic<NamespaceSlot*> namespaces_{nullptr};
    std::mutex namespaces_mutex_;
    dstd::Vector<NamespaceSlot*> slots_;
    dstd::Vector<OpenExtent*> retired_;
    std::mutex retired_mutex_;

    std::atomic<int> latched_errno_{0};

    std::mutex idle_mutex_;
    std::condition_variable idle_cv_;
    dstd::Vector<std::coroutine_handle<>> idle_waiters_;
    std::uint64_t in_flight_ = 0;
};

/// Test-only access to the writer's open and retired extents.
struct ExtentWriterTestHook {
    /// Seals the open extent of a namespace and leaves it in the slot, as an
    /// appender meets it when another thread has not installed the next yet.
    static bool seal_in_place(ExtentWriter& writer, std::uint32_t namespace_id);

    static std::size_t retired_bytes(ExtentWriter& writer);

    static std::size_t retired_extents(ExtentWriter& writer);
};

}  // namespace dftracer::utils::store::engine

#endif  // DFTRACER_UTILS_STORE_OPEN_EXTENT_H
