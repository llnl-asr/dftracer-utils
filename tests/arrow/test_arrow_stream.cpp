// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/arrow/arrow.h>
#include <dftracer/utils/arrow/schema_reconcile.h>
#include <dftracer/utils/arrow/stream.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/coro/async_event.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <nanoarrow/nanoarrow.h>
#include <task_result.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::arrow;
using namespace dftracer::utils::dataframe;

namespace {

DataFrame run(coro::CoroTask<dstd::Result<DataFrame>> t) {
    return dftu_utils_test::ok(default_runtime().submit(std::move(t)).get());
}

DataFrame numbers(std::int64_t rows) {
    dstd::Vector<std::int64_t> a;
    dstd::Vector<double> b;
    for (std::int64_t i = 0; i < rows; ++i) {
        a.push_back(i);
        b.push_back(static_cast<double>(i) * 0.5);
    }
    DataFrame frame;
    frame.names.push_back(dstd::SharedString{"a"});
    frame.names.push_back(dstd::SharedString{"b"});
    frame.columns.push_back(Series::flat_i64(a.data(), rows));
    frame.columns.push_back(Series::flat_f64(b.data(), rows));
    return frame;
}

dstd::SharedPtr<const Source> src(DataFrame frame) {
    return dstd::make_shared<const InMemorySource>(std::move(frame));
}

// SPDX-License-Identifier: MIT
/// A source that gives the frames it was built with, one morsel each, and that
/// fails instead of batch `fail_at` (0 means never).
class ScriptedSource final : public Source {
   public:
    class ScriptedCursor final : public Cursor {
       public:
        ScriptedCursor(dstd::SharedPtr<const dstd::Vector<DataFrame>> frames,
                       std::int64_t fail_at,
                       dstd::Vector<dstd::String> projection,
                       bool names_from_data)
            : frames_(std::move(frames)),
              fail_at_(fail_at),
              projection_(std::move(projection)),
              names_from_data_(names_from_data) {}

        coro::CoroTask<MorselResult> next(std::int64_t) override {
            co_return step();
        }

        dstd::Optional<dstd::Vector<dstd::String>> out_names() const override {
            if (!names_from_data_) return dstd::nullopt;
            return last_names_;
        }

       private:
        MorselResult step() {
            const std::int64_t at = static_cast<std::int64_t>(index_);
            if (fail_at_ > 0 && at == fail_at_ - 1)
                return common::make_error<"scripted source: batch {} failed">(
                    common::ErrorCode::Io, at);
            if (index_ >= frames_->size()) return dstd::nullopt;
            const DataFrame& frame = (*frames_)[index_];
            last_names_.clear();
            for (const dstd::SharedString& name : frame.names)
                last_names_.push_back(dstd::String{name.view()});
            ++index_;
            Morsel morsel;
            morsel.rows = frame.num_rows();
            morsel.batch_index = at;
            if (projection_.empty()) {
                for (std::size_t i = 0; i < frame.columns.size(); ++i)
                    morsel.columns.push_back(
                        const_cast<Series&>(frame.columns[i]).share());
            } else {
                for (const dstd::String& name : projection_) {
                    bool found = false;
                    for (std::size_t i = 0; i < frame.names.size(); ++i) {
                        if (frame.names[i].view() != name) continue;
                        morsel.columns.push_back(
                            const_cast<Series&>(frame.columns[i]).share());
                        found = true;
                        break;
                    }
                    if (!found)
                        return common::make_error<
                            "scripted source: no column '{}'">(
                            common::ErrorCode::NotFound, name);
                }
            }
            return dstd::Optional<Morsel>(std::move(morsel));
        }

        dstd::SharedPtr<const dstd::Vector<DataFrame>> frames_;
        std::int64_t fail_at_ = 0;
        dstd::Vector<dstd::String> projection_;
        std::size_t index_ = 0;
        bool names_from_data_ = false;
        dstd::Vector<dstd::String> last_names_;
    };

    ScriptedSource(Schema schema, dstd::Vector<DataFrame> frames,
                   std::int64_t fail_at = 0, bool names_from_data = false)
        : schema_(std::move(schema)),
          frames_(dstd::make_shared<const dstd::Vector<DataFrame>>(
              std::move(frames))),
          fail_at_(fail_at),
          names_from_data_(names_from_data) {}

    Schema schema() const override { return schema_; }
    bool undeclared_columns() const override { return names_from_data_; }

    dstd::Result<ScanResult> scan(const ScanRequest& request) const override {
        ScanResult result;
        result.filters.assign(request.filters.size(), Pushed::No);
        result.cursor = dstd::make_unique<ScriptedCursor>(
            frames_, fail_at_, request.projection, names_from_data_);
        return result;
    }

   private:
    Schema schema_;
    dstd::SharedPtr<const dstd::Vector<DataFrame>> frames_;
    std::int64_t fail_at_ = 0;
    bool names_from_data_ = false;
};

/// A source whose first batch is `frame` and whose second never comes: its
/// cursor waits on an event that nobody sets, until the task is cancelled.
class BlockingSource final : public Source {
   public:
    class BlockingCursor final : public Cursor {
       public:
        explicit BlockingCursor(const BlockingSource* source)
            : source_(source) {}

        coro::CoroTask<MorselResult> next(std::int64_t) override {
            if (sent_) {
                coro::AsyncEvent::WaitAwaitable never(source_->never_);
                co_await never;
                co_return dstd::nullopt;
            }
            sent_ = true;
            Morsel morsel;
            morsel.rows = source_->frame_.num_rows();
            for (const Series& column : source_->frame_.columns)
                morsel.columns.push_back(const_cast<Series&>(column).share());
            co_return dstd::Optional<Morsel>(std::move(morsel));
        }

       private:
        const BlockingSource* source_;
        bool sent_ = false;
    };

    BlockingSource(Schema schema, DataFrame frame)
        : schema_(std::move(schema)), frame_(std::move(frame)) {}

    Schema schema() const override { return schema_; }

    dstd::Result<ScanResult> scan(const ScanRequest& request) const override {
        ScanResult result;
        result.filters.assign(request.filters.size(), Pushed::No);
        result.cursor = dstd::make_unique<BlockingCursor>(this);
        return result;
    }

   private:
    Schema schema_;
    DataFrame frame_;
    mutable coro::AsyncEvent never_;
};

/// A stream over `inner` that counts the get_next calls made on it.
struct CountingStream {
    ArrowArrayStream inner{};
    int get_next_calls = 0;
    int release_calls = 0;
    /// The get_next call (1-based) that fails with EIO; 0 means none.
    int fail_at = 0;
};

int counting_get_schema(ArrowArrayStream* stream, ArrowSchema* out) {
    auto* self = static_cast<CountingStream*>(stream->private_data);
    return self->inner.get_schema(&self->inner, out);
}

int counting_get_next(ArrowArrayStream* stream, ArrowArray* out) {
    auto* self = static_cast<CountingStream*>(stream->private_data);
    ++self->get_next_calls;
    if (self->fail_at == self->get_next_calls) {
        out->release = nullptr;
        return EIO;
    }
    return self->inner.get_next(&self->inner, out);
}

const char* counting_last_error(ArrowArrayStream* stream) {
    auto* self = static_cast<CountingStream*>(stream->private_data);
    return self->inner.get_last_error(&self->inner);
}

void counting_release(ArrowArrayStream* stream) {
    auto* self = static_cast<CountingStream*>(stream->private_data);
    ++self->release_calls;
    stream->private_data = nullptr;
    stream->release = nullptr;
    if (self->inner.release != nullptr) self->inner.release(&self->inner);
}

void install_counting(CountingStream& counting, ArrowArrayStream& out,
                      ArrowArrayStream& inner) {
    counting.inner = inner;
    out.get_schema = counting_get_schema;
    out.get_next = counting_get_next;
    out.get_last_error = counting_last_error;
    out.release = counting_release;
    out.private_data = &counting;
}

/// Releases a stream at the end of a test.
struct StreamGuard {
    ArrowArrayStream stream{};
    ~StreamGuard() {
        if (stream.release != nullptr) stream.release(&stream);
    }
};

dstd::Vector<std::int64_t> i64_values(const Series& column) {
    dstd::Vector<std::int64_t> out;
    for (std::int64_t i = 0; i < column.length(); ++i)
        out.push_back(column.data<std::int64_t>()[i]);
    return out;
}

}  // namespace

TEST_CASE("arrow stream: a Series round trips, one array per chunk") {
    dstd::Vector<std::int64_t> values{1, 2, 3};
    Series column = Series::flat_i64(values.data(), 3);

    StreamGuard out;
    REQUIRE(to_arrow_stream(column, &out.stream).has_value());
    auto back = series_from_arrow_stream(&out.stream);
    REQUIRE(back.has_value());
    CHECK(back->length() == 3);
    CHECK(back->type() == TypeId::Int64);
    CHECK(i64_values(*back) == values);
}

TEST_CASE(
    "arrow stream: float, string, bool and dictionary columns round trip") {
    SUBCASE("float64") {
        dstd::Vector<double> values{1.5, 2.5};
        Series column = Series::flat_f64(values.data(), 2);
        StreamGuard out;
        REQUIRE(to_arrow_stream(column, &out.stream).has_value());
        auto back = series_from_arrow_stream(&out.stream);
        REQUIRE(back.has_value());
        CHECK(back->type() == TypeId::Float64);
        CHECK(back->data<double>()[1] == 2.5);
    }
    SUBCASE("string") {
        Series column = Series::strings({"x", "yy", "zzz"});
        StreamGuard out;
        REQUIRE(to_arrow_stream(column, &out.stream).has_value());
        auto back = series_from_arrow_stream(&out.stream);
        REQUIRE(back.has_value());
        CHECK(back->type() == TypeId::String);
        CHECK(back->length() == 3);
        CHECK(back->string_at(2) == "zzz");
    }
    SUBCASE("bool") {
        std::uint8_t bits = 0b101;
        Series column = Series::flat(TypeId::Bool, &bits, 3);
        StreamGuard out;
        REQUIRE(to_arrow_stream(column, &out.stream).has_value());
        auto back = series_from_arrow_stream(&out.stream);
        REQUIRE(back.has_value());
        CHECK(back->type() == TypeId::Bool);
        CHECK(back->length() == 3);
    }
    SUBCASE("dictionary") {
        Series column = Series::strings({"a", "b", "a"}).dictionary_encode();
        REQUIRE(column.valid());
        StreamGuard out;
        REQUIRE(to_arrow_stream(column, &out.stream).has_value());
        auto back = series_from_arrow_stream(&out.stream);
        REQUIRE(back.has_value());
        CHECK(back->length() == 3);
        CHECK(back->type() == TypeId::String);
    }
}

TEST_CASE("arrow stream: a chunked Series round trips as two arrays") {
    dstd::Vector<std::int64_t> first{1, 2};
    dstd::Vector<std::int64_t> second{3, 4, 5};
    dstd::Vector<Series> parts;
    parts.push_back(Series::flat_i64(first.data(), 2));
    parts.push_back(Series::flat_i64(second.data(), 3));
    auto column = chunked_column(std::move(parts));
    REQUIRE(column.has_value());
    REQUIRE(dftu_series_chunk_count(column->handle()) == 2);
    CHECK(dftu_series_chunk_count(column->handle()) == 2);

    StreamGuard out;
    REQUIRE(to_arrow_stream(*column, &out.stream).has_value());
    ArrowSchema schema{};
    REQUIRE(out.stream.get_schema(&out.stream, &schema) == 0);
    CHECK(schema.format != nullptr);
    CHECK(dstd::StringView{schema.format} == "l");
    CHECK(schema.n_children == 0);
    schema.release(&schema);

    auto back = series_from_arrow_stream(&out.stream);
    REQUIRE(back.has_value());
    CHECK(dftu_series_chunk_count(back->handle()) == 2);
    CHECK(i64_values(back->materialize()) ==
          dstd::Vector<std::int64_t>{1, 2, 3, 4, 5});
}

TEST_CASE(
    "arrow stream: a DataFrame round trips, and an empty frame does too") {
    DataFrame frame = numbers(4);
    StreamGuard out;
    REQUIRE(to_arrow_stream(frame, &out.stream).has_value());
    auto back = dataframe_from_arrow_stream(&out.stream);
    REQUIRE(back.has_value());
    CHECK(back->names.size() == 2);
    CHECK(back->num_rows() == 4);
    CHECK(i64_values(back->columns[0].materialize()) ==
          dstd::Vector<std::int64_t>{0, 1, 2, 3});

    DataFrame empty = numbers(0);
    StreamGuard empty_out;
    REQUIRE(to_arrow_stream(empty, &empty_out.stream).has_value());
    auto empty_back = dataframe_from_arrow_stream(&empty_out.stream);
    REQUIRE(empty_back.has_value());
    CHECK(empty_back->names.size() == 2);
    CHECK(empty_back->num_rows() == 0);
}

TEST_CASE("arrow stream: a frame over two chunks gives one batch per chunk") {
    std::int64_t a1[] = {1, 2};
    std::int64_t a2[] = {3, 4, 5};
    std::int64_t b1[] = {10, 20};
    std::int64_t b2[] = {30, 40, 50};
    dstd::Vector<Series> first;
    first.push_back(Series::flat_i64(a1, 2));
    first.push_back(Series::flat_i64(b1, 2));
    dstd::Vector<Series> second;
    second.push_back(Series::flat_i64(a2, 3));
    second.push_back(Series::flat_i64(b2, 3));
    dstd::Vector<Series> columns;
    for (std::size_t i = 0; i < 2; ++i) {
        dstd::Vector<Series> parts;
        parts.push_back(std::move(first[i]));
        parts.push_back(std::move(second[i]));
        auto chunked = chunked_column(std::move(parts));
        REQUIRE(chunked.has_value());
        columns.push_back(std::move(*chunked));
    }
    DataFrame frame;
    frame.names.push_back(dstd::SharedString{"a"});
    frame.names.push_back(dstd::SharedString{"b"});
    frame.columns = std::move(columns);

    StreamGuard out;
    REQUIRE(to_arrow_stream(frame, &out.stream).has_value());
    int batches = 0;
    for (;;) {
        ArrowArray array{};
        REQUIRE(out.stream.get_next(&out.stream, &array) == 0);
        if (array.release == nullptr) break;
        ++batches;
        CHECK(array.n_children == 2);
        array.release(&array);
    }
    CHECK(batches == 2);
}

TEST_CASE("arrow stream: a filtered LazyFrame streams the rows collect gives") {
    LazyFrame plan = LazyFrame::scan(src(numbers(100000)))
                         .filter(col(0) > std::int64_t{99990});
    DataFrame collected = run(plan.collect());
    REQUIRE(collected.num_rows() == 9);

    StreamGuard out;
    REQUIRE(to_arrow_stream(std::move(plan), default_runtime(), &out.stream)
                .has_value());
    ArrowSchema declared{};
    REQUIRE(out.stream.get_schema(&out.stream, &declared) == 0);
    CHECK(declared.n_children ==
          static_cast<std::int64_t>(collected.names.size()));
    declared.release(&declared);
    auto streamed = dataframe_from_arrow_stream(&out.stream);
    REQUIRE(streamed.has_value());
    CHECK(streamed->names.size() == collected.names.size());
    CHECK(streamed->num_rows() == collected.num_rows());
    CHECK(i64_values(streamed->columns[0].materialize()) ==
          i64_values(collected.columns[0].materialize()));
}

TEST_CASE("arrow stream: releasing a lazy stream early stops the query") {
    Schema schema;
    schema.fields.push_back(Field{dstd::String{"a"}, scalar(TypeId::Int64)});
    dstd::Vector<std::int64_t> rows{1, 2, 3};
    DataFrame frame;
    frame.names.push_back(dstd::SharedString{"a"});
    frame.columns.push_back(Series::flat_i64(rows.data(), 3));
    LazyFrame plan = LazyFrame::scan(
        dstd::make_shared<const BlockingSource>(schema, std::move(frame)));
    auto* stream = new ArrowArrayStream{};
    REQUIRE(to_arrow_stream(std::move(plan), default_runtime(), stream)
                .has_value());
    ArrowSchema stream_schema{};
    REQUIRE(stream->get_schema(stream, &stream_schema) == 0);
    stream_schema.release(&stream_schema);
    ArrowArray first{};
    REQUIRE(stream->get_next(stream, &first) == 0);
    REQUIRE(first.release != nullptr);
    first.release(&first);

    const auto start = dstd::steady_now();
    stream->release(stream);
    const auto took = dstd::steady_now() - start;
    delete stream;
    CHECK(std::chrono::duration_cast<std::chrono::seconds>(took).count() < 5);
}

TEST_CASE("arrow stream: a failing batch is an error that sticks") {
    Schema schema;
    schema.fields.push_back(Field{dstd::String{"a"}, scalar(TypeId::Int64)});
    dstd::Vector<DataFrame> frames;
    frames.push_back(numbers(2));
    frames.push_back(numbers(3));
    frames.push_back(numbers(4));
    LazyFrame plan = LazyFrame::scan(
        dstd::make_shared<const ScriptedSource>(schema, std::move(frames), 3));

    StreamGuard out;
    REQUIRE(to_arrow_stream(std::move(plan), default_runtime(), &out.stream)
                .has_value());
    ArrowSchema declared{};
    REQUIRE(out.stream.get_schema(&out.stream, &declared) == 0);
    declared.release(&declared);

    int delivered = 0;
    const char* message = nullptr;
    for (;;) {
        ArrowArray batch{};
        const int code = out.stream.get_next(&out.stream, &batch);
        if (code == 0 && batch.release == nullptr) break;
        if (code != 0) {
            CHECK(code == EIO);
            message = out.stream.get_last_error(&out.stream);
            break;
        }
        ++delivered;
        batch.release(&batch);
    }
    CHECK(delivered == 2);
    REQUIRE(message != nullptr);
    CHECK(dstd::StringView{message}.find("batch 2 failed") !=
          dstd::StringView::npos);

    ArrowArray again{};
    CHECK(out.stream.get_next(&out.stream, &again) == EIO);
    CHECK(dstd::StringView{out.stream.get_last_error(&out.stream)} ==
          dstd::StringView{message});
}

TEST_CASE("arrow stream: a column added mid-stream goes to _extra") {
    Schema schema;
    schema.fields.push_back(Field{dstd::String{"a"}, scalar(TypeId::Int64)});
    dstd::Vector<DataFrame> frames;
    dstd::Vector<std::int64_t> one{1, 2};
    dstd::Vector<std::int64_t> two{3};
    dstd::Vector<std::int64_t> seven{7};
    DataFrame first;
    first.names.push_back(dstd::SharedString{"a"});
    first.columns.push_back(Series::flat_i64(one.data(), 2));
    frames.push_back(std::move(first));
    DataFrame second;
    second.names.push_back(dstd::SharedString{"a"});
    second.names.push_back(dstd::SharedString{"b"});
    second.columns.push_back(Series::flat_i64(two.data(), 1));
    second.columns.push_back(Series::flat_i64(seven.data(), 1));
    frames.push_back(std::move(second));
    LazyFrame plan = LazyFrame::scan(dstd::make_shared<const ScriptedSource>(
        schema, std::move(frames), 0, true));

    StreamGuard out;
    REQUIRE(to_arrow_stream(std::move(plan), default_runtime(), &out.stream)
                .has_value());
    ArrowSchema declared{};
    REQUIRE(out.stream.get_schema(&out.stream, &declared) == 0);
    REQUIRE(declared.n_children == 2);
    CHECK(dstd::StringView{declared.children[0]->name} == "a");
    CHECK(dstd::StringView{declared.children[1]->name} ==
          SchemaReconciler::EXTRA_COLUMN_NAME);
    declared.release(&declared);

    ArrowArray first_batch{};
    REQUIRE(out.stream.get_next(&out.stream, &first_batch) == 0);
    REQUIRE(first_batch.release != nullptr);
    CHECK(first_batch.length == 2);
    {
        ArrowArrayView extra;
        ArrowArrayViewInitFromType(&extra, NANOARROW_TYPE_STRING);
        REQUIRE(ArrowArrayViewSetArray(&extra, first_batch.children[1],
                                       nullptr) == NANOARROW_OK);
        CHECK(ArrowArrayViewIsNull(&extra, 0));
        CHECK(ArrowArrayViewIsNull(&extra, 1));
        ArrowArrayViewReset(&extra);
    }
    first_batch.release(&first_batch);

    ArrowArray second_batch{};
    REQUIRE(out.stream.get_next(&out.stream, &second_batch) == 0);
    REQUIRE(second_batch.release != nullptr);
    CHECK(second_batch.length == 1);
    {
        ArrowArrayView kept;
        ArrowArrayViewInitFromType(&kept, NANOARROW_TYPE_INT64);
        REQUIRE(ArrowArrayViewSetArray(&kept, second_batch.children[0],
                                       nullptr) == NANOARROW_OK);
        CHECK(ArrowArrayViewGetIntUnsafe(&kept, 0) == 3);
        ArrowArrayViewReset(&kept);

        ArrowArrayView extra;
        ArrowArrayViewInitFromType(&extra, NANOARROW_TYPE_STRING);
        REQUIRE(ArrowArrayViewSetArray(&extra, second_batch.children[1],
                                       nullptr) == NANOARROW_OK);
        const ArrowStringView text = ArrowArrayViewGetStringUnsafe(&extra, 0);
        CHECK(dstd::StringView(text.data, static_cast<std::size_t>(
                                              text.size_bytes)) == "{\"b\":7}");
        ArrowArrayViewReset(&extra);
    }
    second_batch.release(&second_batch);
}

TEST_CASE("arrow stream: a declared plan whose batch changes columns fails") {
    Schema schema;
    schema.fields.push_back(Field{dstd::String{"a"}, scalar(TypeId::Int64)});
    dstd::Vector<std::int64_t> one{1, 2};
    dstd::Vector<double> two{3.5};
    dstd::Vector<DataFrame> frames;
    DataFrame first;
    first.names.push_back(dstd::SharedString{"a"});
    first.columns.push_back(Series::flat_i64(one.data(), 2));
    frames.push_back(std::move(first));
    DataFrame second;
    second.names.push_back(dstd::SharedString{"a"});
    second.columns.push_back(Series::flat_f64(two.data(), 1));
    frames.push_back(std::move(second));
    LazyFrame plan = LazyFrame::scan(
        dstd::make_shared<const ScriptedSource>(schema, std::move(frames)));
    REQUIRE_FALSE(plan.has_undeclared_columns());

    StreamGuard out;
    REQUIRE(to_arrow_stream(std::move(plan), default_runtime(), &out.stream)
                .has_value());
    ArrowArray first_batch{};
    REQUIRE(out.stream.get_next(&out.stream, &first_batch) == 0);
    REQUIRE(first_batch.release != nullptr);
    CHECK(first_batch.n_children == 1);
    first_batch.release(&first_batch);
    ArrowArray second_batch{};
    CHECK(out.stream.get_next(&out.stream, &second_batch) == EIO);
    CHECK(second_batch.release == nullptr);
    const char* message = out.stream.get_last_error(&out.stream);
    REQUIRE(message != nullptr);
    CHECK(dstd::StringView{message}.find("other columns") !=
          dstd::StringView::npos);
}

TEST_CASE(
    "arrow stream: columns chunked on other boundaries stream as slices") {
    std::int64_t a1[] = {1, 2};
    std::int64_t a2[] = {3, 4, 5};
    std::int64_t b1[] = {10, 20, 30};
    std::int64_t b2[] = {40, 50};
    dstd::Vector<Series> a_parts;
    a_parts.push_back(Series::flat_i64(a1, 2));
    a_parts.push_back(Series::flat_i64(a2, 3));
    dstd::Vector<Series> b_parts;
    b_parts.push_back(Series::flat_i64(b1, 3));
    b_parts.push_back(Series::flat_i64(b2, 2));
    auto a = chunked_column(std::move(a_parts));
    auto b = chunked_column(std::move(b_parts));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    DataFrame frame;
    frame.names.push_back(dstd::SharedString{"a"});
    frame.names.push_back(dstd::SharedString{"b"});
    frame.columns.push_back(std::move(*a));
    frame.columns.push_back(std::move(*b));

    StreamGuard batches;
    REQUIRE(to_arrow_stream(frame, &batches.stream).has_value());
    std::int64_t rows = 0;
    for (;;) {
        ArrowArray array{};
        REQUIRE(batches.stream.get_next(&batches.stream, &array) == 0);
        if (array.release == nullptr) break;
        rows += array.length;
        for (std::int64_t i = 0; i < array.n_children; ++i)
            CHECK(array.children[i]->length == array.length);
        array.release(&array);
    }
    CHECK(rows == 5);

    StreamGuard out;
    REQUIRE(to_arrow_stream(frame, &out.stream).has_value());
    auto back = dataframe_from_arrow_stream(&out.stream);
    REQUIRE(back.has_value());
    REQUIRE(back->num_rows() == 5);
    CHECK(i64_values(back->columns[0].materialize()) ==
          dstd::Vector<std::int64_t>{1, 2, 3, 4, 5});
    CHECK(i64_values(back->columns[1].materialize()) ==
          dstd::Vector<std::int64_t>{10, 20, 30, 40, 50});
}

TEST_CASE("arrow stream: an import that fails releases its stream once") {
    for (int which = 0; which < 2; ++which) {
        CAPTURE(which);
        ArrowArrayStream inner{};
        REQUIRE(to_arrow_stream(numbers(3), &inner).has_value());
        CountingStream counting;
        counting.fail_at = 1;
        ArrowArrayStream wrapper{};
        install_counting(counting, wrapper, inner);
        const bool ok = which == 0
                            ? dataframe_from_arrow_stream(&wrapper).has_value()
                            : series_from_arrow_stream(&wrapper).has_value();
        CHECK_FALSE(ok);
        CHECK(counting.release_calls == 1);
        CHECK(wrapper.release == nullptr);
    }
}

TEST_CASE("arrow stream: a Series with flat and dictionary chunks streams") {
    dstd::Vector<Series> parts;
    parts.push_back(Series::strings({"a", "b"}).dictionary_encode());
    parts.push_back(Series::strings({"c"}));
    auto chunked = chunked_column(std::move(parts));
    REQUIRE(chunked.has_value());
    StreamGuard out;
    REQUIRE(to_arrow_stream(*chunked, &out.stream).has_value());
    ArrowSchema schema{};
    REQUIRE(out.stream.get_schema(&out.stream, &schema) == 0);
    const bool dictionary = schema.dictionary != nullptr;
    schema.release(&schema);
    for (;;) {
        ArrowArray array{};
        REQUIRE(out.stream.get_next(&out.stream, &array) == 0);
        if (array.release == nullptr) break;
        CHECK((array.dictionary != nullptr) == dictionary);
        array.release(&array);
    }
}

TEST_CASE("arrow stream: a declared plan with no rows keeps its columns") {
    LazyFrame plan =
        LazyFrame::scan(src(numbers(10))).filter(col(0) > std::int64_t{100});
    StreamGuard out;
    REQUIRE(to_arrow_stream(std::move(plan), default_runtime(), &out.stream)
                .has_value());
    ArrowSchema schema{};
    REQUIRE(out.stream.get_schema(&out.stream, &schema) == 0);
    CHECK(schema.n_children == 2);
    schema.release(&schema);
    ArrowArray array{};
    REQUIRE(out.stream.get_next(&out.stream, &array) == 0);
    CHECK(array.release == nullptr);
}

TEST_CASE("arrow stream: importing a LazyFrame pulls only on collect") {
    DataFrame frame = numbers(3);
    ArrowArrayStream inner{};
    REQUIRE(to_arrow_stream(frame, &inner).has_value());
    CountingStream counting;
    ArrowArrayStream wrapper{};
    install_counting(counting, wrapper, inner);

    auto plan = lazyframe_from_arrow_stream(&wrapper);
    REQUIRE(plan.has_value());
    CHECK(counting.get_next_calls == 0);
    auto collected_result = default_runtime().submit(plan->collect()).get();
    REQUIRE(collected_result.has_value());
    DataFrame collected = std::move(*collected_result);
    CHECK(counting.get_next_calls > 0);
    CHECK(collected.num_rows() == 3);
    CHECK(collected.names.size() == 2);

    auto again = plan->collect();
    CHECK_FALSE(default_runtime().submit(std::move(again)).get().has_value());
}

TEST_CASE("arrow stream: a null stream and a stream with no batch are errors") {
    CHECK_FALSE(series_from_arrow_stream(nullptr).has_value());
    CHECK_FALSE(dataframe_from_arrow_stream(nullptr).has_value());
    CHECK_FALSE(lazyframe_from_arrow_stream(nullptr).has_value());
}
