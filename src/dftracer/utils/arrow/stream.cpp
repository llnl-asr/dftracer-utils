// SPDX-License-Identifier: MIT
#include <dftracer/utils/arrow/internal.h>
#include <dftracer/utils/arrow/schema_reconcile.h>
#include <dftracer/utils/arrow/stream.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazy/common.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/task_handle.h>
#include <nanoarrow/nanoarrow.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <nanoarrow/nanoarrow.hpp>
#include <type_traits>
#include <utility>

namespace dftracer::utils::arrow {

using dataframe::DataFrame;
using dataframe::LazyFrame;
using dataframe::Morsel;
using dataframe::MorselResult;
using dataframe::Schema;
using dataframe::Series;

namespace {

/// One batch of a query, or the error that ended it.
using QueryBatch = dstd::Result<DataFrame>;

/// Batches a lazy stream's drain task runs ahead of the reader.
constexpr std::size_t LAZY_CHANNEL_CAPACITY = 2;

/// Copies `schema` into `out`, which the caller releases. An errno-style code,
/// as the Arrow C Stream Interface wants it.
int copy_schema(const ArrowSchema* schema, ArrowSchema* out) {
    if (ArrowSchemaDeepCopy(schema, out) != NANOARROW_OK) return ENOMEM;
    return 0;
}

/**
 * @brief What one stream yields: the schema, then one array per @ref get_next,
 * and the message of the first failure.
 */
class StreamBody {
   public:
    virtual ~StreamBody() = default;

    /// Fills `out`; an errno-style code on failure.
    virtual int get_schema(ArrowSchema* out) = 0;
    /// Fills `out`, whose release is null at the end of the stream; an
    /// errno-style code on failure.
    virtual int get_next(ArrowArray* out) = 0;
    /// Stops the work in flight and waits for it. Idempotent.
    virtual void stop() {}

    const dstd::String& last_error() const { return last_error_; }

   protected:
    /// Keeps the first message, so a later failure cannot hide its cause.
    int fail(dstd::StringView message, int code) {
        if (last_error_.empty())
            last_error_.assign(message.data(), message.size());
        return code;
    }

   private:
    dstd::String last_error_;
};

/// A stream over the parts of a frame or of a column: one array per part, each
/// converted inside get_next.
template <class Part>
class PartsBody final : public StreamBody {
   public:
    explicit PartsBody(dstd::Vector<Part> parts) : parts_(std::move(parts)) {}

    int get_schema(ArrowSchema* out) override {
        if (!have_schema_) {
            nanoarrow::UniqueSchema built;
            if (parts_.empty()) {
                detail::column_schema(Series{}, built.get());
            } else {
                detail::column_schema(column_of(parts_.front()), built.get());
            }
            schema_ = std::move(built);
            have_schema_ = true;
        }
        return copy_schema(schema_.get(), out);
    }

    int get_next(ArrowArray* out) override {
        if (index_ >= parts_.size()) {
            out->release = nullptr;
            return 0;
        }
        detail::column_array(column_of(parts_[index_]), out);
        ++index_;
        return 0;
    }

   private:
    static Series column_of(const Part& part) {
        if constexpr (std::is_same_v<Part, DataFrame>) {
            return detail::struct_column(part);
        } else {
            return part.share();
        }
    }

    dstd::Vector<Part> parts_;
    nanoarrow::UniqueSchema schema_;
    std::size_t index_ = 0;
    bool have_schema_ = false;
};

coro::CoroTask<void> drain_query(
    dstd::SharedPtr<coro::Channel<QueryBatch>> channel, LazyFrame plan);

/// Whether `batch` has the columns of `fixed`, by name and Arrow format, in
/// order, with the same dictionary value format.
bool same_columns(const ArrowSchema& fixed, const ArrowSchema& batch) {
    if (fixed.n_children != batch.n_children) return false;
    for (std::int64_t i = 0; i < fixed.n_children; ++i) {
        const ArrowSchema& a = *fixed.children[i];
        const ArrowSchema& b = *batch.children[i];
        if (std::strcmp(a.name, b.name) != 0) return false;
        if (std::strcmp(a.format, b.format) != 0) return false;
        if ((a.dictionary == nullptr) != (b.dictionary == nullptr))
            return false;
        if (a.dictionary != nullptr &&
            std::strcmp(a.dictionary->format, b.dictionary->format) != 0)
            return false;
    }
    return true;
}

/**
 * @brief A stream over a plan: a background task drains the plan into a bounded
 * channel, and get_next hands its batches out. The first batch fixes the
 * schema. A plan with only declared columns hands each later batch out as it
 * is and fails on one whose columns differ; a plan that can return undeclared
 * columns reshapes each batch into the first one's columns plus
 * @ref SchemaReconciler::EXTRA_COLUMN_NAME.
 */
class LazyBody final : public StreamBody {
   public:
    LazyBody(LazyFrame plan, Runtime& runtime);
    ~LazyBody() override { stop(); }

    int get_schema(ArrowSchema* out) override {
        const int rc = prepare();
        if (rc != 0) return rc;
        if (!open_) return copy_schema(fixed_.get(), out);
        if (reconciler_.copy_schema(out) != 0)
            return fail(reconciler_.last_error(), EIO);
        return 0;
    }

    int get_next(ArrowArray* out) override {
        int rc = prepare();
        if (rc != 0) return rc;
        DataFrame frame;
        switch (pull(frame)) {
            case Step::Error:
                return fail(message_, EIO);
            case Step::End:
                out->release = nullptr;
                stop();
                return 0;
            case Step::Frame:
                break;
        }
        Series st = detail::struct_column(frame);
        ArrowSchema batch_schema{};
        ArrowArray batch{};
        detail::column_schema(st, &batch_schema);
        if (!open_) {
            const bool same = same_columns(*fixed_.get(), batch_schema);
            batch_schema.release(&batch_schema);
            if (!same)
                return fail(
                    "to_arrow_stream: a batch has other columns than the "
                    "first one; collect() the query instead",
                    EIO);
            detail::column_array(st, out);
            return 0;
        }
        detail::column_array(st, &batch);
        rc = reconciler_.reconcile(&batch_schema, &batch, out);
        batch_schema.release(&batch_schema);
        batch.release(&batch);
        if (rc != 0) return fail(reconciler_.last_error(), EIO);
        return 0;
    }

    void stop() override;

   private:
    enum class Step { Frame, End, Error };

    /// Fixes the schema from the first batch and holds that batch back, so the
    /// get_schema that reports it and the get_next that emits it agree.
    int prepare();
    /// The next batch of the query, or the end of it, or its error.
    Step pull(DataFrame& frame);

    Runtime* runtime_ = nullptr;
    /// The drain task's cancel source; outlives the task, which stop() waits.
    std::atomic<bool> cancel_{false};
    dstd::SharedPtr<coro::Channel<QueryBatch>> channel_;
    dstd::Optional<TaskHandle> task_;
    SchemaReconciler reconciler_;
    nanoarrow::UniqueSchema fixed_;
    /// The plan's columns, for the schema of a query that yields no batch.
    Schema declared_;
    dstd::Optional<DataFrame> pending_;
    dstd::String message_;
    bool prepared_ = false;
    bool finished_ = false;
    bool stopped_ = false;
    bool open_ = false;
};

LazyBody::LazyBody(LazyFrame plan, Runtime& runtime)
    : runtime_(&runtime),
      channel_(coro::make_channel<QueryBatch>(LAZY_CHANNEL_CAPACITY)),
      open_(plan.has_undeclared_columns()) {
    if (!open_) declared_ = plan.output_schema();
    coro::CoroTask<void> drain = drain_query(channel_, std::move(plan));
    drain.handle().promise().cancel_source = &cancel_;
    task_.emplace(runtime_->submit(std::move(drain), "arrow-to-stream"));
}

void LazyBody::stop() {
    if (stopped_) return;
    stopped_ = true;
    cancel_.store(true, std::memory_order_release);
    if (runtime_->executor() != nullptr) runtime_->executor()->wake_cancelled();
    if (channel_) channel_->close();
    if (task_) task_->wait().discard();
}

int LazyBody::prepare() {
    if (prepared_) return 0;
    DataFrame first;
    const Step step = pull(first);
    if (step == Step::Error) return fail(message_, EIO);
    if (!open_) {
        if (step == Step::Frame) {
            detail::column_schema(detail::struct_column(first), fixed_.get());
            pending_.emplace(std::move(first));
        } else {
            DataFrame empty;
            for (const dataframe::Field& field : declared_.fields) {
                auto column = dataframe::lazy_internal::null_column_of(
                    field, 0, "to_arrow_stream", "the query has no rows");
                if (!column) return fail(column.error().c_str(), EIO);
                empty.names.push_back(dstd::SharedString{field.name});
                empty.columns.push_back(std::move(*column));
            }
            detail::column_schema(detail::struct_column(empty), fixed_.get());
        }
        prepared_ = true;
        return 0;
    }
    if (step == Step::Frame) {
        Series st = detail::struct_column(first);
        ArrowSchema batch_schema{};
        detail::column_schema(st, &batch_schema);
        reconciler_.merge(&batch_schema);
        batch_schema.release(&batch_schema);
        if (!reconciler_.last_error().empty())
            return fail(reconciler_.last_error(), EIO);
        pending_.emplace(std::move(first));
    }
    if (reconciler_.finalize() != 0) {
        const dstd::String& message = reconciler_.last_error();
        if (message.empty())
            return fail("to_arrow_stream: cannot fix the schema", EIO);
        return fail(message, EIO);
    }
    prepared_ = true;
    return 0;
}

LazyBody::Step LazyBody::pull(DataFrame& frame) {
    if (finished_) return Step::End;
    if (pending_) {
        frame = std::move(*pending_);
        pending_.reset();
        return Step::Frame;
    }
    dstd::Optional<QueryBatch> item = channel_->blocking_receive();
    if (!item) {
        finished_ = true;
        return Step::End;
    }
    if (!*item) {
        message_ = (*item).error().c_str();
        finished_ = true;
        return Step::Error;
    }
    frame = std::move(**item);
    return Step::Frame;
}

coro::CoroTask<void> drain_query(
    dstd::SharedPtr<coro::Channel<QueryBatch>> channel, LazyFrame plan) {
    auto chunks = plan.stream();
    for (;;) {
        auto settled = co_await chunks.settled_next();
        if (settled.stopped) {
            co_await coro::stop_here();
        }
        if (settled.error) {
            co_await channel->send(
                QueryBatch(dstd::Unexpected<dstd::Error>(*settled.error)));
            channel->close();
            co_return;
        }
        if (!*settled.value) break;
        QueryBatch next = std::move(**settled.value);
        if (!next) {
            co_await channel->send(
                QueryBatch(dstd::Unexpected<dstd::Error>(next.error())));
            channel->close();
            co_return;
        }
        const bool accepted = co_await channel->send(std::move(next));
        if (!accepted) {
            channel->close();
            co_return;
        }
    }
    channel->close();
    co_return;
}

/// The batches of `frame` as one each: one per chunk when every column is
/// chunked on the same boundaries, else row slices that share the buffers.
dstd::Vector<DataFrame> frame_parts(const DataFrame& frame) {
    const std::size_t ncolumns = frame.columns.size();
    dstd::Vector<DataFrame> parts;
    const std::int32_t chunks =
        ncolumns == 0 ? 1 : dftu_series_chunk_count(frame.columns[0].handle());
    bool per_chunk = ncolumns > 0 && chunks > 1;
    for (std::size_t i = 1; per_chunk && i < ncolumns; ++i)
        per_chunk =
            dftu_series_chunk_count(frame.columns[i].handle()) == chunks;
    if (per_chunk) {
        parts.reserve(static_cast<std::size_t>(chunks));
        for (std::int32_t k = 0; per_chunk && k < chunks; ++k) {
            DataFrame part;
            part.names = frame.names;
            part.columns.reserve(ncolumns);
            for (std::size_t i = 0; per_chunk && i < ncolumns; ++i) {
                part.columns.emplace_back(
                    dftu_series_chunk(frame.columns[i].handle(), k));
                per_chunk = part.columns.back().is_flat() &&
                            part.columns.back().length() ==
                                part.columns.front().length();
            }
            if (per_chunk) parts.push_back(std::move(part));
        }
        if (per_chunk) return parts;
        parts.clear();
    }
    const std::int64_t rows = frame.num_rows();
    const std::int64_t step = common::constants::dataframe::BATCH_SIZE;
    for (std::int64_t offset = 0; offset < rows; offset += step)
        parts.push_back(frame.slice(offset, step));
    if (parts.empty()) parts.push_back(frame.slice(0, 0));
    return parts;
}

/// Releases an ArrowArrayStream exactly once.
class StreamOwner {
   public:
    explicit StreamOwner(ArrowArrayStream* stream) : stream_(stream) {}
    StreamOwner(const StreamOwner&) = delete;
    StreamOwner& operator=(const StreamOwner&) = delete;
    ~StreamOwner() { release_stream(); }

    /// Hands the stream to a new owner, which must release it.
    ArrowArrayStream* release() {
        ArrowArrayStream* stream = stream_;
        stream_ = nullptr;
        return stream;
    }

    ArrowArrayStream* get() const { return stream_; }

   private:
    void release_stream() {
        if (stream_ != nullptr && stream_->release != nullptr) {
            stream_->release(stream_);
            stream_->release = nullptr;
        }
        stream_ = nullptr;
    }

    ArrowArrayStream* stream_;
};

/// The schema of a stream, released once.
class SchemaOwner {
   public:
    SchemaOwner() = default;
    SchemaOwner(const SchemaOwner&) = delete;
    SchemaOwner& operator=(const SchemaOwner&) = delete;
    ~SchemaOwner() {
        if (schema_.release != nullptr) schema_.release(&schema_);
    }

    ArrowSchema* get() { return &schema_; }

   private:
    ArrowSchema schema_{};
};

dstd::Unexpected<dstd::Error> stream_error(ArrowArrayStream* in,
                                           dstd::StringView what) {
    const char* text =
        in->get_last_error != nullptr ? in->get_last_error(in) : nullptr;
    if (text == nullptr || *text == '\0')
        return common::make_error(common::ErrorCode::Io, what);
    return common::make_error(common::ErrorCode::Io, text);
}

/// The indexes of the columns `projection` names, in its order.
dstd::Result<dstd::Vector<std::int64_t>> projection_of(
    const Schema& schema, dstd::Span<const dstd::String> projection) {
    dstd::Vector<std::int64_t> pick;
    pick.reserve(projection.size());
    for (const dstd::String& name : projection) {
        std::int64_t index = -1;
        for (std::size_t i = 0; i < schema.fields.size(); ++i)
            if (schema.fields[i].name == name) {
                index = static_cast<std::int64_t>(i);
                break;
            }
        if (index < 0)
            return common::make_error<"arrow stream: no column named '{}'">(
                common::ErrorCode::InvalidArgument, name);
        pick.push_back(index);
    }
    return pick;
}

/**
 * @brief One batch per @ref ArrowArrayStream::get_next call, read only when the
 * engine pulls, and owned by this cursor.
 */
class StreamCursor final : public dataframe::Cursor {
   public:
    StreamCursor(ArrowArrayStream* stream, const ArrowSchema* described,
                 dstd::Vector<std::int64_t> pick)
        : stream_(stream), pick_(std::move(pick)) {
        ArrowSchema copy{};
        if (ArrowSchemaDeepCopy(described, &copy) == NANOARROW_OK)
            schema_ = nanoarrow::UniqueSchema(&copy);
        else
            release_stream();
    }

    ~StreamCursor() override { release_stream(); }

    coro::CoroTask<MorselResult> next(std::int64_t) override {
        co_return pull();
    }

   private:
    void release_stream() {
        if (stream_ != nullptr && stream_->release != nullptr) {
            stream_->release(stream_);
            stream_->release = nullptr;
        }
        stream_ = nullptr;
    }

    /// Releases the stream, so a failed read leaves nothing live.
    MorselResult fail(dstd::Unexpected<dstd::Error> error) {
        release_stream();
        return error;
    }

    MorselResult pull() {
        if (stream_ == nullptr)
            return common::make_error<"arrow stream: the stream is read">(
                common::ErrorCode::InvalidArgument);
        if (schema_->release == nullptr)
            return fail(common::make_error<
                        "arrow stream: cannot describe the stream schema">(
                common::ErrorCode::InvalidArgument));
        ArrowArray array{};
        const int rc = stream_->get_next(stream_, &array);
        if (rc != 0)
            return fail(stream_error(stream_, "arrow stream: read a batch"));
        if (array.release == nullptr) {
            release_stream();
            return dstd::Optional<Morsel>{};
        }
        const std::int64_t columns =
            static_cast<std::int64_t>(schema_->n_children);
        if (const std::int64_t held = array.n_children; held != columns) {
            array.release(&array);
            return fail(common::make_error<
                        "arrow stream: a batch holds {} columns, the stream "
                        "schema {}">(common::ErrorCode::InvalidArgument, held,
                                     columns));
        }
        DataFrame frame = detail::import_frame(schema_.get(), &array, nullptr);
        if (array.release != nullptr) array.release(&array);
        for (std::size_t i = 0; i < frame.columns.size(); ++i)
            if (!frame.columns[i].valid())
                return fail(
                    common::make_error<
                        "arrow stream: an Arrow type the engine cannot hold "
                        "in column '{}'">(common::ErrorCode::NotFound,
                                          frame.names[i]));
        const std::size_t width =
            pick_.empty() ? frame.columns.size() : pick_.size();
        if (!pick_.empty() && pick_.size() > frame.columns.size())
            return fail(
                common::make_error<"arrow stream: the stream lost a column">(
                    common::ErrorCode::InvalidArgument));
        Morsel morsel;
        morsel.rows = frame.num_rows();
        morsel.batch_index = batch_;
        ++batch_;
        morsel.columns.reserve(width);
        for (std::size_t i = 0; i < width; ++i) {
            const std::size_t at =
                pick_.empty() ? i : static_cast<std::size_t>(pick_[i]);
            Series& column = frame.columns[at];
            morsel.columns.push_back(column.is_flat() ? column.share()
                                                      : column.materialize());
        }
        return dstd::Optional<Morsel>(std::move(morsel));
    }

    ArrowArrayStream* stream_ = nullptr;
    nanoarrow::UniqueSchema schema_;
    dstd::Vector<std::int64_t> pick_;
    std::int64_t batch_ = 0;
};

/// The plan over one Arrow stream: its schema comes from get_schema, and it
/// backs exactly one scan.
class StreamSource final : public dataframe::Source {
   public:
    StreamSource(Schema schema, nanoarrow::UniqueSchema described,
                 ArrowArrayStream* stream)
        : schema_(std::move(schema)),
          stream_(stream),
          described_(std::move(described)) {}
    ~StreamSource() override {
        std::lock_guard<std::mutex> lock(mutex_);
        release_locked();
    }

    Schema schema() const override { return schema_; }

    dstd::Result<dataframe::ScanResult> scan(
        const dataframe::ScanRequest& request) const override {
        auto pick = projection_of(schema_, request.projection);
        if (!pick) return dstd::Unexpected<dstd::Error>(pick.error());
        std::lock_guard<std::mutex> lock(mutex_);
        if (stream_ == nullptr)
            return common::make_error<
                "lazyframe_from_arrow_stream: a stream backs one scan">(
                common::ErrorCode::InvalidArgument);
        ArrowArrayStream* stream = stream_;
        const ArrowSchema* described = described_.get();
        if (stream == nullptr || described == nullptr)
            return common::make_error<
                "lazyframe_from_arrow_stream: a stream backs one scan">(
                common::ErrorCode::InvalidArgument);
        stream_ = nullptr;
        dataframe::ScanResult result;
        result.filters.assign(request.filters.size(), dataframe::Pushed::No);
        result.cursor = dstd::make_unique<StreamCursor>(stream, described,
                                                        std::move(*pick));
        described_ = nanoarrow::UniqueSchema();
        return result;
    }

   private:
    void release_locked() {
        if (stream_ != nullptr && stream_->release != nullptr) {
            stream_->release(stream_);
            stream_->release = nullptr;
        }
        stream_ = nullptr;
    }

    Schema schema_;
    mutable ArrowArrayStream* stream_;
    mutable nanoarrow::UniqueSchema described_;
    mutable std::mutex mutex_;
};

/**
 * @brief What one stream hands its consumer, with the message of the first
 * failure kept for get_last_error.
 */
struct StreamPrivate {
    dstd::UniquePtr<StreamBody> body;
    dstd::String last_error;
    int error_code = 0;
};

int stream_get_schema(ArrowArrayStream* stream, ArrowSchema* out) {
    auto* self = static_cast<StreamPrivate*>(stream->private_data);
    if (self == nullptr) return EINVAL;
    if (self->error_code != 0) return self->error_code;
    const int rc = self->body->get_schema(out);
    if (rc == 0) return 0;
    self->error_code = rc;
    self->last_error = self->body->last_error();
    return rc;
}

int stream_get_next(ArrowArrayStream* stream, ArrowArray* out) {
    auto* self = static_cast<StreamPrivate*>(stream->private_data);
    if (self == nullptr) return EINVAL;
    if (self->error_code != 0) {
        out->release = nullptr;
        return self->error_code;
    }
    const int rc = self->body->get_next(out);
    if (rc == 0) return 0;
    self->error_code = rc;
    self->last_error = self->body->last_error();
    out->release = nullptr;
    return rc;
}

const char* stream_get_last_error(ArrowArrayStream* stream) {
    auto* self = static_cast<StreamPrivate*>(stream->private_data);
    if (self == nullptr || self->last_error.empty()) return nullptr;
    return self->last_error.c_str();
}

void stream_release(ArrowArrayStream* stream) {
    dstd::UniquePtr<StreamPrivate> self(
        static_cast<StreamPrivate*>(stream->private_data));
    stream->private_data = nullptr;
    stream->release = nullptr;
    if (self && self->body) self->body->stop();
}

/// Files `body` behind `out` with the callbacks of the Arrow C Stream
/// Interface, and takes it even when it cannot install it.
dstd::Result<void> install(StreamBody* body, ArrowArrayStream* out) {
    dstd::UniquePtr<StreamBody> owner(body);
    if (out == nullptr)
        return common::make_error<"to_arrow_stream: no output stream">(
            common::ErrorCode::InvalidArgument);
    auto self = dstd::make_unique<StreamPrivate>();
    self->body = std::move(owner);
    out->get_schema = stream_get_schema;
    out->get_next = stream_get_next;
    out->get_last_error = stream_get_last_error;
    out->release = stream_release;
    out->private_data = self.release();
    return {};
}

}  // namespace

dstd::Result<void> to_arrow_stream(const Series& col, ArrowArrayStream* out) {
    if (!col.valid())
        return common::make_error<"to_arrow_stream: a column with no buffers">(
            common::ErrorCode::InvalidArgument);
    const std::int32_t count = dftu_series_chunk_count(col.handle());
    dstd::Vector<Series> chunks;
    chunks.reserve(static_cast<std::size_t>(count));
    bool same_encoding = true;
    for (std::int32_t i = 0; i < count; ++i) {
        chunks.emplace_back(dftu_series_chunk(col.handle(), i));
        same_encoding = same_encoding &&
                        chunks.back().encoding() == chunks.front().encoding();
    }
    // The schema comes from the first chunk, and a dictionary chunk exports in
    // another layout than a flat one, so mixed chunks go out flat.
    if (!same_encoding)
        for (Series& chunk : chunks) chunk = chunk.materialize();
    return install(
        dstd::make_unique<PartsBody<Series>>(std::move(chunks)).release(), out);
}

dstd::Result<void> to_arrow_stream(const DataFrame& frame,
                                   ArrowArrayStream* out) {
    for (std::size_t i = 0; i < frame.columns.size(); ++i)
        if (!frame.columns[i].valid())
            return common::make_error<
                "to_arrow_stream: column '{}' has no buffers">(
                common::ErrorCode::InvalidArgument, frame.names[i]);
    return install(
        dstd::make_unique<PartsBody<DataFrame>>(frame_parts(frame)).release(),
        out);
}

dstd::Result<void> to_arrow_stream(LazyFrame plan, Runtime& runtime,
                                   ArrowArrayStream* out) {
    const auto status = plan.status();
    if (!status) return dstd::Unexpected<dstd::Error>(status.error());
    if (out == nullptr)
        return common::make_error<"to_arrow_stream: no output stream">(
            common::ErrorCode::InvalidArgument);
    return install(
        dstd::make_unique<LazyBody>(std::move(plan), runtime).release(), out);
}

dstd::Result<Series> series_from_arrow_stream(ArrowArrayStream* in) {
    if (in == nullptr)
        return common::make_error<"series_from_arrow_stream: no stream">(
            common::ErrorCode::InvalidArgument);
    StreamOwner stream(in);
    SchemaOwner schema;
    if (in->get_schema(in, schema.get()) != 0)
        return stream_error(in, "series_from_arrow_stream: read the schema");
    dstd::Vector<Series> parts;
    for (;;) {
        ArrowArray array{};
        if (in->get_next(in, &array) != 0)
            return stream_error(in, "series_from_arrow_stream: read a batch");
        if (array.release == nullptr) break;
        Series column = detail::import_series(schema.get(), &array, nullptr);
        if (array.release != nullptr) array.release(&array);
        if (!column.valid())
            return common::make_error<
                "series_from_arrow_stream: an Arrow type the engine cannot "
                "hold">(common::ErrorCode::InvalidArgument);
        parts.push_back(std::move(column));
    }
    if (parts.empty())
        return common::make_error<
            "series_from_arrow_stream: the stream ended with no batch">(
            common::ErrorCode::InvalidArgument);
    return dataframe::chunked_column(std::move(parts));
}

dstd::Result<DataFrame> dataframe_from_arrow_stream(ArrowArrayStream* in) {
    if (in == nullptr)
        return common::make_error<"dataframe_from_arrow_stream: no stream">(
            common::ErrorCode::InvalidArgument);
    StreamOwner stream(in);
    SchemaOwner schema;
    if (in->get_schema(in, schema.get()) != 0)
        return stream_error(in, "dataframe_from_arrow_stream: read the schema");
    if (!detail::is_struct(schema.get()))
        return common::make_error<
            "dataframe_from_arrow_stream: not a struct stream">(
            common::ErrorCode::InvalidArgument);
    const std::int64_t columns = schema.get()->n_children;
    dstd::Vector<DataFrame> parts;
    for (;;) {
        ArrowArray array{};
        if (in->get_next(in, &array) != 0)
            return stream_error(in,
                                "dataframe_from_arrow_stream: read a batch");
        if (array.release == nullptr) break;
        if (const std::int64_t held = array.n_children; held != columns) {
            array.release(&array);
            return common::make_error<
                "dataframe_from_arrow_stream: a batch holds {} columns, the "
                "stream schema {}">(common::ErrorCode::InvalidArgument, held,
                                    columns);
        }
        DataFrame frame = detail::import_frame(schema.get(), &array, nullptr);
        if (array.release != nullptr) array.release(&array);
        for (std::size_t i = 0; i < frame.columns.size(); ++i)
            if (!frame.columns[i].valid())
                return common::make_error<
                    "dataframe_from_arrow_stream: an Arrow type the engine "
                    "cannot hold in column '{}'">(common::ErrorCode::NotFound,
                                                  frame.names[i]);
        parts.push_back(std::move(frame));
    }
    if (parts.empty())
        return common::make_error<
            "dataframe_from_arrow_stream: the stream ended with no batch">(
            common::ErrorCode::InvalidArgument);
    DataFrame out;
    out.names = parts.front().names;
    out.columns.reserve(out.names.size());
    for (std::size_t i = 0; i < out.names.size(); ++i) {
        dstd::Vector<Series> chunks;
        chunks.reserve(parts.size());
        for (DataFrame& part : parts) {
            if (part.names.size() != out.names.size() ||
                part.names[i] != out.names[i])
                return common::make_error<
                    "dataframe_from_arrow_stream: a batch names column {} "
                    "'{}', the first batch '{}'">(
                    common::ErrorCode::InvalidArgument, i, part.names[i],
                    out.names[i]);
            chunks.push_back(std::move(part.columns[i]));
        }
        auto column = dataframe::chunked_column(std::move(chunks));
        if (!column) return dstd::Unexpected<dstd::Error>(column.error());
        out.columns.push_back(std::move(*column));
    }
    return out;
}

dstd::Result<LazyFrame> lazyframe_from_arrow_stream(ArrowArrayStream* in) {
    if (in == nullptr)
        return common::make_error<"lazyframe_from_arrow_stream: no stream">(
            common::ErrorCode::InvalidArgument);
    StreamOwner stream(in);
    SchemaOwner declared;
    if (in->get_schema(in, declared.get()) != 0)
        return stream_error(in, "lazyframe_from_arrow_stream: read the schema");
    if (!detail::is_struct(declared.get()))
        return common::make_error<
            "lazyframe_from_arrow_stream: not a struct stream">(
            common::ErrorCode::InvalidArgument);
    const std::int64_t columns = declared.get()->n_children;
    Schema schema;
    schema.fields.reserve(static_cast<std::size_t>(columns));
    for (std::int64_t i = 0; i < columns; ++i) {
        const ArrowSchema* child = declared.get()->children[i];
        schema.fields.push_back(dataframe::Field{
            child->name != nullptr ? dstd::String(child->name) : dstd::String(),
            detail::field_type(child)});
    }
    nanoarrow::UniqueSchema described(declared.get());
    dstd::SharedPtr<const dataframe::Source> source =
        dstd::make_shared<StreamSource>(std::move(schema), std::move(described),
                                        stream.release());
    return LazyFrame::scan(std::move(source));
}

}  // namespace dftracer::utils::arrow
