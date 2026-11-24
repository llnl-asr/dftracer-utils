// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/trace/table/table_header.h>
#include <dftracer/utils/trace/writer/sink_plan.h>

#include <utility>

namespace dftracer::utils::trace::writer {

namespace {

using common::ErrorCode;
using dataframe::Cursor;
using dataframe::DataFrame;
using dataframe::LazyFrame;
using dataframe::MorselResult;
using dataframe::Series;

dstd::Result<std::uint64_t> number(dstd::StringView key,
                                   dstd::StringView text) {
    std::uint64_t value = 0;
    if (text.empty()) value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9')
            return common::make_error<
                "sink option {} must be a non-negative integer, not '{}'">(
                ErrorCode::InvalidArgument, key, text);
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (text.empty())
        return common::make_error<"sink option {} is empty">(
            ErrorCode::InvalidArgument, key);
    return value;
}

dstd::Result<bool> truth(dstd::StringView key, dstd::StringView text) {
    if (text == "true") return true;
    if (text == "false") return false;
    return common::make_error<"sink option {} must be true or false, not '{}'">(
        ErrorCode::InvalidArgument, key, text);
}

dstd::Result<OutputOptions> options_of(
    OutputRows rows, dstd::StringView path,
    dstd::Span<const dstd::Pair<dstd::String, dstd::String>> given) {
    OutputOptions o;
    o.path = dstd::String(path);
    o.rows = rows;
    dstd::String name;
    dstd::String meta = "{}";
    dstd::Optional<bool> compress;
    dstd::Optional<bool> build_index;
    for (const auto& [key_text, value_text] : given) {
        const dstd::StringView key(key_text);
        const dstd::StringView value(value_text);
        if (key == "mode") {
            if (value == "error")
                o.mode = OutputMode::Error;
            else if (value == "overwrite")
                o.mode = OutputMode::Overwrite;
            else
                return common::make_error<
                    "sink option mode must be error or overwrite, not '{}'">(
                    ErrorCode::InvalidArgument, value);
        } else if (key == "part_size" || key == "max_open_files" ||
                   key == "max_partitions" || key == "level") {
            auto n = number(key, value);
            if (!n) return n.error();
            if (key == "part_size")
                o.part_size = static_cast<std::size_t>(*n);
            else if (key == "max_open_files")
                o.max_open_files = static_cast<std::size_t>(*n);
            else if (key == "max_partitions")
                o.max_partitions = static_cast<std::size_t>(*n);
            else
                o.level = static_cast<int>(*n);
        } else if (key == "member_size") {
            auto n = common::parse_bytes(value);
            if (!n)
                return common::make_error<
                    "sink option member_size is not a byte count: '{}'">(
                    ErrorCode::InvalidArgument, value);
            o.member_size = static_cast<std::size_t>(*n);
        } else if (key == "partition_by") {
            std::size_t at = 0;
            while (at <= value.size()) {
                const std::size_t comma = value.find(',', at);
                const std::size_t stop =
                    comma == dstd::StringView::npos ? value.size() : comma;
                const dstd::StringView column = value.substr(at, stop - at);
                if (column.empty())
                    return common::make_error(
                        ErrorCode::InvalidArgument,
                        "sink option partition_by names an empty column");
                o.partition_by.emplace_back(column);
                at = stop + 1;
            }
        } else if (key == "compress" || key == "build_index") {
            auto b = truth(key, value);
            if (!b.has_value()) return b.error();
            (key == "compress" ? compress : build_index) = *b;
        } else if (key == "name" && rows == OutputRows::Records) {
            name = value_text;
        } else if (key == "meta" && rows == OutputRows::Records) {
            json::Document doc;
            auto parsed = doc.parse(value);
            if (!parsed || parsed->type() != json::Value::Type::Object)
                return common::make_error<
                    "sink option meta must be a JSON object, not '{}'">(
                    ErrorCode::InvalidArgument, value);
            meta = value_text;
        } else {
            return common::make_error<"unknown sink option '{}'">(
                ErrorCode::InvalidArgument, key);
        }
    }
    const bool is_file = is_file_output(path);
    const bool natural = path.ends_with(".gz");
    if (is_file) {
        if (compress && *compress != natural)
            return common::make_error<
                "compress contradicts the extension of {}">(
                ErrorCode::InvalidArgument, path);
        o.compress = natural;
    } else {
        if (compress && !*compress)
            return common::make_error<"{} is a folder of parts, which is gzip">(
                ErrorCode::InvalidArgument, path);
        o.compress = true;
    }
    if (build_index) {
        if (*build_index && !o.compress)
            return common::make_error(ErrorCode::InvalidArgument,
                                      "a plain .pfw output cannot be indexed");
        o.build_index = *build_index;
    } else {
        o.build_index = o.compress;
    }
    if (rows == OutputRows::Records) {
        if (name.empty()) {
            dstd::StringView stem = path;
            while (stem.size() > 1 && stem.back() == '/') stem.remove_suffix(1);
            stem = stem.substr(stem.rfind('/') == dstd::StringView::npos
                                   ? 0
                                   : stem.rfind('/') + 1);
            for (const dstd::StringView suffix : {".pfw.gz", ".pfw"})
                if (stem.ends_with(suffix) && stem.size() > suffix.size()) {
                    stem.remove_suffix(suffix.size());
                    break;
                }
            name = dstd::String(stem);
        }
        dstd::String args = "{\"format\":\"dftu.table\",\"format_version\":\"";
        args += table::detail::FORMAT_VERSION;
        args += "\",\"table\":\"";
        json::append_json_escaped(args, name);
        args += "\",\"index\":[],\"index_names\":[],\"meta\":";
        args += meta;
        args += '}';
        o.header_args_json = std::move(args);
        o.fields_from_first_batch = true;
    }
    return o;
}

// Writes what the plan below streams, then yields one summary row.
class OutputCursor final : public Cursor {
   public:
    OutputCursor(LazyFrame inner, OutputOptions options)
        : inner_(std::move(inner)), options_(std::move(options)) {}

    coro::CoroTask<MorselResult> next(std::int64_t max_rows) override {
        if (!summary_) {
            auto running = coro::with_scope(
                [this](coro::Scope& scope) { return run(&scope); });
            auto ran = co_await std::move(running);
            if (!ran) co_return ran.error();
        }
        auto next_task = summary_->next(max_rows);
        co_return co_await std::move(next_task);
    }

   private:
    coro::CoroTask<dstd::Result<OutputSummary>> write_all(
        OutputWriter& out, std::int64_t& batches) {
        auto stream = inner_.stream(0);
        for (;;) {
            auto pulling = stream.next();
            auto frame = co_await std::move(pulling);
            if (!frame) break;
            if (!*frame) co_return (*frame).error();
            auto writing = out.write(**frame);
            auto written = co_await std::move(writing);
            if (!written) co_return written.error();
            ++batches;
        }
        auto closing = out.close();
        co_return co_await std::move(closing);
    }

    coro::CoroTask<dstd::Result<void>> run(coro::Scope* scope) {
        options_.memory_budget = memory::resolve_spill_budget(
            options_.memory_budget != 0 ? options_.memory_budget
                                        : inner_.memory_budget());
        inner_ = inner_.memory_budget(
            split_sink_budget(options_.memory_budget).scan);
        auto opening = OutputWriter::open(options_);
        if (!opening) co_return opening.error();
        OutputWriter out = std::move(*opening);
        out.bind_scope(*scope);
        std::int64_t batches = 0;
        auto writing = write_all(out, batches);
        auto settled = co_await coro::settle(std::move(writing));
        if (settled.error || settled.stopped || !*settled.value) {
            auto aborting = out.abort();
            co_await std::move(aborting);
            if (settled.stopped) co_await coro::stop_here();
            if (settled.error) co_return *settled.error;
            co_return (*settled.value).error();
        }
        OutputSummary& closed = **settled.value;
        if (options_.build_index) {
            if (auto indexed = index_output(closed, options_); !indexed)
                co_return indexed.error();
        }
        rows_ = static_cast<std::int64_t>(closed.rows);
        batches_ = batches;
        DataFrame summary;
        summary.names.emplace_back("rows");
        summary.names.emplace_back("batches");
        summary.columns.push_back(Series::flat_i64(&rows_, 1));
        summary.columns.push_back(Series::flat_i64(&batches_, 1));
        auto scanned = dataframe::InMemorySource(std::move(summary)).scan({});
        if (!scanned) co_return scanned.error();
        summary_ = std::move(scanned->cursor);
        co_return dstd::Result<void>{};
    }

    LazyFrame inner_;
    OutputOptions options_;
    std::int64_t rows_ = 0;
    std::int64_t batches_ = 0;
    dstd::UniquePtr<Cursor> summary_;
};

class OutputSource final : public dataframe::Source {
   public:
    OutputSource(LazyFrame inner, OutputOptions options)
        : inner_(std::move(inner)), options_(std::move(options)) {}

    dataframe::Schema schema() const override {
        dataframe::Schema s;
        s.fields.push_back(dataframe::Field{
            dstd::String("rows"), dataframe::scalar(dataframe::TypeId::Int64),
            false});
        s.fields.push_back(dataframe::Field{
            dstd::String("batches"),
            dataframe::scalar(dataframe::TypeId::Int64), false});
        return s;
    }

    dstd::Result<dataframe::ScanResult> scan(
        const dataframe::ScanRequest& req) const override {
        dataframe::ScanResult r;
        r.filters.assign(req.filters.size(), dataframe::Pushed::No);
        r.cursor = dstd::make_unique<OutputCursor>(inner_, options_);
        return r;
    }

   private:
    LazyFrame inner_;
    OutputOptions options_;
};

}  // namespace

dstd::Result<LazyFrame> output_sink_plan(
    const LazyFrame& plan, OutputRows rows, dstd::StringView path,
    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> options) {
    auto parsed =
        options_of(rows, path,
                   dstd::Span<const dstd::Pair<dstd::String, dstd::String>>(
                       options.data(), options.size()));
    if (!parsed) return parsed.error();
    return LazyFrame::scan(
        dstd::make_shared<OutputSource>(plan, std::move(*parsed)));
}

}  // namespace dftracer::utils::trace::writer
