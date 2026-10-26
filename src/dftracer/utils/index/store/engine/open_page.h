#ifndef DFTRACER_UTILS_INDEX_STORE_ENGINE_OPEN_PAGE_H
#define DFTRACER_UTILS_INDEX_STORE_ENGINE_OPEN_PAGE_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/store/engine/data_file.h>
#include <dftracer/utils/index/store/engine/record.h>
#include <dftracer/utils/index/store/engine/section.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/io/io_backend.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dftracer::utils {

class Executor;

}  // namespace dftracer::utils

namespace dftracer::utils::index::store::engine {

/// One written block of a data file: a page of sections, or the run of a
/// section that does not fit in a page. `length` is the true length rounded up
/// to 4096 and is what the file holds at `offset`; the bytes past the last
/// section are zero, and `crc` covers all `length` bytes. `contexts` runs
/// parallel to `sections` and holds the context the appender gave for each.
struct WrittenPage {
    std::uint64_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t crc = 0;
    dstd::Vector<PageSection> sections;
    dstd::Vector<void*> contexts;
};

/// Receives a page after its write completed. Runs on an I/O backend thread,
/// so `context` must outlive the writer.
using WrittenPageFn = void (*)(void* context, WrittenPage&& page);

/// Receives the context of a section no WrittenPage will carry: the write of
/// its page failed, or its page was still open when the writer was destroyed.
/// Runs on the appending thread or on an I/O backend thread, so it must free
/// nothing the caller still uses.
using DiscardedContextFn = void (*)(void* context);

/// Called when the last page write in flight completes, after its
/// WrittenPageFn, with the WrittenPageFn context. Runs on an I/O backend
/// thread while the writer holds its idle lock, so it must not call back into
/// the PageWriter.
using IdleFn = void (*)(void* context);

/// Fills the open page of each namespace with sealed sections and writes a
/// page through the runtime I/O backend once it is sealed and its last writer
/// has left.
///
/// append() and seal_all() are safe from many threads at once: appending
/// reserves its bytes with one compare-and-swap on the page state and holds a
/// per-page spin lock only to record the section, and sealing takes the
/// writer's mutexes. wait_idle() blocks the calling thread and must not run on
/// a thread of the executor that owns the backend; wait_idle_async() is the
/// same wait for a coroutine of that executor.
///
/// Destroy the writer only after the in-flight writes reached zero, through
/// one of the two waits: a completion in flight still calls back into it. The
/// contexts of the pages that no write took then go to the discard callback.
class PageWriter {
   public:
    PageWriter(DataFileWriter& file, io::IoBackend& backend, Executor& executor,
               std::uint32_t page_size, WrittenPageFn on_written, void* context,
               DiscardedContextFn on_discarded = nullptr,
               IdleFn on_idle = nullptr);
    PageWriter(const PageWriter&) = delete;
    PageWriter& operator=(const PageWriter&) = delete;
    PageWriter(PageWriter&&) = delete;
    PageWriter& operator=(PageWriter&&) = delete;
    ~PageWriter();

    /// Copies a sealed section into the open page of `namespace_id` at the
    /// next free offset there, or into a run of its own when the section is a
    /// page long or longer. `info.offset_in_page` and `info.length` are set
    /// from the reservation and the rest of `info` is kept. The bytes are
    /// copied before this returns, so the caller's buffer may go away, and
    /// `context` comes back with the section in WrittenPage::contexts. An
    /// empty section is InvalidArgument, and every call returns the first
    /// write error once a page write failed.
    dstd::Expected<void, dstd::Error> append(std::uint32_t namespace_id,
                                             ByteView section_bytes,
                                             PageSection info, void* context);

    /// Seals every open page that holds bytes and starts its write, leaving a
    /// fresh open page per namespace for later appends. An open page with no
    /// bytes is left as it is, so repeated seals allocate nothing. No-op once
    /// a write failed, which it then reports.
    dstd::Expected<void, dstd::Error> seal_all();

    /// Blocks until no page write is in flight and returns the first write
    /// error, if any. Must not run on a thread of the owning executor.
    dstd::Expected<void, dstd::Error> wait_idle();

    /// The same wait for a coroutine: suspends until no page write is in
    /// flight and resumes on the owning executor, returning the first write
    /// error. Call it from a thread of that executor.
    coro::CoroTask<dstd::Expected<void, dstd::Error>> wait_idle_async();

    std::uint32_t page_size() const noexcept { return page_size_; }

    /// True while a page write is in flight. Its completion will release
    /// bytes, so a caller under back pressure need not seal a partial page.
    bool writing();

   private:
    friend struct PageWriterTestHook;

    struct OpenPage;
    struct NamespaceSlot;
    struct PendingWrite;
    struct OpenSection;
    struct IdleWait;

    NamespaceSlot* find_slot(std::uint32_t namespace_id);
    OpenPage* publish_page(NamespaceSlot& slot, OpenPage* expected);
    static void push_section(OpenPage* page, OpenSection section);
    void leave_page(OpenPage* page);
    bool seal_page(OpenPage* page);
    void start_page_write(OpenPage* page);
    /// Retires a sealed page whose writers have all left. Its buffer and
    /// section list are already moved out; the page is deleted with the writer.
    void retire(OpenPage* page);
    dstd::Expected<void, dstd::Error> append_run(ByteView section_bytes,
                                                 PageSection info,
                                                 void* context);
    void submit(PendingWrite* pending);
    void resubmit(PendingWrite* pending);
    /// Counts one write in flight. Called before a seal becomes visible, and
    /// by each write that starts without a seal, so that no wait can return in
    /// the window between the seal of a page and the submit of its write.
    void begin_write();
    void release_in_flight();
    void discard(OpenPage* page);
    void latch_error(int error_number);
    dstd::Error latched_error() const;
    static void on_write_done(void* context, ssize_t result) noexcept;

    DataFileWriter& file_;
    io::IoBackend& backend_;
    Executor& executor_;
    std::uint32_t page_size_ = 0;
    WrittenPageFn on_written_ = nullptr;
    void* on_written_context_ = nullptr;
    DiscardedContextFn on_discarded_ = nullptr;
    IdleFn on_idle_ = nullptr;

    std::atomic<NamespaceSlot*> namespaces_{nullptr};
    std::mutex namespaces_mutex_;
    dstd::Vector<NamespaceSlot*> slots_;
    dstd::Vector<OpenPage*> retired_;
    std::mutex retired_mutex_;

    std::atomic<int> latched_errno_{0};

    std::mutex idle_mutex_;
    std::condition_variable idle_cv_;
    dstd::Vector<std::coroutine_handle<>> idle_waiters_;
    std::uint64_t in_flight_ = 0;
};

/// Test-only access to the writer's open and retired pages. Defined in
/// open_page.cpp, where the open page layout is complete.
struct PageWriterTestHook {
    /// Seals the open page of a namespace and leaves it while the slot still
    /// points at it, which is the state an appender meets when another thread
    /// sealed the page and has not installed the next one yet.
    static bool seal_in_place(PageWriter& writer, std::uint32_t namespace_id);

    /// Bytes still held by the pages the writer retired and has not deleted.
    static std::size_t retired_bytes(PageWriter& writer);

    /// Pages retired so far, which stay at one per page written.
    static std::size_t retired_pages(PageWriter& writer);
};

}  // namespace dftracer::utils::index::store::engine

#endif  // DFTRACER_UTILS_INDEX_STORE_ENGINE_OPEN_PAGE_H
