// SPDX-License-Identifier: MIT
#include <dftracer/utils/arrow/internal.h>
#include <dftracer/utils/arrow/ipc.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/memory_budget.h>
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>
#include <sys/mman.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <utility>

namespace dftracer::utils::arrow {

namespace {

using dataframe::DataFrame;
using dataframe::Morsel;
using dataframe::MorselResult;

constexpr char FILE_MAGIC[] = "ARROW1";
/// An Arrow IPC file is at least magic(8) + footer_size(4) + magic(6).
constexpr std::size_t MIN_FILE_SIZE = 18;

std::atomic<std::uint64_t> g_peak_in_flight_bytes{0};

void note_in_flight_bytes(std::uint64_t bytes) {
    std::uint64_t peak = g_peak_in_flight_bytes.load(std::memory_order_relaxed);
    while (bytes > peak && !g_peak_in_flight_bytes.compare_exchange_weak(
                               peak, bytes, std::memory_order_relaxed)) {
    }
}

ArrowBufferView view_at(const std::uint8_t* data, std::int64_t offset,
                        std::int64_t size) {
    ArrowBufferView view;
    view.data.as_uint8 = data + offset;
    view.size_bytes = size;
    return view;
}

/// A memory mapping that outlives the reader, shared with every frame the
/// reader produced.
dstd::SharedPtr<void> map_file(const dstd::File& file, std::size_t size,
                               const std::uint8_t** out) {
    void* mapped =
        mmap(nullptr, size, PROT_READ, MAP_PRIVATE, file.native_handle(), 0);
    if (mapped == MAP_FAILED) return nullptr;
    madvise(mapped, size, MADV_SEQUENTIAL);
    *out = static_cast<const std::uint8_t*>(mapped);
    struct Mapping {
        void* data;
        std::size_t size;
    };
    auto* owner = dstd::make_unique<Mapping>().release();
    owner->data = mapped;
    owner->size = size;
    return dstd::adopt_shared_ptr(owner, [](Mapping* m) {
        munmap(m->data, m->size);
        dstd::UniquePtr<Mapping>(m).reset();
    });
}

}  // namespace

struct Reader::Impl {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    dstd::SharedPtr<void> keep;
    ArrowIpcDecoder* decoder = nullptr;
    ArrowSchema schema{};
    bool is_file = false;
    dstd::Vector<ArrowIpcFileBlock> blocks;
    std::size_t index = 0;
    std::size_t pos = 0;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
        if (decoder != nullptr) {
            ArrowIpcDecoderReset(decoder);
            dstd::UniquePtr<ArrowIpcDecoder>(decoder).reset();
        }
        if (schema.release != nullptr) schema.release(&schema);
    }

    bool start(const std::uint8_t* bytes, std::size_t n,
               dstd::SharedPtr<void> owner, dstd::StringView what,
               dstd::Result<void>* error);
    dstd::Result<dstd::Optional<DataFrame>> next_batch();
};

bool Reader::Impl::start(const std::uint8_t* bytes, std::size_t n,
                         dstd::SharedPtr<void> owner, dstd::StringView what,
                         dstd::Result<void>* error) {
    data = bytes;
    size = n;
    keep = std::move(owner);
    if (n < MIN_FILE_SIZE) {
        *error = common::make_error<"{}: {} bytes is too short for Arrow IPC">(
            common::ErrorCode::Parse, what, n);
        return false;
    }

    decoder = dstd::make_unique<ArrowIpcDecoder>().release();
    std::memset(decoder, 0, sizeof(ArrowIpcDecoder));
    if (ArrowIpcDecoderInit(decoder) != NANOARROW_OK) {
        *error = common::make_error<"{}: decoder init failed">(
            common::ErrorCode::Internal, what);
        return false;
    }
    ArrowError nano_error;
    nano_error.message[0] = '\0';

    is_file = std::memcmp(data, FILE_MAGIC, 6) == 0 &&
              std::memcmp(data + size - 6, FILE_MAGIC, 6) == 0;
    if (!is_file) {
        // The stream format: a schema message, then record batch messages.
        ArrowBufferView remaining =
            view_at(data, 0, static_cast<std::int64_t>(size));
        const int rc =
            ArrowIpcDecoderVerifyHeader(decoder, remaining, &nano_error);
        if (rc != NANOARROW_OK) {
            *error = common::make_error<"{}: no Arrow IPC schema message: {}">(
                common::ErrorCode::Parse, what, nano_error.message);
            return false;
        }
        const ArrowBufferView header =
            view_at(data, 0, decoder->header_size_bytes);
        if (ArrowIpcDecoderDecodeHeader(decoder, header, &nano_error) !=
                NANOARROW_OK ||
            ArrowIpcDecoderDecodeSchema(decoder, &schema, &nano_error) !=
                NANOARROW_OK) {
            *error = common::make_error<"{}: bad Arrow IPC schema: {}">(
                common::ErrorCode::Parse, what, nano_error.message);
            return false;
        }
        pos = static_cast<std::size_t>(decoder->header_size_bytes);
    } else {
        std::int32_t footer_size = 0;
        std::memcpy(&footer_size, data + size - 10, sizeof(footer_size));
        const std::int64_t total = static_cast<std::int64_t>(footer_size) + 10;
        if (footer_size <= 0 || total > static_cast<std::int64_t>(size) - 8) {
            *error = common::make_error<"{}: bad Arrow IPC footer size">(
                common::ErrorCode::Parse, what);
            return false;
        }
        const ArrowBufferView footer =
            view_at(data, static_cast<std::int64_t>(size) - total, total);
        if (ArrowIpcDecoderVerifyFooter(decoder, footer, &nano_error) !=
                NANOARROW_OK ||
            ArrowIpcDecoderDecodeFooter(decoder, footer, &nano_error) !=
                NANOARROW_OK) {
            *error = common::make_error<"{}: bad Arrow IPC footer: {}">(
                common::ErrorCode::Parse, what, nano_error.message);
            return false;
        }
        const ArrowIpcFooter* decoded = decoder->footer;
        const auto count =
            static_cast<std::size_t>(decoded->record_batch_blocks.size_bytes /
                                     sizeof(ArrowIpcFileBlock));
        const auto* footer_blocks = reinterpret_cast<const ArrowIpcFileBlock*>(
            decoded->record_batch_blocks.data);
        const auto file_size = static_cast<std::int64_t>(size);
        for (std::size_t i = 0; i < count; ++i) {
            const ArrowIpcFileBlock& block = footer_blocks[i];
            const std::int64_t metadata = block.metadata_length;
            const bool inside =
                block.offset >= 0 && metadata >= 0 && block.body_length >= 0 &&
                block.offset <= file_size &&
                metadata <= file_size - block.offset &&
                block.body_length <= file_size - block.offset - metadata;
            if (!inside) {
                *error = common::make_error<
                    "{}: a footer block points outside the file">(
                    common::ErrorCode::Parse, what);
                return false;
            }
            blocks.push_back(block);
        }
        if (ArrowSchemaDeepCopy(&decoded->schema, &schema) != NANOARROW_OK) {
            *error = common::make_error<"{}: schema copy failed">(
                common::ErrorCode::Internal, what);
            return false;
        }
    }

    // The decoder builds its own array view from a copy of the schema and
    // does not keep it, so the copy lives only for this call.
    auto for_decoder = dstd::make_unique<ArrowSchema>();
    if (ArrowSchemaDeepCopy(&schema, for_decoder.get()) != NANOARROW_OK ||
        ArrowIpcDecoderSetSchema(decoder, for_decoder.get(), &nano_error) !=
            NANOARROW_OK) {
        *error = common::make_error<"{}: schema rejected: {}">(
            common::ErrorCode::Parse, what, nano_error.message);
        return false;
    }
    return true;
}

dstd::Result<dstd::Optional<DataFrame>> Reader::Impl::next_batch() {
    ArrowError nano_error;
    nano_error.message[0] = '\0';
    ArrowBufferView header;
    ArrowBufferView body;
    if (is_file) {
        if (index >= blocks.size()) return dstd::Optional<DataFrame>{};
        const ArrowIpcFileBlock& block = blocks[index++];
        header = view_at(data, block.offset, block.metadata_length);
        body = view_at(data, block.offset + block.metadata_length,
                       block.body_length);
    } else {
        const ArrowBufferView remaining =
            view_at(data, static_cast<std::int64_t>(pos),
                    static_cast<std::int64_t>(size - pos));
        const int peek =
            ArrowIpcDecoderVerifyHeader(decoder, remaining, &nano_error);
        if (peek == ENODATA) return dstd::Optional<DataFrame>{};
        if (peek != NANOARROW_OK)
            return common::make_error<"arrow ipc read: truncated batch: {}">(
                common::ErrorCode::Parse, nano_error.message);
        const std::int64_t header_size = decoder->header_size_bytes;
        const std::int64_t body_size = decoder->body_size_bytes;
        const std::int64_t left = static_cast<std::int64_t>(size - pos);
        if (header_size < 0 || body_size < 0 || header_size > left ||
            body_size > left - header_size)
            return common::make_error<
                "arrow ipc read: a batch message claims more bytes than are "
                "left">(common::ErrorCode::Parse);
        header = view_at(data, static_cast<std::int64_t>(pos), header_size);
        body = view_at(data, static_cast<std::int64_t>(pos) + header_size,
                       body_size);
        pos += static_cast<std::size_t>(header_size) +
               static_cast<std::size_t>(body_size);
    }

    if (ArrowIpcDecoderDecodeHeader(decoder, header, &nano_error) !=
        NANOARROW_OK)
        return common::make_error<"arrow ipc read: bad batch header: {}">(
            common::ErrorCode::Parse, nano_error.message);
    ArrowArray decoded;
    std::memset(&decoded, 0, sizeof(decoded));
    if (ArrowIpcDecoderDecodeArray(decoder, body, -1, &decoded,
                                   NANOARROW_VALIDATION_LEVEL_FULL,
                                   &nano_error) != NANOARROW_OK)
        return common::make_error<"arrow ipc read: bad batch body: {}">(
            common::ErrorCode::Parse, nano_error.message);
    DataFrame frame = detail::import_frame(&schema, &decoded, keep);
    if (decoded.release != nullptr) decoded.release(&decoded);
    if (frame.names.empty() && schema.n_children > 0)
        return common::make_error<"arrow ipc read: a batch is not a struct">(
            common::ErrorCode::Parse);
    for (std::size_t i = 0; i < frame.columns.size(); ++i) {
        if (frame.columns[i].valid()) continue;
        return common::make_error<
            "arrow ipc read: an Arrow type the engine cannot hold in column "
            "'{}'">(common::ErrorCode::NotFound, frame.names[i]);
    }
    return dstd::Optional<DataFrame>(std::move(frame));
}

Reader::Reader(dstd::UniquePtr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Reader::Reader(Reader&& other) noexcept = default;
Reader& Reader::operator=(Reader&& other) noexcept = default;
Reader::~Reader() = default;

dstd::Result<Reader> Reader::open(const dstd::Path& path) {
    auto opened = dstd::File::open(path.view(), dstd::OpenOptions{});
    if (!opened)
        return common::make_error<"arrow ipc open '{}': {}">(
            common::ErrorCode::Io, path.view(), opened.error().message());
    auto size = opened->size();
    if (!size)
        return common::make_error<"arrow ipc open '{}': {}">(
            common::ErrorCode::Io, path.view(), size.error().message());
    if (*size == 0)
        return common::make_error<"arrow ipc open '{}': empty file">(
            common::ErrorCode::Parse, path.view());

    auto impl = dstd::make_unique<Impl>();
    const std::uint8_t* data = nullptr;
    auto mapping = map_file(*opened, static_cast<std::size_t>(*size), &data);
    if (mapping == nullptr)
        return common::make_error<"arrow ipc open '{}': mapping failed">(
            common::ErrorCode::Io, path.view());
    dstd::Result<void> error{};
    if (!impl->start(data, static_cast<std::size_t>(*size), std::move(mapping),
                     path.view(), &error))
        return error.error();
    return Reader{std::move(impl)};
}

dstd::Result<Reader> Reader::from_bytes(dstd::Span<const std::uint8_t> bytes) {
    auto impl = dstd::make_unique<Impl>();
    dstd::Result<void> error{};
    if (!impl->start(bytes.data(), bytes.size(), nullptr, "arrow ipc bytes",
                     &error))
        return error.error();
    return Reader{std::move(impl)};
}

dataframe::Schema Reader::schema() const {
    if (impl_ == nullptr) return {};
    dataframe::Schema out;
    const ArrowSchema* schema = &impl_->schema;
    out.fields.reserve(static_cast<std::size_t>(schema->n_children));
    for (std::int64_t i = 0; i < schema->n_children; ++i) {
        const ArrowSchema* child = schema->children[i];
        out.fields.push_back({child->name != nullptr ? child->name : "",
                              detail::field_type(child),
                              (child->flags & ARROW_FLAG_NULLABLE) != 0});
    }
    return out;
}

dstd::Result<dstd::Optional<DataFrame>> Reader::next() {
    if (impl_ == nullptr)
        return common::make_error<"arrow ipc read: an empty reader">(
            common::ErrorCode::InvalidArgument);
    return impl_->next_batch();
}

namespace {

/// One message a read-ahead task sends its cursor: a decoded batch of file
/// `file`, or the marker that ends that file.
struct IpcItem {
    std::size_t file = 0;
    bool done = false;
    dstd::Result<DataFrame> frame;
};

using IpcChannel = coro::UnboundedChannel<IpcItem>;
using IpcProducer = decltype(std::declval<IpcChannel&>().producer());

/// Settles `task`, so a read-ahead that nobody reads reports to nobody.
coro::detail::Coro detach(coro::CoroTask<dstd::Result<void>> task) {
    (void)co_await coro::settle(std::move(task));
}

/// Posts `task` to the pulling executor, so a read-ahead shares whatever pool
/// is draining the cursor.
void spawn_here(coro::CoroTask<dstd::Result<void>> task) {
    coro::Executor* exec = coro::Executor::current();
    if (exec == nullptr) exec = dftracer::utils::default_runtime().executor();
    if (task.handle()) task.handle().promise().set_executor(exec);
    coro::detail::Coro posted = detach(std::move(task));
    exec->post(posted.release());
}

/// Decodes every batch of one file into `producer`, then the marker that ends
/// it. A read error goes out as the first item, and ends the rest.
coro::CoroTask<dstd::Result<void>> read_file(
    std::size_t index, Reader reader, IpcProducer producer,
    dstd::SharedPtr<std::atomic<bool>> stop,
    dstd::Vector<dstd::String> projection) {
    auto guard = producer.guard();
    while (!stop->load(std::memory_order_relaxed)) {
        auto next = reader.next();
        if (!next) {
            IpcItem failed;
            failed.file = index;
            failed.frame = next.error();
            co_await producer.send(std::move(failed));
            co_return dstd::Result<void>{};
        }
        if (!next->has_value()) break;
        DataFrame frame = std::move(**next);
        if (!projection.empty()) {
            auto selected = frame.select(projection);
            if (!selected) {
                IpcItem failed;
                failed.file = index;
                failed.frame = selected.error();
                co_await producer.send(std::move(failed));
                co_return dstd::Result<void>{};
            }
            frame = std::move(*selected);
        }
        IpcItem item;
        item.file = index;
        item.frame = std::move(frame);
        co_await producer.send(std::move(item));
    }
    IpcItem last;
    last.file = index;
    last.done = true;
    co_await producer.send(std::move(last));
    co_return dstd::Result<void>{};
}

bool same_schema(const dataframe::Schema& a, const dataframe::Schema& b) {
    if (a.fields.size() != b.fields.size()) return false;
    for (std::size_t i = 0; i < a.fields.size(); ++i)
        if (!(a.fields[i] == b.fields[i])) return false;
    return true;
}

/// The record batches of a list of files, one morsel per batch, in file order,
/// decoded by a read-ahead running on the pulling executor.
///
/// The read-ahead holds the next files while their summed size stays inside the
/// memory budget - a decoded batch costs about its share of the file - and the
/// next one always, so a file larger than the budget still makes progress.
/// Their batches go out in file order, so a batch of a later file waits until
/// the earlier files are done.
class IpcCursor final : public dataframe::Cursor {
   public:
    IpcCursor(dstd::Vector<Reader> readers, dstd::Vector<std::uint64_t> sizes,
              dstd::Vector<dstd::String> projection, std::uint64_t budget)
        : readers_(std::move(readers)),
          sizes_(std::move(sizes)),
          projection_(std::move(projection)),
          channel_(coro::make_unbounded_channel<IpcItem>()),
          stop_(dstd::make_shared<std::atomic<bool>>(false)),
          budget_(budget) {}

    IpcCursor(const IpcCursor&) = delete;
    IpcCursor& operator=(const IpcCursor&) = delete;

    /// Abandoning the cursor stops the read-ahead behind it: `read_file` holds
    /// its own channel handle, so only this flag and the closed channel end it.
    ~IpcCursor() override {
        stop_->store(true, std::memory_order_relaxed);
        channel_->close();
    }

    coro::CoroTask<MorselResult> next(std::int64_t) override;

   private:
    /// Starts the next files in order while each still fits the budget next to
    /// the files in flight, and the next file of all whatever its size.
    void start_next() {
        while (next_file_ < readers_.size()) {
            const std::uint64_t next_size = sizes_[next_file_];
            if (next_file_ != head_ && in_flight_ + next_size > budget_) break;
            IpcProducer producer = channel_->producer();
            Reader reader = std::move(readers_[next_file_]);
            spawn_here(read_file(next_file_, std::move(reader),
                                 std::move(producer), stop_, projection_));
            in_flight_ += next_size;
            ++next_file_;
            ++running_;
            note_in_flight_bytes(in_flight_);
        }
    }

    dstd::Vector<Reader> readers_;
    dstd::Vector<std::uint64_t> sizes_;
    dstd::Vector<dstd::String> projection_;
    dstd::SharedPtr<IpcChannel> channel_;
    dstd::SharedPtr<std::atomic<bool>> stop_;
    std::uint64_t budget_ = 0;
    /// The summed size of the files in flight, `head_` up to `next_file_`.
    std::uint64_t in_flight_ = 0;
    /// Batches that arrived before their turn.
    dstd::Vector<IpcItem> pending_;
    std::size_t next_file_ = 0;
    std::size_t running_ = 0;
    /// The file whose batches go out now; every later file waits.
    std::size_t head_ = 0;
    std::int64_t batch_index_ = 0;
    bool started_ = false;
    bool done_ = false;
};

coro::CoroTask<MorselResult> IpcCursor::next(std::int64_t) {
    if (done_) co_return MorselResult{};
    if (!started_) {
        started_ = true;
        start_next();
    }
    while (true) {
        std::size_t pick = pending_.size();
        for (std::size_t i = 0; i < pending_.size(); ++i) {
            if (pending_[i].file != head_) continue;
            pick = i;
            break;
        }
        if (pick < pending_.size()) {
            IpcItem item = std::move(pending_[pick]);
            pending_.erase(pending_.begin() +
                           static_cast<std::ptrdiff_t>(pick));
            if (item.done) {
                --running_;
                in_flight_ -= sizes_[head_];
                ++head_;
                start_next();
                continue;
            }
            DataFrame frame = std::move(*item.frame);
            Morsel morsel;
            morsel.rows = frame.num_rows();
            morsel.batch_index = batch_index_++;
            morsel.columns = std::move(frame.columns);
            co_return MorselResult{std::move(morsel)};
        }
        if (running_ == 0 && next_file_ >= readers_.size()) {
            done_ = true;
            co_return MorselResult{};
        }
        auto received = co_await channel_->receive();
        if (!received) {
            done_ = true;
            co_return MorselResult{};
        }
        if (!received->frame) {
            stop_->store(true, std::memory_order_relaxed);
            done_ = true;
            co_return received->frame.error();
        }
        pending_.push_back(std::move(*received));
    }
}

class IpcSource final : public dataframe::Source {
   public:
    explicit IpcSource(dstd::Vector<dstd::Path> paths)
        : paths_(std::move(paths)) {}

    /// The first file's columns; empty when it cannot be opened, which the
    /// scan then reports.
    dataframe::Schema schema() const override {
        if (paths_.empty()) return {};
        auto reader = Reader::open(paths_.front());
        return reader ? reader->schema() : dataframe::Schema{};
    }

    dstd::Result<dataframe::ScanResult> scan(
        const dataframe::ScanRequest& req) const override {
        dstd::Vector<Reader> readers;
        dstd::Vector<std::uint64_t> sizes;
        readers.reserve(paths_.size());
        sizes.reserve(paths_.size());
        dataframe::Schema first;
        for (std::size_t i = 0; i < paths_.size(); ++i) {
            auto opened = Reader::open(paths_[i]);
            if (!opened) return opened.error();
            if (i == 0) {
                first = opened->schema();
            } else if (!same_schema(first, opened->schema())) {
                return common::make_error<
                    "arrow ipc scan '{}': the columns differ from '{}'">(
                    common::ErrorCode::InvalidArgument, paths_[i].view(),
                    paths_.front().view());
            }
            auto bytes = dstd::fs::file_size(paths_[i].view());
            if (!bytes) return bytes.error();
            sizes.push_back(*bytes);
            readers.push_back(std::move(*opened));
        }
        const std::uint64_t budget = req.memory_budget != 0
                                         ? req.memory_budget
                                         : common::compute_memory_budget();
        dataframe::ScanResult result;
        result.cursor = dstd::make_unique<IpcCursor>(
            std::move(readers), std::move(sizes), req.projection, budget);
        result.filters.assign(req.filters.size(), dataframe::Pushed::No);
        return result;
    }

   private:
    dstd::Vector<dstd::Path> paths_;
};

}  // namespace

namespace detail {

std::uint64_t peak_ipc_in_flight_bytes_for_test() {
    return g_peak_in_flight_bytes.load(std::memory_order_relaxed);
}

void reset_ipc_in_flight_bytes_for_test() {
    g_peak_in_flight_bytes.store(0, std::memory_order_relaxed);
}

}  // namespace detail

dstd::Result<dataframe::LazyFrame> scan_ipc(
    dstd::Span<const dstd::Path> paths) {
    if (paths.empty())
        return common::make_error<"arrow ipc scan: no paths">(
            common::ErrorCode::InvalidArgument);
    dstd::Vector<dstd::Path> owned;
    owned.reserve(paths.size());
    for (const auto& path : paths) owned.push_back(path);
    return dataframe::LazyFrame::scan(
        dstd::make_shared<const IpcSource>(std::move(owned)));
}

dstd::Result<dataframe::DataFrame> read_ipc(
    dstd::Span<const dstd::Path> paths) {
    auto lazy = scan_ipc(paths);
    if (!lazy) return lazy.error();
    return dftracer::utils::default_runtime().block_on(lazy->collect());
}

}  // namespace dftracer::utils::arrow
