#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/index/store/engine/data_file.h>
#include <dftracer/utils/index/store/engine/open_page.h>
#include <dftracer/utils/index/store/engine/section.h>
#include <dftracer/utils/runtime/io/thread_pool_backend.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
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

namespace dftracer::utils::index::store::engine {

struct DataFileWriterTestHook {
    /// Swaps the writer's descriptor for a read-only one on the same file, so
    /// every pwrite fails with EBADF.
    static void reopen_read_only(DataFileWriter& writer,
                                 const std::string& path) {
        if (writer.fd_ >= 0) ::close(writer.fd_);
        writer.fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    }
};

}  // namespace dftracer::utils::index::store::engine

namespace {

namespace dstd = dftracer::utils::dstd;
namespace engine = dftracer::utils::index::store::engine;
namespace compress = dftracer::utils::utilities::fileio::compress;
namespace io = dftracer::utils::io;
namespace fs = std::filesystem;

using dftracer::utils::ByteView;
using dftracer::utils::Executor;
using dftracer::utils::Runtime;
using engine::DataFileWriter;
using engine::PageSection;
using engine::PageWriter;
using engine::SectionBuilder;
using engine::SectionView;
using engine::WrittenPage;

constexpr std::uint32_t FOUR_KIB = 4096;
constexpr std::uint32_t SIXTY_FOUR_KIB = 64 * 1024;
constexpr std::uint32_t EIGHT_KIB = 8 * 1024;
constexpr std::chrono::milliseconds PAGE_WRITE_DEADLINE{5000};

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("dftu_store_open_page_" + std::to_string(::getpid()));
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

/// Collects the pages the writer reports from its I/O completion threads.
struct Collector {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<WrittenPage> pages;

    void add(WrittenPage&& page) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            pages.push_back(std::move(page));
        }
        cv.notify_all();
    }

    bool wait_for(std::size_t count, std::chrono::milliseconds limit) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, limit, [&] { return pages.size() >= count; });
    }

    std::vector<WrittenPage> take() {
        std::lock_guard<std::mutex> lock(mutex);
        return std::move(pages);
    }
};

void collect_written(void* context, WrittenPage&& page) {
    static_cast<Collector*>(context)->add(std::move(page));
}

/// Counts one discarded context; pass the counter itself as the section
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

/// A sealed section whose single entry names `id` and whose value is
/// `value_bytes` long and derived from `id`.
std::vector<std::uint8_t> make_section(std::uint32_t id,
                                       std::size_t value_bytes) {
    std::uint8_t key[8];
    write_id(key, id);
    std::vector<std::uint8_t> value(value_bytes);
    write_id(value.data(), id);
    for (std::size_t i = 8; i < value.size(); ++i) {
        value[i] = static_cast<std::uint8_t>((id + i) & 0xFFU);
    }
    SectionBuilder builder;
    builder.add(ByteView(key, sizeof(key)),
                ByteView(value.data(), value.size()));
    auto sealed = builder.seal(nullptr, nullptr, nullptr);
    REQUIRE(sealed.has_value());
    return std::vector<std::uint8_t>(sealed->begin(), sealed->end());
}

/// A section-shaped payload of exactly `length` bytes that names `id`.
std::vector<std::uint8_t> make_payload(std::uint32_t id, std::size_t length) {
    std::vector<std::uint8_t> bytes(length, 0);
    write_id(bytes.data(), id);
    for (std::size_t i = 8; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>((id * 31U + i) & 0xFFU);
    }
    return bytes;
}

void check_payload(const std::vector<std::uint8_t>& page_bytes,
                   const PageSection& section, std::uint32_t id) {
    REQUIRE(section.offset_in_page + section.length <= page_bytes.size());
    const std::uint8_t* at = page_bytes.data() + section.offset_in_page;
    CHECK(read_id(at) == id);
    for (std::size_t i = 8; i < section.length; ++i) {
        CHECK(at[i] == static_cast<std::uint8_t>((id * 31U + i) & 0xFFU));
    }
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

/// Ascending pages, none of them overlapping another.
void check_page_layout(std::vector<WrittenPage>& pages,
                       std::uint32_t page_size) {
    std::sort(pages.begin(), pages.end(),
              [](const WrittenPage& a, const WrittenPage& b) {
                  return a.offset < b.offset;
              });
    for (std::size_t i = 0; i < pages.size(); ++i) {
        CHECK(pages[i].offset % page_size == 0);
        CHECK(pages[i].length % FOUR_KIB == 0);
        CHECK(pages[i].length <= page_size);
        CHECK(!pages[i].sections.empty());
        CHECK(pages[i].contexts.size() == pages[i].sections.size());
        if (i != 0) {
            CHECK(pages[i].offset >= pages[i - 1].offset + pages[i - 1].length);
        }
    }
}

/// Checks every page against the file and reports how often each section id
/// was found. `ids` bounds the ids the caller appended.
void check_pages_on_disk(int fd, const std::vector<WrittenPage>& pages,
                         std::vector<int>& seen) {
    for (const WrittenPage& page : pages) {
        const std::vector<std::uint8_t> bytes =
            read_range(fd, page.offset, page.length);
        CHECK(compress::gzip_crc32(0, bytes.data(), bytes.size()) == page.crc);
        std::uint64_t previous_end = 0;
        for (const PageSection& section : page.sections) {
            CHECK(section.offset_in_page >= previous_end);
            previous_end = section.offset_in_page + section.length;
            CHECK(previous_end <= page.length);
            const ByteView view(bytes.data() + section.offset_in_page,
                                section.length);
            auto opened = SectionView::open(view);
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

PageSection section_info(std::uint32_t copy_id, std::uint64_t entry_count) {
    PageSection info;
    info.copy_id = copy_id;
    info.entry_count = entry_count;
    return info;
}

}  // namespace

TEST_CASE("store open page: one thread fills one namespace") {
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
    PageWriter writer(*file, backend, *runtime.executor(), SIXTY_FOUR_KIB,
                      &collect_written, &collector);

    constexpr std::uint32_t SECTIONS = 1000;
    for (std::uint32_t id = 0; id < SECTIONS; ++id) {
        const std::vector<std::uint8_t> bytes = make_section(id, 400);
        auto appended = writer.append(1, ByteView(bytes.data(), bytes.size()),
                                      section_info(7, 1), nullptr);
        REQUIRE(appended.has_value());
    }
    REQUIRE(writer.seal_all().has_value());
    REQUIRE(writer.wait_idle().has_value());

    std::vector<WrittenPage> pages = collector.take();
    REQUIRE(!pages.empty());
    check_page_layout(pages, SIXTY_FOUR_KIB);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    std::vector<int> seen(SECTIONS, 0);
    check_pages_on_disk(fd, pages, seen);
    for (std::uint32_t id = 0; id < SECTIONS; ++id) {
        CHECK(seen[id] == 1);
    }
    ::close(fd);
}

TEST_CASE("store open page: many threads across three namespaces") {
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
    PageWriter writer(*file, backend, *runtime.executor(), SIXTY_FOUR_KIB,
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
                const std::vector<std::uint8_t> bytes = make_section(id, 40);
                auto appended = writer.append(
                    namespace_id, ByteView(bytes.data(), bytes.size()),
                    section_info(namespace_id + 1, 1), nullptr);
                if (!appended) ++failures;
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    CHECK(failures.load() == 0);

    REQUIRE(writer.seal_all().has_value());
    REQUIRE(writer.wait_idle().has_value());

    std::vector<WrittenPage> pages = collector.take();
    REQUIRE(!pages.empty());
    check_page_layout(pages, SIXTY_FOUR_KIB);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    const std::uint32_t total = THREADS * PER_THREAD;
    std::vector<int> seen(total, 0);
    check_pages_on_disk(fd, pages, seen);
    for (std::uint32_t id = 0; id < total; ++id) {
        CHECK(seen[id] == 1);
    }
    ::close(fd);

    for (const WrittenPage& page : pages) {
        const std::uint32_t namespace_id = page.sections.front().copy_id - 1;
        for (const PageSection& section : page.sections) {
            CHECK(section.copy_id == namespace_id + 1);
        }
    }
}

TEST_CASE("store open page: a section of three pages is written alone") {
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
    PageWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                      &collect_written, &collector);

    const std::size_t length = 3U * EIGHT_KIB;
    const std::vector<std::uint8_t> payload = make_payload(11, length);
    REQUIRE(writer
                .append(1, ByteView(payload.data(), payload.size()),
                        section_info(1, 5), nullptr)
                .has_value());
    REQUIRE(writer.seal_all().has_value());
    REQUIRE(writer.wait_idle().has_value());

    std::vector<WrittenPage> pages = collector.take();
    REQUIRE(pages.size() == 1);
    const WrittenPage& page = pages.front();
    CHECK(page.offset % EIGHT_KIB == 0);
    CHECK(page.length == length);
    REQUIRE(page.sections.size() == 1);
    CHECK(page.sections.front().offset_in_page == 0);
    CHECK(page.sections.front().length == length);
    CHECK(page.sections.front().entry_count == 5);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    const std::vector<std::uint8_t> bytes =
        read_range(fd, page.offset, page.length);
    CHECK(compress::gzip_crc32(0, bytes.data(), bytes.size()) == page.crc);
    CHECK(bytes == payload);
    ::close(fd);
}

TEST_CASE(
    "store open page: an exact fit fills the page and one more seals it") {
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
    PageWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                      &collect_written, &collector);

    const std::size_t first = EIGHT_KIB - 64;
    std::vector<std::uint8_t> a = make_payload(1, first);
    std::vector<std::uint8_t> b = make_payload(2, 64);
    std::vector<std::uint8_t> c(1, 0x5AU);

    int context_a = 0;
    int context_b = 0;
    int context_c = 0;
    REQUIRE(writer
                .append(1, ByteView(a.data(), a.size()), section_info(1, 1),
                        &context_a)
                .has_value());
    REQUIRE(writer
                .append(1, ByteView(b.data(), b.size()), section_info(1, 1),
                        &context_b)
                .has_value());
    REQUIRE(writer
                .append(1, ByteView(c.data(), c.size()), section_info(1, 1),
                        &context_c)
                .has_value());
    REQUIRE(writer.seal_all().has_value());
    REQUIRE(writer.wait_idle().has_value());

    std::vector<WrittenPage> pages = collector.take();
    check_page_layout(pages, EIGHT_KIB);
    REQUIRE(pages.size() == 2);

    const WrittenPage& full = pages[0];
    REQUIRE(full.sections.size() == 2);
    CHECK(full.sections[0].offset_in_page == 0);
    CHECK(full.sections[0].length == first);
    CHECK(full.sections[1].offset_in_page == first);
    CHECK(full.sections[1].length == 64);
    CHECK(full.length == EIGHT_KIB);
    REQUIRE(full.contexts.size() == 2);
    CHECK(full.contexts[0] == &context_a);
    CHECK(full.contexts[1] == &context_b);

    const WrittenPage& rest = pages[1];
    REQUIRE(rest.sections.size() == 1);
    CHECK(rest.sections[0].offset_in_page == 0);
    CHECK(rest.sections[0].length == 1);
    CHECK(rest.length == FOUR_KIB);
    REQUIRE(rest.contexts.size() == 1);
    CHECK(rest.contexts[0] == &context_c);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    const std::vector<std::uint8_t> full_bytes =
        read_range(fd, full.offset, full.length);
    check_payload(full_bytes, full.sections[0], 1);
    check_payload(full_bytes, full.sections[1], 2);
    const std::vector<std::uint8_t> rest_bytes =
        read_range(fd, rest.offset, rest.length);
    CHECK(rest_bytes[rest.sections[0].offset_in_page] == 0x5AU);
    ::close(fd);
}

TEST_CASE("store open page: a failed write latches and reports no page") {
    Runtime runtime(4);
    auto* executor = runtime.executor();
    REQUIRE(executor->has_io_backend());
    auto& backend = executor->io_backend();

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());
    engine::DataFileWriterTestHook::reopen_read_only(*file, file_path.string());

    Collector collector;
    std::atomic<std::size_t> discarded{0};
    PageWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                      &collect_written, &collector, &count_discarded);

    const std::vector<std::uint8_t> payload = make_payload(1, 1024);
    REQUIRE(writer
                .append(1, ByteView(payload.data(), payload.size()),
                        section_info(1, 1), &discarded)
                .has_value());
    REQUIRE(writer.seal_all().has_value());

    auto idle = writer.wait_idle();
    REQUIRE_FALSE(idle.has_value());
    CHECK(idle.error().is(dstd::Condition::Io));
    CHECK(collector.take().empty());
    // The page write failed, so no record will ever name that section and the
    // failure path is the only one that can hand its context back.
    CHECK(discarded.load() == 1);

    auto later = writer.append(21, ByteView(payload.data(), payload.size()),
                               section_info(1, 1), nullptr);
    REQUIRE_FALSE(later.has_value());
    CHECK(later.error().is(dstd::Condition::Io));
    auto sealed = writer.seal_all();
    REQUIRE_FALSE(sealed.has_value());
    CHECK(sealed.error().is(dstd::Condition::Io));
}

TEST_CASE("store open page: the pages no write took hand back their contexts") {
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
        PageWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                          &collect_written, &collector, &count_discarded);
        constexpr std::uint32_t SECTIONS = 3;
        for (std::uint32_t id = 0; id < SECTIONS; ++id) {
            const std::vector<std::uint8_t> bytes = make_section(id, 120);
            REQUIRE(writer
                        .append(1, ByteView(bytes.data(), bytes.size()),
                                section_info(1, 1), &discarded)
                        .has_value());
        }
        CHECK(discarded.load() == 0);
    }
    // Nothing sealed the page, so its contexts are the writer's to hand back.
    CHECK(discarded.load() == 3);
    CHECK(collector.take().empty());
}

TEST_CASE(
    "store open page: the file hands out aligned ranges from one counter") {
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
    auto page = file->append_page(ByteView(payload.data(), payload.size()));
    REQUIRE(page.has_value());
    CHECK(page->offset == 2 * SIXTY_FOUR_KIB);
    CHECK(page->length == payload.size());
    auto big = file->append_big(ByteView(payload.data(), payload.size()));
    REQUIRE(big.has_value());
    CHECK(big->offset == 2 * SIXTY_FOUR_KIB + FOUR_KIB);
}

TEST_CASE("store open page: stress a small page from many threads") {
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
    PageWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
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
                const std::vector<std::uint8_t> bytes = make_section(id, 80);
                auto appended =
                    writer.append(1, ByteView(bytes.data(), bytes.size()),
                                  section_info(1, 1), nullptr);
                if (!appended) ++failures;
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    CHECK(failures.load() == 0);

    REQUIRE(writer.seal_all().has_value());
    REQUIRE(writer.wait_idle().has_value());

    std::vector<WrittenPage> pages = collector.take();
    REQUIRE(!pages.empty());
    check_page_layout(pages, EIGHT_KIB);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    const std::uint32_t total = THREADS * PER_THREAD;
    std::vector<int> seen(total, 0);
    check_pages_on_disk(fd, pages, seen);
    for (std::uint32_t id = 0; id < total; ++id) {
        CHECK(seen[id] == 1);
    }
    ::close(fd);
}

TEST_CASE("store open page: a short write is finished with resubmissions") {
    Runtime runtime(1);
    FaultBackend backend(*runtime.executor(), FaultBackend::Fault::ShortWrite);

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());

    Collector collector;
    PageWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                      &collect_written, &collector);

    const std::vector<std::uint8_t> bytes = make_section(3, 200);
    REQUIRE(writer
                .append(1, ByteView(bytes.data(), bytes.size()),
                        section_info(1, 1), nullptr)
                .has_value());
    REQUIRE(writer.seal_all().has_value());
    REQUIRE(writer.wait_idle().has_value());
    CHECK(backend.completions() > 1);

    std::vector<WrittenPage> pages = collector.take();
    REQUIRE(pages.size() == 1);
    CHECK(pages.front().length == FOUR_KIB);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    std::vector<int> seen(4, 0);
    check_pages_on_disk(fd, pages, seen);
    CHECK(seen[3] == 1);
    ::close(fd);
}

TEST_CASE("store open page: an interrupted write is retried in full") {
    Runtime runtime(1);
    FaultBackend backend(*runtime.executor(), FaultBackend::Fault::EintrOnce);

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());

    Collector collector;
    PageWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                      &collect_written, &collector);

    const std::vector<std::uint8_t> bytes = make_section(4, 300);
    REQUIRE(writer
                .append(1, ByteView(bytes.data(), bytes.size()),
                        section_info(1, 1), nullptr)
                .has_value());
    REQUIRE(writer.seal_all().has_value());
    REQUIRE(writer.wait_idle().has_value());
    CHECK(backend.completions() > 1);

    std::vector<WrittenPage> pages = collector.take();
    REQUIRE(pages.size() == 1);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    std::vector<int> seen(5, 0);
    check_pages_on_disk(fd, pages, seen);
    CHECK(seen[4] == 1);
    ::close(fd);
}

TEST_CASE("store open page: a zero write result latches an Io error") {
    Runtime runtime(1);
    FaultBackend backend(*runtime.executor(), FaultBackend::Fault::ZeroOnce);

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());

    Collector collector;
    PageWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                      &collect_written, &collector);

    const std::vector<std::uint8_t> bytes = make_section(5, 128);
    REQUIRE(writer
                .append(1, ByteView(bytes.data(), bytes.size()),
                        section_info(1, 1), nullptr)
                .has_value());
    REQUIRE(writer.seal_all().has_value());

    auto idle = writer.wait_idle();
    REQUIRE_FALSE(idle.has_value());
    CHECK(idle.error().is(dstd::Condition::Io));
    CHECK(collector.take().empty());
}

TEST_CASE("store open page: a page write needs the backend flush") {
    Runtime runtime(1);
    FaultBackend backend(*runtime.executor(), FaultBackend::Fault::None);

    TempDir dir;
    const fs::path file_path = dir.path / "data.dfs";
    auto file =
        DataFileWriter::create(file_path.string(), EIGHT_KIB, "host", "label");
    REQUIRE(file.has_value());

    Collector collector;
    PageWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                      &collect_written, &collector);

    const std::vector<std::uint8_t> bytes = make_section(6, 96);
    REQUIRE(writer
                .append(1, ByteView(bytes.data(), bytes.size()),
                        section_info(1, 1), nullptr)
                .has_value());
    REQUIRE(writer.seal_all().has_value());
    REQUIRE(collector.wait_for(1, PAGE_WRITE_DEADLINE));
    REQUIRE(writer.wait_idle().has_value());

    std::vector<WrittenPage> pages = collector.take();
    REQUIRE(pages.size() == 1);
    CHECK(pages.front().length == FOUR_KIB);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    std::vector<int> seen(7, 0);
    check_pages_on_disk(fd, pages, seen);
    CHECK(seen[6] == 1);
    ::close(fd);
}

TEST_CASE("store open page: a section of exactly one page is written alone") {
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
    PageWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                      &collect_written, &collector);

    const std::vector<std::uint8_t> payload = make_payload(11, EIGHT_KIB);
    REQUIRE(writer
                .append(1, ByteView(payload.data(), payload.size()),
                        section_info(1, 5), nullptr)
                .has_value());
    REQUIRE(writer.wait_idle().has_value());

    std::vector<WrittenPage> pages = collector.take();
    REQUIRE(pages.size() == 1);
    const WrittenPage& page = pages.front();
    CHECK(page.offset % EIGHT_KIB == 0);
    CHECK(page.length == EIGHT_KIB);
    REQUIRE(page.sections.size() == 1);
    CHECK(page.sections.front().offset_in_page == 0);
    CHECK(page.sections.front().length == EIGHT_KIB);
    CHECK(page.sections.front().entry_count == 5);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    const std::vector<std::uint8_t> bytes =
        read_range(fd, page.offset, page.length);
    CHECK(compress::gzip_crc32(0, bytes.data(), bytes.size()) == page.crc);
    CHECK(bytes == payload);
    ::close(fd);
}

TEST_CASE("store open page: appends concurrent with seal_all land once") {
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
    PageWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
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
                const std::vector<std::uint8_t> bytes = make_section(id, 40);
                auto appended =
                    writer.append(1, ByteView(bytes.data(), bytes.size()),
                                  section_info(1, 1), nullptr);
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

    std::vector<WrittenPage> pages = collector.take();
    REQUIRE(!pages.empty());
    check_page_layout(pages, EIGHT_KIB);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    const std::uint32_t total = APPENDERS * PER_THREAD;
    std::vector<int> seen(total, 0);
    check_pages_on_disk(fd, pages, seen);
    for (std::uint32_t id = 0; id < total; ++id) {
        CHECK(seen[id] == 1);
    }
    ::close(fd);
}

TEST_CASE("store open page: an append on a sealed page goes to the next page") {
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
    PageWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                      &collect_written, &collector);

    const std::vector<std::uint8_t> first = make_payload(1, 300);
    const std::vector<std::uint8_t> second = make_payload(2, 300);
    REQUIRE(writer
                .append(1, ByteView(first.data(), first.size()),
                        section_info(1, 1), nullptr)
                .has_value());
    REQUIRE(engine::PageWriterTestHook::seal_in_place(writer, 1));
    REQUIRE(writer
                .append(1, ByteView(second.data(), second.size()),
                        section_info(1, 1), nullptr)
                .has_value());
    REQUIRE(writer.seal_all().has_value());
    REQUIRE(writer.wait_idle().has_value());

    std::vector<WrittenPage> pages = collector.take();
    REQUIRE(pages.size() == 2);
    check_page_layout(pages, EIGHT_KIB);
    REQUIRE(pages[0].sections.size() == 1);
    REQUIRE(pages[1].sections.size() == 1);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    const std::vector<std::uint8_t> first_bytes =
        read_range(fd, pages[0].offset, pages[0].length);
    check_payload(first_bytes, pages[0].sections[0], 1);
    const std::vector<std::uint8_t> second_bytes =
        read_range(fd, pages[1].offset, pages[1].length);
    check_payload(second_bytes, pages[1].sections[0], 2);
    ::close(fd);
}

TEST_CASE("store open page: sealing an empty page holds no buffer") {
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
    PageWriter writer(*file, backend, *runtime.executor(), EIGHT_KIB,
                      &collect_written, &collector);

    const std::vector<std::uint8_t> bytes = make_section(7, 120);
    REQUIRE(writer
                .append(1, ByteView(bytes.data(), bytes.size()),
                        section_info(1, 1), nullptr)
                .has_value());

    constexpr int SEALS = 2000;
    for (int i = 0; i < SEALS; ++i) {
        REQUIRE(writer.seal_all().has_value());
    }
    CHECK(engine::PageWriterTestHook::retired_bytes(writer) <
          2 * std::size_t{EIGHT_KIB});
    // Sealing an empty page retires nothing, so 2000 seals leave only the page
    // that held the append.
    CHECK(engine::PageWriterTestHook::retired_pages(writer) <= 2);

    REQUIRE(writer.wait_idle().has_value());
    std::vector<WrittenPage> pages = collector.take();
    REQUIRE(pages.size() == 1);

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    std::vector<int> seen(8, 0);
    check_pages_on_disk(fd, pages, seen);
    CHECK(seen[7] == 1);
    ::close(fd);
}
