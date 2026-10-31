#include <dftracer/utils/runtime/coro/completion_latch.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/store/crc.h>
#include <dftracer/utils/store/extent_codec.h>
#include <dftracer/utils/store/open_extent.h>

#include <algorithm>
#include <cerrno>
#include <coroutine>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace dftracer::utils::store::engine {

namespace {

/// Bits 0..31 of the extent state word: bytes used of the extent.
constexpr std::uint64_t OPEN_EXTENT_BUMP_MASK = 0xFFFFFFFFULL;
/// Bits 32..47: appenders currently inside the extent, at most 0xFFFF of them.
constexpr std::uint64_t OPEN_EXTENT_WRITER_ONE = 1ULL << 32U;
constexpr std::uint64_t OPEN_EXTENT_WRITER_MASK = 0xFFFFULL << 32U;
constexpr std::uint64_t OPEN_EXTENT_MAX_WRITERS = 0xFFFFULL;
/// Bit 63: no appender may reserve space in the extent any more.
constexpr std::uint64_t OPEN_EXTENT_SEALED = 1ULL << 63U;

/// After the first dictionary, one page in this many goes to the sample of the
/// second, so it learns from a wider stretch of the data at the same cost.
constexpr std::uint64_t DICT_RETRAIN_STRIDE = 8;
constexpr std::uint8_t DICT_STAGES = 2;
/// The page bytes a namespace seals while its dictionary trains before a seal
/// waits for the training, so a dictionary is never later than this.
constexpr std::uint64_t DICT_WAIT_BYTES = std::uint64_t{16} << 20;

/// One dictionary training, shared by the namespace and the executor task that
/// runs it. Whoever claims it first trains: the task, or a seal that cannot
/// wait. The task holds this alone, so the writer never waits for a task that
/// has not started.
struct DictTraining {
    std::mutex mutex;
    std::condition_variable done_cv;
    dstd::Vector<std::uint8_t> sample;
    dstd::Vector<std::size_t> sizes;
    dstd::Vector<std::uint8_t> result;
    bool claimed = false;
    bool done = false;
};

void run_training(DictTraining& job) {
    dstd::Vector<std::uint8_t> sample;
    dstd::Vector<std::size_t> sizes;
    {
        std::lock_guard<std::mutex> lock(job.mutex);
        if (job.claimed) return;
        job.claimed = true;
        sample = std::move(job.sample);
        sizes = std::move(job.sizes);
    }
    dstd::Vector<std::uint8_t> bytes;
    try {
        auto trained = train_page_dict(
            dstd::ByteView(reinterpret_cast<const std::byte*>(sample.data()),
                           sample.size()),
            dstd::Span<const std::size_t>(sizes.data(), sizes.size()));
        if (trained) bytes = std::move(*trained);
    } catch (const std::exception&) {
        bytes.clear();
    }
    sample = dstd::Vector<std::uint8_t>();
    {
        std::lock_guard<std::mutex> lock(job.mutex);
        job.result = std::move(bytes);
        job.done = true;
    }
    job.done_cv.notify_all();
}

coro::CoroTask<void> training_task(dstd::SharedPtr<DictTraining> job) {
    run_training(*job);
    co_return;
}

dstd::Unexpected<dstd::Error> invalid_error() {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::InvalidArgument));
}

}  // namespace

struct ExtentWriter::OpenSegment {
    ExtentSegment segment;
    void* context = nullptr;
};

struct ExtentWriter::OpenExtent {
    OpenExtent(std::uint32_t open_namespace_id, PageCodec open_codec,
               std::uint32_t bytes)
        : namespace_id(open_namespace_id),
          codec(open_codec),
          buffer(bytes, 0) {}

    std::uint32_t namespace_id = 0;
    PageCodec codec = PageCodec::Zstd;
    std::atomic<std::uint64_t> state{0};
    std::atomic_flag segments_lock = ATOMIC_FLAG_INIT;
    dstd::Vector<std::uint8_t> buffer;
    dstd::Vector<OpenSegment> segments;
};

struct ExtentWriter::NamespaceSlot {
    std::uint32_t id = 0;
    PageCodec codec = PageCodec::Zstd;
    std::atomic<OpenExtent*> extent{nullptr};
    NamespaceSlot* next = nullptr;

    std::mutex dict_mutex;
    dstd::SharedPtr<const PageDict> dict;
    dstd::Vector<std::uint8_t> sample;
    dstd::Vector<std::size_t> sample_sizes;
    std::uint64_t pages_seen = 0;
    std::uint8_t dict_stage = 0;
    dstd::SharedPtr<DictTraining> training;
    std::uint64_t sealed_while_training = 0;
};

struct ExtentWriter::PendingWrite {
    ExtentWriter* owner = nullptr;
    WrittenExtent written;
    dstd::Vector<std::uint8_t> buffer;
    std::size_t written_bytes = 0;
};

ExtentWriter::ExtentWriter(DataFileWriter& file, io::IoBackend& backend,
                           Executor& executor, std::uint32_t extent_size,
                           WrittenExtentFn on_written, void* context,
                           DiscardedContextFn on_discarded, IdleFn on_idle)
    : file_(file),
      backend_(backend),
      executor_(executor),
      extent_size_(extent_size),
      on_written_(on_written),
      on_written_context_(context),
      on_discarded_(on_discarded),
      on_idle_(on_idle) {}

ExtentWriter::~ExtentWriter() {
    for (NamespaceSlot* slot : slots_) {
        OpenExtent* extent = slot->extent.load(std::memory_order_acquire);
        if (extent != nullptr) retire(extent);
        delete slot;
    }
    for (OpenExtent* extent : retired_) {
        discard(extent);
        delete extent;
    }
}

void ExtentWriter::discard(OpenExtent* extent) {
    if (on_discarded_ == nullptr) return;
    for (OpenSegment& open : extent->segments) on_discarded_(open.context);
    extent->segments.clear();
}

ExtentWriter::NamespaceSlot* ExtentWriter::find_slot(std::uint32_t namespace_id,
                                                     PageCodec codec) {
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
    slot->codec = codec;
    slot->next = namespaces_.load(std::memory_order_relaxed);
    namespaces_.store(slot, std::memory_order_release);
    slots_.push_back(slot);
    return slot;
}

dstd::SharedPtr<const PageDict> ExtentWriter::page_dict(
    NamespaceSlot& slot, const std::uint8_t* raw, std::uint32_t used,
    dstd::Span<const std::uint32_t> starts) {
    dstd::SharedPtr<DictTraining> job;
    bool must_finish = false;
    bool started = false;
    {
        std::lock_guard<std::mutex> lock(slot.dict_mutex);
        if (slot.training != nullptr) {
            job = slot.training;
            slot.sealed_while_training += used;
            must_finish = slot.sealed_while_training >= DICT_WAIT_BYTES;
        } else {
            if (slot.dict_stage >= DICT_STAGES) return slot.dict;
            const std::size_t sample_bytes =
                g_dict_sample_bytes.load(std::memory_order_relaxed);
            for (std::size_t i = 0;
                 i < starts.size() && slot.sample.size() < sample_bytes; ++i) {
                const std::uint32_t begin = starts[i];
                const std::uint32_t end =
                    i + 1 < starts.size() ? starts[i + 1] : used;
                const bool keep = slot.dict_stage == 0 ||
                                  slot.pages_seen % DICT_RETRAIN_STRIDE == 0;
                ++slot.pages_seen;
                if (!keep) continue;
                // The sample doubles up to its full size, so a namespace that
                // never fills it holds no more than twice what it wrote.
                const std::size_t need = slot.sample.size() + (end - begin);
                if (need > slot.sample.capacity()) {
                    slot.sample.reserve(
                        std::min(std::max(need, slot.sample.capacity() * 2),
                                 sample_bytes + PAGE_BYTES));
                }
                slot.sample.insert(slot.sample.end(), raw + begin, raw + end);
                slot.sample_sizes.push_back(end - begin);
            }
            if (slot.sample.size() < sample_bytes) return slot.dict;
            // Training runs as an executor task, and extents sealed meanwhile
            // keep the dictionary they had. The seal that finds it done writes
            // the dictionary, while the data file is open.
            job = dstd::make_shared<DictTraining>();
            job->sample = std::move(slot.sample);
            job->sizes = std::move(slot.sample_sizes);
            slot.sample = dstd::Vector<std::uint8_t>();
            slot.sample_sizes = dstd::Vector<std::size_t>();
            slot.training = job;
            slot.sealed_while_training = 0;
            started = true;
        }
    }
    if (started) {
        // Just started. A task that never runs leaves the job to the seal that
        // must finish it.
        (void)spawn_detached(&executor_, training_task(job));
        std::lock_guard<std::mutex> lock(slot.dict_mutex);
        return slot.dict;
    }
    if (must_finish) {
        // The namespace outran its training: finish it here. A job no thread
        // claimed is trained on this one, and a claimed one runs elsewhere.
        run_training(*job);
        BlockingRegion blocking;
        std::unique_lock<std::mutex> lock(job->mutex);
        job->done_cv.wait(lock, [&job] { return job->done; });
    }
    bool done = false;
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        done = job->done;
    }
    dstd::Vector<std::uint8_t> ready;
    {
        // One seal takes the result: the one that still finds the job here.
        // The slot lock is taken before the job lock, as everywhere.
        std::lock_guard<std::mutex> lock(slot.dict_mutex);
        if (!done || slot.training != job) return slot.dict;
        slot.training = nullptr;
        std::lock_guard<std::mutex> job_lock(job->mutex);
        ready = std::move(job->result);
    }
    dstd::SharedPtr<const PageDict> made;
    if (!ready.empty()) {
        const dstd::ByteView view(
            reinterpret_cast<const std::byte*>(ready.data()), ready.size());
        auto ref = file_.append_dict(view);
        if (ref) {
            auto dict = PageDict::for_encode(view, *ref);
            if (dict) made = std::move(*dict);
        }
    }
    std::lock_guard<std::mutex> lock(slot.dict_mutex);
    if (made == nullptr) {
        slot.dict_stage = DICT_STAGES;
        return slot.dict;
    }
    ++slot.dict_stage;
    slot.dict = std::move(made);
    return slot.dict;
}

ExtentWriter::OpenExtent* ExtentWriter::publish_extent(NamespaceSlot& slot,
                                                       OpenExtent* expected) {
    OpenExtent* current = slot.extent.load(std::memory_order_acquire);
    if (current != expected) return current;
    auto* fresh = new OpenExtent(slot.id, slot.codec, extent_size_);
    if (slot.extent.compare_exchange_strong(expected, fresh,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
        return fresh;
    }
    // A failed exchange leaves the extent another thread installed in
    // `expected`, so the caller retries on it.
    delete fresh;
    return expected;
}

void ExtentWriter::push_segment(OpenExtent* extent, OpenSegment segment) {
    while (extent->segments_lock.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    extent->segments.push_back(std::move(segment));
    extent->segments_lock.clear(std::memory_order_release);
}

void ExtentWriter::leave_extent(OpenExtent* extent) {
    std::uint64_t state = extent->state.load(std::memory_order_acquire);
    for (;;) {
        const std::uint64_t next = state - OPEN_EXTENT_WRITER_ONE;
        if (extent->state.compare_exchange_weak(state, next,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            if ((next & OPEN_EXTENT_SEALED) != 0 &&
                (next & OPEN_EXTENT_WRITER_MASK) == 0) {
                start_extent_write(extent);
            }
            return;
        }
    }
}

void ExtentWriter::retire(OpenExtent* extent) {
    std::lock_guard<std::mutex> lock(retired_mutex_);
    retired_.push_back(extent);
}

bool ExtentWriter::seal_extent(OpenExtent* extent) {
    std::uint64_t state = extent->state.load(std::memory_order_acquire);
    if ((state & OPEN_EXTENT_SEALED) != 0) return false;
    // Counted before the seal is visible, so a wait that observes the sealed
    // extent also observes this write.
    begin_write();
    for (;;) {
        if ((state & OPEN_EXTENT_SEALED) != 0) {
            release_in_flight();
            return false;
        }
        const std::uint64_t writers = (state & OPEN_EXTENT_WRITER_MASK) >> 32U;
        if (writers == OPEN_EXTENT_MAX_WRITERS) {
            std::this_thread::yield();
            state = extent->state.load(std::memory_order_acquire);
            continue;
        }
        if (extent->state.compare_exchange_weak(
                state, (state + OPEN_EXTENT_WRITER_ONE) | OPEN_EXTENT_SEALED,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
}

void ExtentWriter::latch_error(int error_number) {
    int expected = 0;
    latched_errno_.compare_exchange_strong(expected, error_number,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire);
}

dstd::Error ExtentWriter::latched_error() const {
    return dstd::Error(
        dstd::Condition::Io,
        "store extent write failed with errno " +
            std::to_string(latched_errno_.load(std::memory_order_acquire)));
}

void ExtentWriter::begin_write() {
    std::lock_guard<std::mutex> lock(idle_mutex_);
    ++in_flight_;
}

void ExtentWriter::release_in_flight() {
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

void ExtentWriter::submit(PendingWrite* pending) {
    backend_.submit_pwrite_callback(file_.fd(), pending->buffer.data(),
                                    pending->buffer.size(),
                                    static_cast<off_t>(pending->written.offset),
                                    &ExtentWriter::on_write_done, pending);
    backend_.flush();
}

void ExtentWriter::resubmit(PendingWrite* pending) {
    const std::size_t done = pending->written_bytes;
    backend_.submit_pwrite_callback(
        file_.fd(), pending->buffer.data() + done,
        pending->buffer.size() - done,
        static_cast<off_t>(pending->written.offset + done),
        &ExtentWriter::on_write_done, pending);
    backend_.flush();
}

void ExtentWriter::on_write_done(void* context, ssize_t result) noexcept {
    auto* pending = static_cast<PendingWrite*>(context);
    ExtentWriter* writer = pending->owner;
    if (result == -EINTR) {
        writer->resubmit(pending);
        return;
    }
    if (result < 0 || result == 0) {
        // A zero result with bytes left is not progress towards a full write.
        writer->latch_error(result < 0 ? static_cast<int>(-result) : EIO);
        if (writer->on_discarded_ != nullptr) {
            for (void* segment_context : pending->written.contexts) {
                writer->on_discarded_(segment_context);
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

    WrittenExtent written = std::move(pending->written);
    delete pending;
    if (writer->on_written_ != nullptr) {
        writer->on_written_(writer->on_written_context_, std::move(written));
    }
    writer->release_in_flight();
}

void ExtentWriter::start_extent_write(OpenExtent* extent) {
    const std::uint64_t state = extent->state.load(std::memory_order_acquire);
    const std::uint32_t used =
        static_cast<std::uint32_t>(state & OPEN_EXTENT_BUMP_MASK);
    if (used == 0) {
        retire(extent);
        release_in_flight();
        return;
    }

    std::sort(extent->segments.begin(), extent->segments.end(),
              [](const OpenSegment& a, const OpenSegment& b) {
                  return a.segment.offset_in_extent <
                         b.segment.offset_in_extent;
              });
    dstd::Vector<SegmentSpan> spans;
    spans.reserve(extent->segments.size());
    const auto reject = [this, extent] {
        latch_error(EINVAL);
        discard(extent);
        retire(extent);
        release_in_flight();
    };
    for (const OpenSegment& open : extent->segments) {
        const auto* raw = extent->buffer.data() + open.segment.offset_in_extent;
        if (!append_segment_spans(
                dstd::ByteView{reinterpret_cast<const std::byte*>(raw),
                               open.segment.length},
                open.segment.offset_in_extent, spans)) {
            reject();
            return;
        }
    }
    const dstd::Vector<std::uint32_t> starts = page_starts(spans, used);
    dstd::SharedPtr<const PageDict> dict;
    if (extent->codec == PageCodec::Zstd) {
        dict = page_dict(*find_slot(extent->namespace_id, extent->codec),
                         extent->buffer.data(), used, starts);
    }
    auto stored = encode_extent(
        dstd::ByteView(
            reinterpret_cast<const std::byte*>(extent->buffer.data()), used),
        starts, EXTENT_ALIGN_BYTES, extent->codec, dict.get());
    if (!stored) {
        reject();
        return;
    }
    extent->buffer = dstd::Vector<std::uint8_t>();
    const std::uint32_t length = static_cast<std::uint32_t>(stored->size());
    auto* pending = new PendingWrite;
    pending->owner = this;
    pending->written.offset = file_.reserve_range(length, EXTENT_ALIGN_BYTES);
    pending->written.length = length;
    pending->buffer = std::move(*stored);
    pending->written.crc = crc32(0, pending->buffer.data(), length);
    pending->written.segments.reserve(extent->segments.size());
    pending->written.contexts.reserve(extent->segments.size());
    for (OpenSegment& open : extent->segments) {
        pending->written.segments.push_back(std::move(open.segment));
        pending->written.contexts.push_back(open.context);
    }
    extent->segments.clear();
    retire(extent);
    submit(pending);
}

dstd::Expected<void, dstd::Error> ExtentWriter::append_run(
    std::uint32_t namespace_id, dstd::ByteView segment_bytes,
    ExtentSegment info, void* context, PageCodec codec) {
    dstd::Vector<SegmentSpan> spans;
    if (!append_segment_spans(segment_bytes, 0, spans)) return invalid_error();
    const std::uint32_t length =
        static_cast<std::uint32_t>(segment_bytes.size());
    const dstd::Vector<std::uint32_t> starts = page_starts(spans, length);
    dstd::SharedPtr<const PageDict> dict;
    if (codec == PageCodec::Zstd) {
        dict = page_dict(
            *find_slot(namespace_id, codec),
            reinterpret_cast<const std::uint8_t*>(segment_bytes.data()), length,
            starts);
    }
    auto stored = encode_extent(segment_bytes, starts, EXTENT_ALIGN_BYTES,
                                codec, dict.get());
    if (!stored) return dstd::Unexpected<dstd::Error>(stored.error());
    const std::uint32_t stored_length =
        static_cast<std::uint32_t>(stored->size());
    auto* pending = new PendingWrite;
    pending->owner = this;
    pending->buffer = std::move(*stored);
    pending->written.offset =
        file_.reserve_range(stored_length, EXTENT_ALIGN_BYTES);
    pending->written.length = stored_length;
    pending->written.crc = crc32(0, pending->buffer.data(), stored_length);
    info.offset_in_extent = 0;
    info.length = length;
    pending->written.segments.push_back(std::move(info));
    pending->written.contexts.push_back(context);
    begin_write();
    submit(pending);
    return {};
}

dstd::Expected<void, dstd::Error> ExtentWriter::append(
    std::uint32_t namespace_id, dstd::ByteView segment_bytes,
    ExtentSegment info, void* context, PageCodec codec) {
    return append_with(
        namespace_id, segment_bytes.size(), std::move(info), context, codec,
        [](void* bytes, std::uint8_t* out) {
            const dstd::ByteView& view =
                *static_cast<const dstd::ByteView*>(bytes);
            std::memcpy(out, view.data(), view.size());
        },
        &segment_bytes);
}

dstd::Expected<void, dstd::Error> ExtentWriter::append_with(
    std::uint32_t namespace_id, std::size_t segment_length, ExtentSegment info,
    void* context, PageCodec codec, SegmentWriteFn write, void* write_context) {
    const int latched = latched_errno_.load(std::memory_order_acquire);
    if (latched != 0) return dstd::Unexpected<dstd::Error>(latched_error());
    if (segment_length == 0 ||
        segment_length > std::numeric_limits<std::uint32_t>::max()) {
        return invalid_error();
    }
    const std::size_t capacity = extent_size_ - EXTENT_HEADER_BYTES;
    if (segment_length >= capacity) {
        dstd::Vector<std::uint8_t> run(segment_length);
        write(write_context, run.data());
        return append_run(
            namespace_id,
            dstd::ByteView(reinterpret_cast<const std::byte*>(run.data()),
                           run.size()),
            std::move(info), context, codec);
    }

    NamespaceSlot* slot = find_slot(namespace_id, codec);
    const std::uint32_t length = static_cast<std::uint32_t>(segment_length);
    for (;;) {
        OpenExtent* extent = slot->extent.load(std::memory_order_acquire);
        if (extent == nullptr) extent = publish_extent(*slot, nullptr);

        std::uint64_t state = extent->state.load(std::memory_order_acquire);
        for (;;) {
            if ((state & OPEN_EXTENT_SEALED) != 0) break;
            const std::uint64_t used = state & OPEN_EXTENT_BUMP_MASK;
            const std::uint64_t writers =
                (state & OPEN_EXTENT_WRITER_MASK) >> 32U;
            if (used + length > capacity ||
                writers == OPEN_EXTENT_MAX_WRITERS) {
                break;
            }
            if (extent->state.compare_exchange_weak(
                    state, state + length + OPEN_EXTENT_WRITER_ONE,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                write(write_context, extent->buffer.data() + used);
                info.offset_in_extent = static_cast<std::uint32_t>(used);
                info.length = length;
                OpenSegment open;
                open.segment = std::move(info);
                open.context = context;
                push_segment(extent, std::move(open));
                leave_extent(extent);
                return {};
            }
        }

        // Sealing counts as a writer, so whoever leaves last writes the extent.
        if (seal_extent(extent)) leave_extent(extent);
        publish_extent(*slot, extent);
    }
}

dstd::Expected<void, dstd::Error> ExtentWriter::seal_all() {
    const int latched = latched_errno_.load(std::memory_order_acquire);
    if (latched != 0) return dstd::Unexpected<dstd::Error>(latched_error());
    for (NamespaceSlot* slot = namespaces_.load(std::memory_order_acquire);
         slot != nullptr; slot = slot->next) {
        OpenExtent* extent = slot->extent.load(std::memory_order_acquire);
        if (extent == nullptr) continue;
        // Sealing an empty extent would only install a fresh buffer.
        if ((extent->state.load(std::memory_order_acquire) &
             OPEN_EXTENT_BUMP_MASK) == 0) {
            continue;
        }
        if (seal_extent(extent)) leave_extent(extent);
        publish_extent(*slot, extent);
    }
    return {};
}

dstd::Expected<void, dstd::Error> ExtentWriter::wait_idle() {
    {
        std::unique_lock<std::mutex> lock(idle_mutex_);
        idle_cv_.wait(lock, [this] { return in_flight_ == 0; });
    }
    const int latched = latched_errno_.load(std::memory_order_acquire);
    if (latched != 0) return dstd::Unexpected<dstd::Error>(latched_error());
    return {};
}

/// Suspends the calling coroutine while an extent write is in flight. The last
/// completion resumes it through the writer's executor, under the same lock
/// the check takes, so a completion that lands first is not lost.
struct ExtentWriter::IdleWait {
    ExtentWriter& writer;

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
ExtentWriter::wait_idle_async() {
    IdleWait idle{*this};
    co_return co_await idle;
}

bool ExtentWriterTestHook::seal_in_place(ExtentWriter& writer,
                                         std::uint32_t namespace_id) {
    ExtentWriter::NamespaceSlot* slot =
        writer.find_slot(namespace_id, PageCodec::Zstd);
    ExtentWriter::OpenExtent* extent =
        slot->extent.load(std::memory_order_acquire);
    if (extent == nullptr) return false;
    if (!writer.seal_extent(extent)) return false;
    writer.leave_extent(extent);
    return true;
}

std::size_t ExtentWriterTestHook::retired_bytes(ExtentWriter& writer) {
    std::lock_guard<std::mutex> lock(writer.retired_mutex_);
    std::size_t bytes = 0;
    for (ExtentWriter::OpenExtent* extent : writer.retired_) {
        bytes += extent->buffer.size() +
                 extent->segments.size() * sizeof(ExtentWriter::OpenSegment);
    }
    return bytes;
}

std::size_t ExtentWriterTestHook::retired_extents(ExtentWriter& writer) {
    std::lock_guard<std::mutex> lock(writer.retired_mutex_);
    return writer.retired_.size();
}

bool ExtentWriter::writing() {
    std::lock_guard<std::mutex> lock(idle_mutex_);
    return in_flight_ != 0;
}

}  // namespace dftracer::utils::store::engine
