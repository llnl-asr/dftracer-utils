#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/runtime/io/thread_pool_backend.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/crc.h>
#include <dftracer/utils/store/data_file.h>
#include <dftracer/utils/store/extent_codec.h>
#include <dftracer/utils/store/open_extent.h>
#include <dftracer/utils/store/segment.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dftracer::utils::store {

struct DataFileWriterTestHook {
    /// Swaps the writer's descriptor for a read-only one on the same file, so
    /// every pwrite fails with EBADF.
    static void reopen_read_only(DataFileWriter& writer,
                                 const std::string& path) {
        if (writer.fd_ >= 0) ::close(writer.fd_);
        writer.fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    }
};

}  // namespace dftracer::utils::store

namespace {

namespace dstd = dftracer::utils::dstd;
namespace store = dftracer::utils::store;
namespace io = dftracer::utils::io;
namespace fs = std::filesystem;

using dftracer::utils::ByteView;
using dftracer::utils::Executor;
using dftracer::utils::Runtime;
using store::DataFileWriter;
using store::ExtentSegment;
using store::ExtentWriter;
using store::SegmentBuilder;
using store::SegmentView;
using store::WrittenExtent;

constexpr std::uint32_t FOUR_KIB = 4096;
constexpr std::uint32_t SIXTY_FOUR_KIB = 64 * 1024;
constexpr std::uint32_t EIGHT_KIB = 8 * 1024;
constexpr std::chrono::milliseconds EXTENT_WRITE_DEADLINE{5000};

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("dftu_store_open_extent_" + std::to_string(::getpid()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

/// Collects the extents the writer reports from its I/O completion threads.
struct Collector {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<WrittenExtent> extents;

    void add(WrittenExtent&& extent) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            extents.push_back(std::move(extent));
        }
        cv.notify_all();
    }

    bool wait_for(std::size_t count, std::chrono::milliseconds limit) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, limit,
                           [&] { return extents.size() >= count; });
    }

    std::vector<WrittenExtent> take() {
        std::lock_guard<std::mutex> lock(mutex);
        return std::move(extents);
    }
};

void collect_written(void* context, WrittenExtent&& extent) {
    static_cast<Collector*>(context)->add(std::move(extent));
}

/// Counts one discarded context; pass the counter itself as the segment
/// context.
void count_discarded(void* context) {
    static_cast<std::atomic<std::size_t>*>(context)->fetch_add(
        1, std::memory_order_relaxed);
}

/// An I/O backend that queues a positional write and runs it on the thread
/// that calls flush(), the way the thread pool runs a short batch, and that can
/// inject a short write, one -EINTR or one zero result.
class FaultBackend : public io::ThreadPoolBackend {
   public:
    enum class Fault { None, ShortWrite, EintrOnce, ZeroOnce };

    FaultBackend(Executor& executor, Fault fault)
        : io::ThreadPoolBackend(executor, 1, 0),
          fault_(fault),
          eintr_pending_(fault == Fault::EintrOnce),
          zero_pending_(fault == Fault::ZeroOnce) {}

    std::string name() const override { return "fault"; }

    void submit_pwrite_callback(int fd, const void* buf, std::size_t len,
                                off_t offset, io::IoCompletionFn completion,
                                void* context) override {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(Op{fd, buf, len, offset, completion, context});
    }

    int flush() override {
        std::vector<Op> ops;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ops.swap(pending_);
        }
        for (const Op& op : ops) run(op);
        return static_cast<int>(ops.size());
    }

    std::size_t completions() const {
        return completions_.load(std::memory_order_acquire);
    }

   private:
    struct Op {
        int fd;
        const void* buf;
        std::size_t len;
        off_t offset;
        io::IoCompletionFn completion;
        void* context;
    };

    static ssize_t pwrite_some(int fd, const void* buf, std::size_t len,
                               off_t offset) {
        const ssize_t written = ::pwrite(fd, buf, len, offset);
        return written < 0 ? static_cast<ssize_t>(-errno) : written;
    }

    void run(const Op& op) {
        ssize_t result = 0;
        switch (fault_) {
            case Fault::None:
                result = pwrite_some(op.fd, op.buf, op.len, op.offset);
                break;
            case Fault::ShortWrite: {
                const std::size_t half = op.len > 1 ? op.len / 2 : op.len;
                result = pwrite_some(op.fd, op.buf, half, op.offset);
                break;
            }
            case Fault::EintrOnce:
                result = eintr_pending_.exchange(false)
                             ? static_cast<ssize_t>(-EINTR)
                             : pwrite_some(op.fd, op.buf, op.len, op.offset);
                break;
            case Fault::ZeroOnce:
                result = zero_pending_.exchange(false)
                             ? static_cast<ssize_t>(0)
                             : pwrite_some(op.fd, op.buf, op.len, op.offset);
                break;
        }
        completions_.fetch_add(1, std::memory_order_acq_rel);
        op.completion(op.context, result);
    }

    Fault fault_;
    std::atomic<bool> eintr_pending_;
    std::atomic<bool> zero_pending_;
    std::atomic<std::size_t> completions_{0};
    std::mutex mutex_;
    std::vector<Op> pending_;
};

void write_id(std::uint8_t* out, std::uint32_t id) {
    dftracer::utils::write_u64_le(out, static_cast<std::uint64_t>(id));
}

std::uint32_t read_id(const std::uint8_t* in) {
    return static_cast<std::uint32_t>(dftracer::utils::read_u64_le(in));
}

/// A sealed segment whose single entry names `id` and whose value is
/// `value_bytes` long and derived from `id`.
std::vector<std::uint8_t> make_segment(std::uint32_t id,
                                       std::size_t value_bytes) {
    std::uint8_t key[8];
    write_id(key, id);
    std::vector<std::uint8_t> value(value_bytes);
    for (std::size_t i = 0; i < value.size(); ++i) {
        value[i] = static_cast<std::uint8_t>((id + i) & 0xFFU);
    }
    if (value.size() >= sizeof key) write_id(value.data(), id);
    SegmentBuilder builder;
    builder.add(ByteView(key, sizeof(key)),
                ByteView(value.data(), value.size()));
    auto sealed = builder.seal(nullptr, nullptr, nullptr);
    REQUIRE(sealed.has_value());
    return std::vector<std::uint8_t>(sealed->begin(), sealed->end());
}

/// A segment-shaped payload of exactly `length` bytes that names `id`.
std::vector<std::uint8_t> make_payload(std::uint32_t id, std::size_t length) {
    std::vector<std::uint8_t> bytes(length, 0);
    write_id(bytes.data(), id);
    for (std::size_t i = 8; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>((id * 31U + i) & 0xFFU);
    }
    return bytes;
}

/// A sealed segment of exactly `length` bytes that names `id`, empty when no
/// value length seals to it.
std::vector<std::uint8_t> make_segment_of(std::uint32_t id,
                                          std::size_t length) {
    std::size_t value_bytes = length > 128 ? length - 128 : 0;
    for (int attempt = 0; attempt < 32; ++attempt) {
        std::vector<std::uint8_t> bytes = make_segment(id, value_bytes);
        if (bytes.size() == length) return bytes;
        if (bytes.size() > length) {
            if (value_bytes == 0) return {};
            --value_bytes;
        } else {
            value_bytes += length - bytes.size();
        }
    }
    return {};
}

std::vector<std::uint8_t> read_range(int fd, std::uint64_t offset,
                                     std::size_t length) {
    std::vector<std::uint8_t> bytes(length);
    std::size_t done = 0;
    while (done < length) {
        const ssize_t read = ::pread(fd, bytes.data() + done, length - done,
                                     static_cast<off_t>(offset + done));
        REQUIRE(read > 0);
        done += static_cast<std::size_t>(read);
    }
    return bytes;
}

/// The raw bytes of one written extent: its stored bytes, checked against the
/// extent crc, then decoded.
std::vector<std::uint8_t> read_extent_raw(int fd, const WrittenExtent& extent) {
    const std::vector<std::uint8_t> stored =
        read_range(fd, extent.offset, extent.length);
    CHECK(store::crc32(0, stored.data(), stored.size()) == extent.crc);
    dstd::Vector<std::uint8_t> bytes(stored.begin(), stored.end());
    REQUIRE(store::decode_extent(bytes).has_value());
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

/// Ascending extents, none of them overlapping another.
void check_extent_layout(std::vector<WrittenExtent>& extents,
                         std::uint32_t extent_size) {
    std::sort(extents.begin(), extents.end(),
              [](const WrittenExtent& a, const WrittenExtent& b) {
                  return a.offset < b.offset;
              });
    for (std::size_t i = 0; i < extents.size(); ++i) {
        CHECK(extents[i].offset % store::EXTENT_ALIGN_BYTES == 0);
        CHECK(extents[i].length % FOUR_KIB == 0);
        CHECK(extents[i].length <= extent_size);
        CHECK(!extents[i].segments.empty());
        CHECK(extents[i].contexts.size() == extents[i].segments.size());
        if (i != 0) {
            CHECK(extents[i].offset >=
                  extents[i - 1].offset + extents[i - 1].length);
        }
    }
}

/// Checks every extent against the file and reports how often each segment id
/// was found. `ids` bounds the ids the caller appended.
void check_extents_on_disk(int fd, const std::vector<WrittenExtent>& extents,
                           std::vector<int>& seen) {
    for (const WrittenExtent& extent : extents) {
        const std::vector<std::uint8_t> bytes = read_extent_raw(fd, extent);
        std::uint64_t previous_end = 0;
        for (const ExtentSegment& segment : extent.segments) {
            CHECK(segment.offset_in_extent >= previous_end);
            previous_end = segment.offset_in_extent + segment.length;
            CHECK(previous_end <= bytes.size());
            const ByteView view(bytes.data() + segment.offset_in_extent,
                                segment.length);
            auto opened = SegmentView::open(view);
            REQUIRE(opened.has_value());
            CHECK(opened->entry_count() == 1);
            auto cursor = opened->lower_bound(ByteView{});
            REQUIRE(!cursor.at_end());
            const auto entry = cursor.entry();
            REQUIRE(entry.key().size() == 8);
            const std::uint32_t id = read_id(entry.key().as<std::uint8_t>());
            REQUIRE(entry.value().size() >= 8);
            CHECK(read_id(entry.value().as<std::uint8_t>()) == id);
            const std::uint8_t* value = entry.value().as<std::uint8_t>();
            for (std::size_t i = 8; i < entry.value().size(); ++i) {
                CHECK(value[i] == static_cast<std::uint8_t>((id + i) & 0xFFU));
            }
            REQUIRE(id < seen.size());
            ++seen[id];
        }
    }
}

ExtentSegment segment_info(std::uint32_t copy_id, std::uint64_t entry_count) {
    ExtentSegment info;
    info.copy_id = copy_id;
    info.entry_count = entry_count;
    return info;
}

}  // namespace

TEST_CASE("store open extent: one thread fills one namespace") {
    Runtime runtime(4);
    auto* executor = runtime.executor();
    REQUIRE(executor->has_io_backend());
    auto& backend = executor->io_backend();

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file = DataFileWriter::create(file_path.string(), SIXTY_FOUR_KIB,
                                       "host", "label");
    REQUIRE(file.has_value());
    CHECK(file->fd() >= 0);

    Collector collector;
    ExtentWriter writer(*file, backend, *runtime.executor(), SIXTY_FOUR_KIB,
                        &collect_written, &collector);

    constexpr std::uint32_t SEGMENTS = 1000;
    for (std::uint32_t id = 0; id < SEGMENTS; ++id) {
        const std::vector<std::uint8_t> bytes = make_segment(id, 400);
        auto appended = writer.append(1, ByteView(bytes.data(), bytes.size()),
                                      segment_info(7, 1), nullptr);
        REQUIRE(appended.has_value());
    }
    REQUIRE(writer.seal_all().has_value());
    REQUIRE(writer.wait_idle().has_value());

    std::vector<WrittenExtent> extents = collector.take();
    REQUIRE(!extents.empty());
    check_extent_layout(extents, SIXTY_FOUR_KIB);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    std::vector<int> seen(SEGMENTS, 0);
    check_extents_on_disk(fd, extents, seen);
    for (std::uint32_t id = 0; id < SEGMENTS; ++id) {
        CHECK(seen[id] == 1);
    }
    ::close(fd);
}

TEST_CASE("store open extent: many threads across three namespaces") {
    Runtime runtime(4);
    auto* executor = runtime.executor();
    REQUIRE(executor->has_io_backend());
    auto& backend = executor->io_backend();

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file = DataFileWriter::create(file_path.string(), SIXTY_FOUR_KIB,
                                       "host", "label");
    REQUIRE(file.has_value());

    Collector collector;
    ExtentWriter writer(*file, backend, *runtime.executor(), SIXTY_FOUR_KIB,
                        &collect_written, &collector);

    constexpr std::uint32_t THREADS = 16;
    constexpr std::uint32_t PER_THREAD = 2000;
    constexpr std::uint32_t NAMESPACES = 3;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (std::uint32_t t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            for (std::uint32_t i = 0; i < PER_THREAD; ++i) {
                const std::uint32_t id = t * PER_THREAD + i;
                const std::uint32_t namespace_id = id % NAMESPACES;
                const std::vector<std::uint8_t> bytes = make_segment(id, 40);
                auto appended = writer.append(
                    namespace_id, ByteView(bytes.data(), bytes.size()),
                    segment_info(namespace_id + 1, 1), nullptr);
                if (!appended) ++failures;
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    CHECK(failures.load() == 0);

    REQUIRE(writer.seal_all().has_value());
    REQUIRE(writer.wait_idle().has_value());

    std::vector<WrittenExtent> extents = collector.take();
    REQUIRE(!extents.empty());
    check_extent_layout(extents, SIXTY_FOUR_KIB);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    const std::uint32_t total = THREADS * PER_THREAD;
    std::vector<int> seen(total, 0);
    check_extents_on_disk(fd, extents, seen);
    for (std::uint32_t id = 0; id < total; ++id) {
        CHECK(seen[id] == 1);
    }
    ::close(fd);

    for (const WrittenExtent& extent : extents) {
        const std::uint32_t namespace_id = extent.segments.front().copy_id - 1;
        for (const ExtentSegment& segment : extent.segments) {
            CHECK(segment.copy_id == namespace_id + 1);
        }
    }
}

TEST_CASE("store open extent: a segment of three extents is written alone") {
    Runtime runtime(4);
    auto* executor = runtime.executor();
    REQUIRE(executor->has_io_backend());
    auto& backend = executor->io_backend();

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());

    Collector collector;
    ExtentWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                        &collect_written, &collector);

    const std::size_t length = 3U * EIGHT_KIB;
    const std::vector<std::uint8_t> payload = make_segment_of(11, length);
    REQUIRE(payload.size() == length);
    REQUIRE(writer
                .append(1, ByteView(payload.data(), payload.size()),
                        segment_info(1, 5), nullptr)
                .has_value());
    REQUIRE(writer.seal_all().has_value());
    REQUIRE(writer.wait_idle().has_value());

    std::vector<WrittenExtent> extents = collector.take();
    REQUIRE(extents.size() == 1);
    const WrittenExtent& extent = extents.front();
    CHECK(extent.offset % store::EXTENT_ALIGN_BYTES == 0);
    CHECK(extent.length % FOUR_KIB == 0);
    REQUIRE(extent.segments.size() == 1);
    CHECK(extent.segments.front().offset_in_extent == 0);
    CHECK(extent.segments.front().length == length);
    CHECK(extent.segments.front().entry_count == 5);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    const std::vector<std::uint8_t> bytes = read_extent_raw(fd, extent);
    CHECK(bytes == payload);
    ::close(fd);
}

TEST_CASE(
    "store open extent: an exact fit fills the extent and one more seals it") {
    Runtime runtime(4);
    auto* executor = runtime.executor();
    REQUIRE(executor->has_io_backend());
    auto& backend = executor->io_backend();

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());

    Collector collector;
    ExtentWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                        &collect_written, &collector);

    // The extent keeps room for the header of its stored form.
    const std::size_t first = EIGHT_KIB - store::EXTENT_HEADER_BYTES - 64;
    std::vector<std::uint8_t> a = make_segment_of(1, first);
    std::vector<std::uint8_t> b = make_segment_of(2, 64);
    std::vector<std::uint8_t> c = make_segment(3, 8);
    REQUIRE(a.size() == first);
    REQUIRE(b.size() == 64);

    int context_a = 0;
    int context_b = 0;
    int context_c = 0;
    REQUIRE(writer
                .append(1, ByteView(a.data(), a.size()), segment_info(1, 1),
                        &context_a)
                .has_value());
    REQUIRE(writer
                .append(1, ByteView(b.data(), b.size()), segment_info(1, 1),
                        &context_b)
                .has_value());
    REQUIRE(writer
                .append(1, ByteView(c.data(), c.size()), segment_info(1, 1),
                        &context_c)
                .has_value());
    REQUIRE(writer.seal_all().has_value());
    REQUIRE(writer.wait_idle().has_value());

    std::vector<WrittenExtent> extents = collector.take();
    check_extent_layout(extents, EIGHT_KIB);
    REQUIRE(extents.size() == 2);

    const WrittenExtent& full = extents[0];
    REQUIRE(full.segments.size() == 2);
    CHECK(full.segments[0].offset_in_extent == 0);
    CHECK(full.segments[0].length == first);
    CHECK(full.segments[1].offset_in_extent == first);
    CHECK(full.segments[1].length == 64);
    CHECK(full.length <= EIGHT_KIB);
    REQUIRE(full.contexts.size() == 2);
    CHECK(full.contexts[0] == &context_a);
    CHECK(full.contexts[1] == &context_b);

    const WrittenExtent& rest = extents[1];
    REQUIRE(rest.segments.size() == 1);
    CHECK(rest.segments[0].offset_in_extent == 0);
    CHECK(rest.segments[0].length == c.size());
    CHECK(rest.length == FOUR_KIB);
    REQUIRE(rest.contexts.size() == 1);
    CHECK(rest.contexts[0] == &context_c);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    std::vector<std::uint8_t> expected = a;
    expected.insert(expected.end(), b.begin(), b.end());
    const std::vector<std::uint8_t> full_bytes = read_extent_raw(fd, full);
    CHECK(full_bytes == expected);
    const std::vector<std::uint8_t> rest_bytes = read_extent_raw(fd, rest);
    CHECK(rest_bytes == c);
    ::close(fd);
}

TEST_CASE("store open extent: a failed write latches and reports no extent") {
    Runtime runtime(4);
    auto* executor = runtime.executor();
    REQUIRE(executor->has_io_backend());
    auto& backend = executor->io_backend();

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());
    store::DataFileWriterTestHook::reopen_read_only(*file, file_path.string());

    Collector collector;
    std::atomic<std::size_t> discarded{0};
    ExtentWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                        &collect_written, &collector, &count_discarded);

    const std::vector<std::uint8_t> payload = make_segment(1, 1024);
    REQUIRE(writer
                .append(1, ByteView(payload.data(), payload.size()),
                        segment_info(1, 1), &discarded)
                .has_value());
    REQUIRE(writer.seal_all().has_value());

    auto idle = writer.wait_idle();
    REQUIRE_FALSE(idle.has_value());
    CHECK(idle.error().is(dstd::Condition::Io));
    CHECK(collector.take().empty());
    // The extent write failed, so no record will ever name that segment and the
    // failure path is the only one that can hand its context back.
    CHECK(discarded.load() == 1);

    auto later = writer.append(21, ByteView(payload.data(), payload.size()),
                               segment_info(1, 1), nullptr);
    REQUIRE_FALSE(later.has_value());
    CHECK(later.error().is(dstd::Condition::Io));
    auto sealed = writer.seal_all();
    REQUIRE_FALSE(sealed.has_value());
    CHECK(sealed.error().is(dstd::Condition::Io));
}

TEST_CASE(
    "store open extent: the extents no write took hand back their contexts") {
    Runtime runtime(4);
    auto* executor = runtime.executor();
    REQUIRE(executor->has_io_backend());
    auto& backend = executor->io_backend();

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());

    Collector collector;
    std::atomic<std::size_t> discarded{0};
    {
        ExtentWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                            &collect_written, &collector, &count_discarded);
        constexpr std::uint32_t SEGMENTS = 3;
        for (std::uint32_t id = 0; id < SEGMENTS; ++id) {
            const std::vector<std::uint8_t> bytes = make_segment(id, 120);
            REQUIRE(writer
                        .append(1, ByteView(bytes.data(), bytes.size()),
                                segment_info(1, 1), &discarded)
                        .has_value());
        }
        CHECK(discarded.load() == 0);
    }
    // Nothing sealed the extent, so its contexts are the writer's to hand back.
    CHECK(discarded.load() == 3);
    CHECK(collector.take().empty());
}

TEST_CASE(
    "store open extent: the file hands out aligned ranges from one counter") {
    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file = DataFileWriter::create(file_path.string(), SIXTY_FOUR_KIB,
                                       "host", "label");
    REQUIRE(file.has_value());

    CHECK(file->reserve_range(10, FOUR_KIB) == FOUR_KIB);
    CHECK(file->reserve_range(20, FOUR_KIB) == 2 * FOUR_KIB);
    CHECK(file->reserve_range(100, SIXTY_FOUR_KIB) == SIXTY_FOUR_KIB);
    CHECK(file->reserve_range(20, FOUR_KIB) == SIXTY_FOUR_KIB + FOUR_KIB);

    const std::vector<std::uint8_t> payload = make_payload(4, 512);
    auto extent = file->append_extent(ByteView(payload.data(), payload.size()));
    REQUIRE(extent.has_value());
    // An extent starts at the next 4 KiB after the file end, not at the next
    // extent size, and its stored form fills whole 4 KiB blocks.
    CHECK(extent->offset == SIXTY_FOUR_KIB + 2 * FOUR_KIB);
    CHECK(extent->length == FOUR_KIB);
    auto big = file->append_big(ByteView(payload.data(), payload.size()));
    REQUIRE(big.has_value());
    CHECK(big->offset == SIXTY_FOUR_KIB + 3 * FOUR_KIB);
}

TEST_CASE("store open extent: stress a small extent from many threads") {
    Runtime runtime(4);
    auto* executor = runtime.executor();
    REQUIRE(executor->has_io_backend());
    auto& backend = executor->io_backend();

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());

    Collector collector;
    ExtentWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                        &collect_written, &collector);

    constexpr std::uint32_t THREADS = 32;
    constexpr std::uint32_t PER_THREAD = 1563;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (std::uint32_t t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            for (std::uint32_t i = 0; i < PER_THREAD; ++i) {
                const std::uint32_t id = t * PER_THREAD + i;
                const std::vector<std::uint8_t> bytes = make_segment(id, 80);
                auto appended =
                    writer.append(1, ByteView(bytes.data(), bytes.size()),
                                  segment_info(1, 1), nullptr);
                if (!appended) ++failures;
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    CHECK(failures.load() == 0);

    REQUIRE(writer.seal_all().has_value());
    REQUIRE(writer.wait_idle().has_value());

    std::vector<WrittenExtent> extents = collector.take();
    REQUIRE(!extents.empty());
    check_extent_layout(extents, EIGHT_KIB);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    const std::uint32_t total = THREADS * PER_THREAD;
    std::vector<int> seen(total, 0);
    check_extents_on_disk(fd, extents, seen);
    for (std::uint32_t id = 0; id < total; ++id) {
        CHECK(seen[id] == 1);
    }
    ::close(fd);
}

TEST_CASE("store open extent: a short write is finished with resubmissions") {
    Runtime runtime(1);
    FaultBackend backend(*runtime.executor(), FaultBackend::Fault::ShortWrite);

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());

    Collector collector;
    ExtentWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                        &collect_written, &collector);

    const std::vector<std::uint8_t> bytes = make_segment(3, 200);
    REQUIRE(writer
                .append(1, ByteView(bytes.data(), bytes.size()),
                        segment_info(1, 1), nullptr)
                .has_value());
    REQUIRE(writer.seal_all().has_value());
    REQUIRE(writer.wait_idle().has_value());
    CHECK(backend.completions() > 1);

    std::vector<WrittenExtent> extents = collector.take();
    REQUIRE(extents.size() == 1);
    CHECK(extents.front().length == FOUR_KIB);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    std::vector<int> seen(4, 0);
    check_extents_on_disk(fd, extents, seen);
    CHECK(seen[3] == 1);
    ::close(fd);
}

TEST_CASE("store open extent: an interrupted write is retried in full") {
    Runtime runtime(1);
    FaultBackend backend(*runtime.executor(), FaultBackend::Fault::EintrOnce);

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());

    Collector collector;
    ExtentWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                        &collect_written, &collector);

    const std::vector<std::uint8_t> bytes = make_segment(4, 300);
    REQUIRE(writer
                .append(1, ByteView(bytes.data(), bytes.size()),
                        segment_info(1, 1), nullptr)
                .has_value());
    REQUIRE(writer.seal_all().has_value());
    REQUIRE(writer.wait_idle().has_value());
    CHECK(backend.completions() > 1);

    std::vector<WrittenExtent> extents = collector.take();
    REQUIRE(extents.size() == 1);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    std::vector<int> seen(5, 0);
    check_extents_on_disk(fd, extents, seen);
    CHECK(seen[4] == 1);
    ::close(fd);
}

TEST_CASE("store open extent: a zero write result latches an Io error") {
    Runtime runtime(1);
    FaultBackend backend(*runtime.executor(), FaultBackend::Fault::ZeroOnce);

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());

    Collector collector;
    ExtentWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                        &collect_written, &collector);

    const std::vector<std::uint8_t> bytes = make_segment(5, 128);
    REQUIRE(writer
                .append(1, ByteView(bytes.data(), bytes.size()),
                        segment_info(1, 1), nullptr)
                .has_value());
    REQUIRE(writer.seal_all().has_value());

    auto idle = writer.wait_idle();
    REQUIRE_FALSE(idle.has_value());
    CHECK(idle.error().is(dstd::Condition::Io));
    CHECK(collector.take().empty());
}

TEST_CASE("store open extent: an extent write needs the backend flush") {
    Runtime runtime(1);
    FaultBackend backend(*runtime.executor(), FaultBackend::Fault::None);

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());

    Collector collector;
    ExtentWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                        &collect_written, &collector);

    const std::vector<std::uint8_t> bytes = make_segment(6, 96);
    REQUIRE(writer
                .append(1, ByteView(bytes.data(), bytes.size()),
                        segment_info(1, 1), nullptr)
                .has_value());
    REQUIRE(writer.seal_all().has_value());
    REQUIRE(collector.wait_for(1, EXTENT_WRITE_DEADLINE));
    REQUIRE(writer.wait_idle().has_value());

    std::vector<WrittenExtent> extents = collector.take();
    REQUIRE(extents.size() == 1);
    CHECK(extents.front().length == FOUR_KIB);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    std::vector<int> seen(7, 0);
    check_extents_on_disk(fd, extents, seen);
    CHECK(seen[6] == 1);
    ::close(fd);
}

TEST_CASE(
    "store open extent: a segment of exactly one extent is written alone") {
    Runtime runtime(4);
    auto* executor = runtime.executor();
    REQUIRE(executor->has_io_backend());
    auto& backend = executor->io_backend();

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());

    Collector collector;
    ExtentWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                        &collect_written, &collector);

    const std::vector<std::uint8_t> payload = make_segment_of(11, EIGHT_KIB);
    REQUIRE(payload.size() == EIGHT_KIB);
    REQUIRE(writer
                .append(1, ByteView(payload.data(), payload.size()),
                        segment_info(1, 5), nullptr)
                .has_value());
    REQUIRE(writer.wait_idle().has_value());

    std::vector<WrittenExtent> extents = collector.take();
    REQUIRE(extents.size() == 1);
    const WrittenExtent& extent = extents.front();
    CHECK(extent.offset % store::EXTENT_ALIGN_BYTES == 0);
    CHECK(extent.length % FOUR_KIB == 0);
    REQUIRE(extent.segments.size() == 1);
    CHECK(extent.segments.front().offset_in_extent == 0);
    CHECK(extent.segments.front().length == EIGHT_KIB);
    CHECK(extent.segments.front().entry_count == 5);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    const std::vector<std::uint8_t> bytes = read_extent_raw(fd, extent);
    CHECK(bytes == payload);
    ::close(fd);
}

TEST_CASE("store open extent: appends concurrent with seal_all land once") {
    Runtime runtime(4);
    auto* executor = runtime.executor();
    REQUIRE(executor->has_io_backend());
    auto& backend = executor->io_backend();

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());

    Collector collector;
    ExtentWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                        &collect_written, &collector);

    constexpr std::uint32_t APPENDERS = 8;
    constexpr std::uint32_t SEALERS = 2;
    constexpr std::uint32_t PER_THREAD = 1000;
    std::atomic<bool> appending{true};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(APPENDERS + SEALERS);
    for (std::uint32_t t = 0; t < APPENDERS; ++t) {
        threads.emplace_back([&, t] {
            for (std::uint32_t i = 0; i < PER_THREAD; ++i) {
                const std::uint32_t id = t * PER_THREAD + i;
                const std::vector<std::uint8_t> bytes = make_segment(id, 40);
                auto appended =
                    writer.append(1, ByteView(bytes.data(), bytes.size()),
                                  segment_info(1, 1), nullptr);
                if (!appended) ++failures;
            }
        });
    }
    for (std::uint32_t s = 0; s < SEALERS; ++s) {
        threads.emplace_back([&] {
            while (appending.load(std::memory_order_acquire)) {
                if (!writer.seal_all()) ++failures;
            }
        });
    }
    for (std::uint32_t t = 0; t < APPENDERS; ++t) threads[t].join();
    appending.store(false, std::memory_order_release);
    for (std::uint32_t s = 0; s < SEALERS; ++s) {
        threads[APPENDERS + s].join();
    }
    CHECK(failures.load() == 0);

    REQUIRE(writer.seal_all().has_value());
    REQUIRE(writer.wait_idle().has_value());

    std::vector<WrittenExtent> extents = collector.take();
    REQUIRE(!extents.empty());
    check_extent_layout(extents, EIGHT_KIB);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    const std::uint32_t total = APPENDERS * PER_THREAD;
    std::vector<int> seen(total, 0);
    check_extents_on_disk(fd, extents, seen);
    for (std::uint32_t id = 0; id < total; ++id) {
        CHECK(seen[id] == 1);
    }
    ::close(fd);
}

TEST_CASE(
    "store open extent: an append on a sealed extent goes to the next extent") {
    Runtime runtime(4);
    auto* executor = runtime.executor();
    REQUIRE(executor->has_io_backend());
    auto& backend = executor->io_backend();

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());

    Collector collector;
    ExtentWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                        &collect_written, &collector);

    const std::vector<std::uint8_t> first = make_segment(1, 300);
    const std::vector<std::uint8_t> second = make_segment(2, 300);
    REQUIRE(writer
                .append(1, ByteView(first.data(), first.size()),
                        segment_info(1, 1), nullptr)
                .has_value());
    REQUIRE(store::ExtentWriterTestHook::seal_in_place(writer, 1));
    REQUIRE(writer
                .append(1, ByteView(second.data(), second.size()),
                        segment_info(1, 1), nullptr)
                .has_value());
    REQUIRE(writer.seal_all().has_value());
    REQUIRE(writer.wait_idle().has_value());

    std::vector<WrittenExtent> extents = collector.take();
    REQUIRE(extents.size() == 2);
    check_extent_layout(extents, EIGHT_KIB);
    REQUIRE(extents[0].segments.size() == 1);
    REQUIRE(extents[1].segments.size() == 1);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    const std::vector<std::uint8_t> first_bytes =
        read_extent_raw(fd, extents[0]);
    CHECK(first_bytes == first);
    const std::vector<std::uint8_t> second_bytes =
        read_extent_raw(fd, extents[1]);
    CHECK(second_bytes == second);
    ::close(fd);
}

TEST_CASE("store open extent: sealing an empty extent holds no buffer") {
    Runtime runtime(4);
    auto* executor = runtime.executor();
    REQUIRE(executor->has_io_backend());
    auto& backend = executor->io_backend();

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());

    Collector collector;
    ExtentWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                        &collect_written, &collector);

    const std::vector<std::uint8_t> bytes = make_segment(7, 120);
    REQUIRE(writer
                .append(1, ByteView(bytes.data(), bytes.size()),
                        segment_info(1, 1), nullptr)
                .has_value());

    constexpr int SEALS = 2000;
    for (int i = 0; i < SEALS; ++i) {
        REQUIRE(writer.seal_all().has_value());
    }
    CHECK(store::ExtentWriterTestHook::retired_bytes(writer) <
          2 * std::size_t{EIGHT_KIB});
    // Sealing an empty extent retires nothing, so 2000 seals leave only the
    // extent that held the append.
    CHECK(store::ExtentWriterTestHook::retired_extents(writer) <= 2);

    REQUIRE(writer.wait_idle().has_value());
    std::vector<WrittenExtent> extents = collector.take();
    REQUIRE(extents.size() == 1);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    std::vector<int> seen(8, 0);
    check_extents_on_disk(fd, extents, seen);
    CHECK(seen[7] == 1);
    ::close(fd);
}
