#include <dftracer/utils/index/store/engine/open_page.h>
#include <dftracer/utils/runtime/coro/completion_latch.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>

#include <algorithm>
#include <cerrno>
#include <coroutine>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <utility>

namespace dftracer::utils::index::store::engine {

namespace {

namespace compress = dftracer::utils::utilities::fileio::compress;

/// Bits 0..31 of the page state word: bytes used of the page.
constexpr std::uint64_t OPEN_PAGE_BUMP_MASK = 0xFFFFFFFFULL;
/// Bits 32..47: appenders currently inside the page, at most 0xFFFF of them.
constexpr std::uint64_t OPEN_PAGE_WRITER_ONE = 1ULL << 32U;
constexpr std::uint64_t OPEN_PAGE_WRITER_MASK = 0xFFFFULL << 32U;
constexpr std::uint64_t OPEN_PAGE_MAX_WRITERS = 0xFFFFULL;
/// Bit 63: no appender may reserve space in the page any more.
constexpr std::uint64_t OPEN_PAGE_SEALED = 1ULL << 63U;
constexpr std::uint32_t OPEN_PAGE_WRITE_ALIGN = 4096;

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    const std::uint64_t remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
}

dstd::Unexpected<dstd::Error> invalid_error() {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::InvalidArgument));
}

}  // namespace

struct PageWriter::OpenSection {
    PageSection section;
    void* context = nullptr;
};

struct PageWriter::OpenPage {
    OpenPage(std::uint32_t open_namespace_id, std::uint32_t bytes)
        : namespace_id(open_namespace_id), buffer(bytes, 0) {}

    std::uint32_t namespace_id = 0;
    std::atomic<std::uint64_t> state{0};
    std::atomic_flag sections_lock = ATOMIC_FLAG_INIT;
    dstd::Vector<std::uint8_t> buffer;
    dstd::Vector<OpenSection> sections;
};

struct PageWriter::NamespaceSlot {
    std::uint32_t id = 0;
    std::atomic<OpenPage*> page{nullptr};
    NamespaceSlot* next = nullptr;
};

struct PageWriter::PendingWrite {
    PageWriter* owner = nullptr;
    WrittenPage written;
    dstd::Vector<std::uint8_t> buffer;
    std::size_t written_bytes = 0;
};

PageWriter::PageWriter(DataFileWriter& file, io::IoBackend& backend,
                       Executor& executor, std::uint32_t page_size,
                       WrittenPageFn on_written, void* context,
                       DiscardedContextFn on_discarded, IdleFn on_idle)
    : file_(file),
      backend_(backend),
      executor_(executor),
      page_size_(page_size),
      on_written_(on_written),
      on_written_context_(context),
      on_discarded_(on_discarded),
      on_idle_(on_idle) {}

PageWriter::~PageWriter() {
    for (NamespaceSlot* slot : slots_) {
        OpenPage* page = slot->page.load(std::memory_order_acquire);
        if (page != nullptr) retire(page);
        delete slot;
    }
    for (OpenPage* page : retired_) {
        discard(page);
        delete page;
    }
}

void PageWriter::discard(OpenPage* page) {
    if (on_discarded_ == nullptr) return;
    for (OpenSection& open : page->sections) on_discarded_(open.context);
    page->sections.clear();
}

PageWriter::NamespaceSlot* PageWriter::find_slot(std::uint32_t namespace_id) {
    for (NamespaceSlot* slot = namespaces_.load(std::memory_order_acquire);
         slot != nullptr; slot = slot->next) {
        if (slot->id == namespace_id) return slot;
    }

    std::lock_guard<std::mutex> lock(namespaces_mutex_);
    for (NamespaceSlot* slot = namespaces_.load(std::memory_order_relaxed);
         slot != nullptr; slot = slot->next) {
        if (slot->id == namespace_id) return slot;
    }
    auto* slot = new NamespaceSlot;
    slot->id = namespace_id;
    slot->next = namespaces_.load(std::memory_order_relaxed);
    namespaces_.store(slot, std::memory_order_release);
    slots_.push_back(slot);
    return slot;
}

PageWriter::OpenPage* PageWriter::publish_page(NamespaceSlot& slot,
                                               OpenPage* expected) {
    OpenPage* current = slot.page.load(std::memory_order_acquire);
    if (current != expected) return current;
    auto* fresh = new OpenPage(slot.id, page_size_);
    if (slot.page.compare_exchange_strong(expected, fresh,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
        return fresh;
    }
    // A failed exchange leaves the page another thread installed in
    // `expected`, so the caller retries on it.
    delete fresh;
    return expected;
}

void PageWriter::push_section(OpenPage* page, OpenSection section) {
    while (page->sections_lock.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    page->sections.push_back(std::move(section));
    page->sections_lock.clear(std::memory_order_release);
}

void PageWriter::leave_page(OpenPage* page) {
    std::uint64_t state = page->state.load(std::memory_order_acquire);
    for (;;) {
        const std::uint64_t next = state - OPEN_PAGE_WRITER_ONE;
        if (page->state.compare_exchange_weak(state, next,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            if ((next & OPEN_PAGE_SEALED) != 0 &&
                (next & OPEN_PAGE_WRITER_MASK) == 0) {
                start_page_write(page);
            }
            return;
        }
    }
}

void PageWriter::retire(OpenPage* page) {
    std::lock_guard<std::mutex> lock(retired_mutex_);
    retired_.push_back(page);
}

bool PageWriter::seal_page(OpenPage* page) {
    std::uint64_t state = page->state.load(std::memory_order_acquire);
    if ((state & OPEN_PAGE_SEALED) != 0) return false;
    // Counted before the seal is visible, so that a wait which observes the
    // sealed page also observes this write: the crc and the range of that
    // write are still to be computed.
    begin_write();
    for (;;) {
        if ((state & OPEN_PAGE_SEALED) != 0) {
            release_in_flight();
            return false;
        }
        const std::uint64_t writers = (state & OPEN_PAGE_WRITER_MASK) >> 32U;
        if (writers == OPEN_PAGE_MAX_WRITERS) {
            std::this_thread::yield();
            state = page->state.load(std::memory_order_acquire);
            continue;
        }
        if (page->state.compare_exchange_weak(
                state, (state + OPEN_PAGE_WRITER_ONE) | OPEN_PAGE_SEALED,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
}

void PageWriter::latch_error(int error_number) {
    int expected = 0;
    latched_errno_.compare_exchange_strong(expected, error_number,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire);
}

dstd::Error PageWriter::latched_error() const {
    return dstd::Error(
        dstd::Condition::Io,
        "store page write failed with errno " +
            std::to_string(latched_errno_.load(std::memory_order_acquire)));
}

void PageWriter::begin_write() {
    std::lock_guard<std::mutex> lock(idle_mutex_);
    ++in_flight_;
}

void PageWriter::release_in_flight() {
    dstd::Vector<std::coroutine_handle<>> waiters;
    {
        std::lock_guard<std::mutex> lock(idle_mutex_);
        --in_flight_;
        // Under the lock, so wait_idle() cannot observe the zero count, return
        // and let the writer be destroyed while this notify is still running.
        if (in_flight_ == 0) {
            waiters.swap(idle_waiters_);
            if (on_idle_ != nullptr) on_idle_(on_written_context_);
        }
        idle_cv_.notify_all();
    }
    for (std::coroutine_handle<> waiter : waiters) {
        coro::resume_continuation(&executor_, waiter);
    }
}

void PageWriter::submit(PendingWrite* pending) {
    backend_.submit_pwrite_callback(file_.fd(), pending->buffer.data(),
                                    pending->buffer.size(),
                                    static_cast<off_t>(pending->written.offset),
                                    &PageWriter::on_write_done, pending);
    backend_.flush();
}

void PageWriter::resubmit(PendingWrite* pending) {
    const std::size_t done = pending->written_bytes;
    backend_.submit_pwrite_callback(
        file_.fd(), pending->buffer.data() + done,
        pending->buffer.size() - done,
        static_cast<off_t>(pending->written.offset + done),
        &PageWriter::on_write_done, pending);
    backend_.flush();
}

void PageWriter::on_write_done(void* context, ssize_t result) noexcept {
    auto* pending = static_cast<PendingWrite*>(context);
    PageWriter* writer = pending->owner;
    if (result == -EINTR) {
        writer->resubmit(pending);
        return;
    }
    if (result < 0 || result == 0) {
        // A zero result with bytes left is not progress towards a full write.
        writer->latch_error(result < 0 ? static_cast<int>(-result) : EIO);
        if (writer->on_discarded_ != nullptr) {
            for (void* section_context : pending->written.contexts) {
                writer->on_discarded_(section_context);
            }
        }
        delete pending;
        writer->release_in_flight();
        return;
    }

    pending->written_bytes += static_cast<std::size_t>(result);
    if (pending->written_bytes < pending->buffer.size()) {
        writer->resubmit(pending);
        return;
    }

    WrittenPage written = std::move(pending->written);
    delete pending;
    if (writer->on_written_ != nullptr) {
        writer->on_written_(writer->on_written_context_, std::move(written));
    }
    writer->release_in_flight();
}

void PageWriter::start_page_write(OpenPage* page) {
    const std::uint64_t state = page->state.load(std::memory_order_acquire);
    const std::uint32_t used =
        static_cast<std::uint32_t>(state & OPEN_PAGE_BUMP_MASK);
    if (used == 0) {
        retire(page);
        release_in_flight();
        return;
    }

    const std::uint32_t length =
        static_cast<std::uint32_t>(align_up(used, OPEN_PAGE_WRITE_ALIGN));
    auto* pending = new PendingWrite;
    pending->owner = this;
    pending->written.offset = file_.reserve_range(length, page_size_);
    pending->written.length = length;
    pending->buffer = std::move(page->buffer);
    pending->buffer.resize(length);
    pending->written.crc =
        compress::gzip_crc32(0, pending->buffer.data(), length);
    pending->written.sections.reserve(page->sections.size());
    pending->written.contexts.reserve(page->sections.size());
    std::sort(page->sections.begin(), page->sections.end(),
              [](const OpenSection& a, const OpenSection& b) {
                  return a.section.offset_in_page < b.section.offset_in_page;
              });
    for (OpenSection& open : page->sections) {
        pending->written.sections.push_back(std::move(open.section));
        pending->written.contexts.push_back(open.context);
    }
    page->sections.clear();
    retire(page);
    submit(pending);
}

dstd::Expected<void, dstd::Error> PageWriter::append_run(ByteView section_bytes,
                                                         PageSection info,
                                                         void* context) {
    if (section_bytes.size() >
        std::numeric_limits<std::uint32_t>::max() - OPEN_PAGE_WRITE_ALIGN) {
        return invalid_error();
    }
    const std::uint32_t length = static_cast<std::uint32_t>(
        align_up(section_bytes.size(), OPEN_PAGE_WRITE_ALIGN));
    auto* pending = new PendingWrite;
    pending->owner = this;
    const auto* begin =
        reinterpret_cast<const std::uint8_t*>(section_bytes.data());
    pending->buffer.assign(begin, begin + section_bytes.size());
    pending->buffer.resize(length);
    pending->written.offset = file_.reserve_range(length, page_size_);
    pending->written.length = length;
    pending->written.crc =
        compress::gzip_crc32(0, pending->buffer.data(), length);
    info.offset_in_page = 0;
    info.length = static_cast<std::uint32_t>(section_bytes.size());
    pending->written.sections.push_back(std::move(info));
    pending->written.contexts.push_back(context);
    begin_write();
    submit(pending);
    return {};
}

dstd::Expected<void, dstd::Error> PageWriter::append(std::uint32_t namespace_id,
                                                     ByteView section_bytes,
                                                     PageSection info,
                                                     void* context) {
    const int latched = latched_errno_.load(std::memory_order_acquire);
    if (latched != 0) return dstd::Unexpected<dstd::Error>(latched_error());
    if (section_bytes.empty() ||
        section_bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        return invalid_error();
    }
    if (section_bytes.size() >= page_size_) {
        return append_run(section_bytes, std::move(info), context);
    }

    NamespaceSlot* slot = find_slot(namespace_id);
    const std::uint32_t length =
        static_cast<std::uint32_t>(section_bytes.size());
    for (;;) {
        OpenPage* page = slot->page.load(std::memory_order_acquire);
        if (page == nullptr) page = publish_page(*slot, nullptr);

        std::uint64_t state = page->state.load(std::memory_order_acquire);
        for (;;) {
            if ((state & OPEN_PAGE_SEALED) != 0) break;
            const std::uint64_t used = state & OPEN_PAGE_BUMP_MASK;
            const std::uint64_t writers =
                (state & OPEN_PAGE_WRITER_MASK) >> 32U;
            if (used + length > page_size_ ||
                writers == OPEN_PAGE_MAX_WRITERS) {
                break;
            }
            if (page->state.compare_exchange_weak(
                    state, state + length + OPEN_PAGE_WRITER_ONE,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                std::memcpy(page->buffer.data() + used, section_bytes.data(),
                            length);
                info.offset_in_page = static_cast<std::uint32_t>(used);
                info.length = length;
                OpenSection open;
                open.section = std::move(info);
                open.context = context;
                push_section(page, std::move(open));
                leave_page(page);
                return {};
            }
        }

        // Sealing counts as a writer, so whoever leaves last writes the page,
        // and that is this thread when no appender is inside.
        if (seal_page(page)) leave_page(page);
        publish_page(*slot, page);
    }
}

dstd::Expected<void, dstd::Error> PageWriter::seal_all() {
    const int latched = latched_errno_.load(std::memory_order_acquire);
    if (latched != 0) return dstd::Unexpected<dstd::Error>(latched_error());
    for (NamespaceSlot* slot = namespaces_.load(std::memory_order_acquire);
         slot != nullptr; slot = slot->next) {
        OpenPage* page = slot->page.load(std::memory_order_acquire);
        if (page == nullptr) continue;
        // An empty page has nothing to write, and sealing it would install a
        // fresh buffer while retaining this one for no gain.
        if ((page->state.load(std::memory_order_acquire) &
             OPEN_PAGE_BUMP_MASK) == 0) {
            continue;
        }
        if (seal_page(page)) leave_page(page);
        publish_page(*slot, page);
    }
    return {};
}

dstd::Expected<void, dstd::Error> PageWriter::wait_idle() {
    {
        std::unique_lock<std::mutex> lock(idle_mutex_);
        idle_cv_.wait(lock, [this] { return in_flight_ == 0; });
    }
    const int latched = latched_errno_.load(std::memory_order_acquire);
    if (latched != 0) return dstd::Unexpected<dstd::Error>(latched_error());
    return {};
}

/// Suspends the calling coroutine while a page write is in flight. The last
/// completion resumes it through the writer's executor, under the same lock
/// the check takes, so a completion that lands first is not lost.
struct PageWriter::IdleWait {
    PageWriter& writer;

    bool await_ready() const {
        std::lock_guard<std::mutex> lock(writer.idle_mutex_);
        return writer.in_flight_ == 0;
    }

    bool await_suspend(std::coroutine_handle<> handle) {
        std::lock_guard<std::mutex> lock(writer.idle_mutex_);
        if (writer.in_flight_ == 0) return false;
        writer.idle_waiters_.push_back(handle);
        return true;
    }

    dstd::Expected<void, dstd::Error> await_resume() const {
        const int latched =
            writer.latched_errno_.load(std::memory_order_acquire);
        if (latched != 0) {
            return dstd::Unexpected<dstd::Error>(writer.latched_error());
        }
        return {};
    }
};

coro::CoroTask<dstd::Expected<void, dstd::Error>>
PageWriter::wait_idle_async() {
    IdleWait idle{*this};
    co_return co_await idle;
}

bool PageWriterTestHook::seal_in_place(PageWriter& writer,
                                       std::uint32_t namespace_id) {
    PageWriter::NamespaceSlot* slot = writer.find_slot(namespace_id);
    PageWriter::OpenPage* page = slot->page.load(std::memory_order_acquire);
    if (page == nullptr) return false;
    if (!writer.seal_page(page)) return false;
    writer.leave_page(page);
    return true;
}

std::size_t PageWriterTestHook::retired_bytes(PageWriter& writer) {
    std::lock_guard<std::mutex> lock(writer.retired_mutex_);
    std::size_t bytes = 0;
    for (PageWriter::OpenPage* page : writer.retired_) {
        bytes += page->buffer.size() +
                 page->sections.size() * sizeof(PageWriter::OpenSection);
    }
    return bytes;
}

std::size_t PageWriterTestHook::retired_pages(PageWriter& writer) {
    std::lock_guard<std::mutex> lock(writer.retired_mutex_);
    return writer.retired_.size();
}

bool PageWriter::writing() {
    std::lock_guard<std::mutex> lock(idle_mutex_);
    return in_flight_ != 0;
}

}  // namespace dftracer::utils::index::store::engine
