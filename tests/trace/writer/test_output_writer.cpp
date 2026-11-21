// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/fileio/gzip_line_writer.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/table/table_codec.h>
#include <dftracer/utils/trace/writer/output_writer.h>
#include <dftracer/utils/trace/writer/sink_plan.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::writer;
using namespace dftu_utils_test;
using dataframe::DataFrame;
using dataframe::Series;
using dataframe::TypeId;

namespace {

constexpr dstd::StringView HEADER_ARGS =
    "{\"format\":\"dftu.table\",\"format_version\":\"1.0\",\"table\":\"t\","
    "\"index\":[],\"index_names\":[],\"fields\":[{\"name\":\"i\","
    "\"dtype\":\"int64\"}],\"meta\":{}}";

DataFrame ints(std::int64_t first, std::int64_t count) {
    dstd::Vector<std::int64_t> values;
    for (std::int64_t k = 0; k < count; ++k) values.push_back(first + k);
    DataFrame frame;
    frame.names.push_back(dstd::SharedString("i"));
    frame.columns.push_back(Series::flat(TypeId::Int64, values.data(), count));
    return frame;
}

constexpr dstd::StringView KEYED_HEADER_ARGS =
    "{\"format\":\"dftu.table\",\"format_version\":\"1.0\",\"table\":\"t\","
    "\"index\":[],\"index_names\":[],\"fields\":[{\"name\":\"k\","
    "\"dtype\":\"object\"},{\"name\":\"i\",\"dtype\":\"int64\"}],"
    "\"meta\":{}}";

// Rows 0..count-1, row r keyed by keys[r % keys.size()].
DataFrame keyed(dstd::Span<const dstd::String> keys, std::int64_t count) {
    dstd::Vector<dstd::String> k;
    dstd::Vector<std::int64_t> values;
    for (std::int64_t r = 0; r < count; ++r) {
        k.push_back(keys[static_cast<std::size_t>(r) % keys.size()]);
        values.push_back(r);
    }
    DataFrame frame;
    frame.names.push_back(dstd::SharedString("k"));
    frame.columns.push_back(
        Series::strings(dstd::Span<const dstd::String>(k.data(), k.size())));
    frame.names.push_back(dstd::SharedString("i"));
    frame.columns.push_back(Series::flat(TypeId::Int64, values.data(), count));
    return frame;
}

OutputOptions keyed_to(dstd::StringView path) {
    OutputOptions o;
    o.path = dstd::String(path);
    o.header_args_json = dstd::String(KEYED_HEADER_ARGS);
    o.build_index = false;
    o.partition_by.push_back(dstd::String("k"));
    return o;
}

OutputOptions records_to(dstd::StringView path) {
    OutputOptions o;
    o.path = dstd::String(path);
    o.header_args_json = dstd::String(HEADER_ARGS);
    o.build_index = false;
    return o;
}

coro::CoroTask<dstd::Result<OutputSummary>> write_frames(
    OutputOptions options, dstd::Vector<DataFrame> frames) {
    auto opening = OutputWriter::open(std::move(options));
    if (!opening) co_return opening.error();
    OutputWriter out = std::move(*opening);
    for (const DataFrame& frame : frames) {
        auto writing = out.write(frame);
        auto written = co_await std::move(writing);
        if (!written) co_return written.error();
    }
    auto closing = out.close();
    co_return co_await std::move(closing);
}

dstd::Result<OutputSummary> run(OutputOptions options,
                                dstd::Vector<DataFrame> frames) {
    auto done = default_runtime().block_on(coro::named(
        "write_frames", write_frames(std::move(options), std::move(frames))));
    if (!done) return done.error();
    return std::move(*done);
}

coro::CoroTask<dstd::Result<OutputSummary>> write_then_create(
    OutputOptions options, DataFrame frame, dstd::String create_path) {
    auto opening = OutputWriter::open(std::move(options));
    if (!opening) co_return opening.error();
    OutputWriter out = std::move(*opening);
    auto writing = out.write(frame);
    auto written = co_await std::move(writing);
    if (!written) co_return written.error();
    auto created = dstd::fs::write_file(dstd::PathView(create_path), "mine");
    if (!created) co_return created.error();
    auto closing = out.close();
    co_return co_await std::move(closing);
}

coro::CoroTask<dstd::Result<OutputSummary>> write_frames_bound(
    OutputOptions options, dstd::Vector<DataFrame> frames) {
    auto opening = OutputWriter::open(std::move(options));
    if (!opening) co_return opening.error();
    OutputWriter out = std::move(*opening);
    coro::Scope scope;
    out.bind_scope(scope);
    dstd::Result<OutputSummary> result =
        common::make_error(common::ErrorCode::Internal, "no write");
    for (const DataFrame& frame : frames) {
        auto writing = out.write(frame);
        auto written = co_await std::move(writing);
        if (!written) {
            result = written.error();
            break;
        }
    }
    if (result.has_value() || frames.empty()) {
        auto closing = out.close();
        result = co_await std::move(closing);
    }
    if (!result) {
        auto aborting = out.abort();
        co_await std::move(aborting);
    }
    auto joining = scope.join();
    co_await std::move(joining);
    co_return result;
}

enum class EndOf { Failure, Stop };

// Yields `batches` frames of ints, then ends its next() the way `end` says:
// as a task failure or a stop, not as a returned error.
class EndingSource final : public dataframe::Source {
   public:
    EndingSource(int batches, EndOf end) : batches_(batches), end_(end) {}

    dataframe::Schema schema() const override {
        return dataframe::InMemorySource(ints(0, 1)).schema();
    }

    dstd::Result<dataframe::ScanResult> scan(
        const dataframe::ScanRequest&) const override {
        dataframe::ScanResult r;
        r.cursor = dstd::make_unique<EndingCursor>(batches_, end_);
        return r;
    }

   private:
    class EndingCursor final : public dataframe::Cursor {
       public:
        EndingCursor(int batches, EndOf end) : left_(batches), end_(end) {}

        coro::CoroTask<dataframe::MorselResult> next(std::int64_t) override {
            if (left_ == 0) {
                if (end_ == EndOf::Stop) co_await coro::stop_here();
                auto failure = coro::fail_with(
                    common::make_error(common::ErrorCode::Internal,
                                       "the source failed")
                        .error());
                co_await failure;
            }
            --left_;
            DataFrame frame = ints(next_first_, 4000);
            next_first_ += 4000;
            dataframe::Morsel m;
            m.rows = 4000;
            for (Series& c : frame.columns) m.columns.push_back(std::move(c));
            co_return dstd::Optional<dataframe::Morsel>(std::move(m));
        }

       private:
        int left_;
        EndOf end_;
        std::int64_t next_first_ = 0;
    };

    int batches_;
    EndOf end_;
};

// An event line with a top-level key that no event column holds.
dstd::String event_line(std::int64_t i) {
    dstd::String line = "{\"id\":";
    line += dstd::fmt<"{}">(i);
    line +=
        ",\"name\":\"read\",\"cat\":\"POSIX\",\"pid\":1,\"tid\":1,"
        "\"ts\":";
    line += dstd::fmt<"{}">(1000 + i);
    line += ",\"dur\":1,\"ph\":\"X\",\"args\":{\"k\":\"v\"},\"top\":";
    line += dstd::fmt<"{}">(i * 7);
    line += "}";
    return line;
}

dstd::String event_text(std::int64_t first, std::int64_t count) {
    dstd::String text;
    for (std::int64_t i = first; i < first + count; ++i) {
        text += event_line(i);
        text += '\n';
    }
    return text;
}

OutputOptions events_to(dstd::StringView path) {
    OutputOptions o;
    o.path = dstd::String(path);
    o.rows = OutputRows::Events;
    o.build_index = false;
    return o;
}

coro::CoroTask<dstd::Result<OutputSummary>> write_texts(
    OutputOptions options, dstd::Vector<dstd::String> texts) {
    auto opening = OutputWriter::open(std::move(options));
    if (!opening) co_return opening.error();
    OutputWriter out = std::move(*opening);
    for (const dstd::String& text : texts) {
        auto writing = out.write_lines(text);
        auto written = co_await std::move(writing);
        if (!written) co_return written.error();
    }
    auto closing = out.close();
    co_return co_await std::move(closing);
}

dstd::Result<OutputSummary> run_texts(OutputOptions options,
                                      dstd::Vector<dstd::String> texts) {
    auto done = default_runtime().block_on(coro::named(
        "write_texts", write_texts(std::move(options), std::move(texts))));
    if (!done) return done.error();
    return std::move(*done);
}

// The lines of `file`, read back through a view, sorted.
dstd::Vector<dstd::String> lines_of(dstd::StringView file) {
    const dstd::String path(file);
    test_view_common::StringSink sink;
    auto view = trace::views::View::from_file(
        path, dstd::String(trace::internal::determine_index_path(path, "")));
    ok(view.sink_json(sink).get());
    auto lines = sink.lines();
    std::sort(lines.begin(), lines.end());
    return lines;
}

dstd::Vector<dstd::String> one_text(dstd::String text) {
    dstd::Vector<dstd::String> v;
    v.push_back(std::move(text));
    return v;
}

dstd::Vector<DataFrame> one(DataFrame frame) {
    dstd::Vector<DataFrame> v;
    v.push_back(std::move(frame));
    return v;
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 |
           static_cast<std::uint32_t>(p[3]) << 24;
}

/// Checks that the bytes from `at` are one gzip member of stored blocks and
/// returns its payload and its end.
dstd::String stored_payload(dstd::StringView file, std::size_t at,
                            std::size_t& end) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(file.data()) + at;
    REQUIRE(b[0] == 0x1f);
    REQUIRE(b[1] == 0x8b);
    std::size_t p = 10;
    dstd::String payload;
    for (;;) {
        const bool last = (b[p] & 1) != 0;
        REQUIRE((b[p] >> 1) == 0);
        const std::size_t len = b[p + 1] | (b[p + 2] << 8);
        CHECK(static_cast<std::uint16_t>(~(b[p + 3] | (b[p + 4] << 8))) == len);
        payload.append(reinterpret_cast<const char*>(b + p + 5), len);
        p += 5 + len;
        if (last) break;
    }
    CHECK(le32(b + p) ==
          fileio::compress::gzip_crc32(0, payload.data(), payload.size()));
    CHECK(le32(b + p + 4) == payload.size());
    end = at + p + 8;
    return payload;
}

dstd::Vector<dstd::String> names_in(dstd::StringView dir) {
    dstd::Vector<dstd::String> names;
    auto walked = dstd::fs::walk(
        dstd::PathView(dir), [&](dstd::PathView path, dstd::fs::FileKind) {
            if (path.str() == dir) return dstd::fs::WalkAction::Continue;
            names.emplace_back(dstd::String(path.filename()));
            return dstd::fs::WalkAction::SkipDir;
        });
    REQUIRE(walked.has_value());
    std::sort(names.begin(), names.end());
    return names;
}

std::uint64_t dead_pid() {
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) ::_exit(0);
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    return static_cast<std::uint64_t>(child);
}

bool has_name(dstd::StringView dir, dstd::StringView name) {
    for (const dstd::String& n : names_in(dir))
        if (dstd::StringView(n) == name) return true;
    return false;
}

bool has_staging(dstd::StringView dir) {
    for (const dstd::String& n : names_in(dir))
        if (dstd::StringView(n).find(".tmp-") != dstd::StringView::npos)
            return true;
    return false;
}

}  // namespace

TEST_SUITE("trace.writer.output_writer") {
    TEST_CASE("gzip_stored_member spans blocks and carries the CRC32") {
        for (const std::size_t size :
             {std::size_t{0}, std::size_t{10}, fileio::GZIP_STORED_BLOCK_MAX,
              fileio::GZIP_STORED_BLOCK_MAX + 1,
              std::size_t{3 * fileio::GZIP_STORED_BLOCK_MAX + 7}}) {
            CAPTURE(size);
            dstd::String text(size, 'a');
            for (std::size_t i = 0; i < size; ++i)
                text[i] = static_cast<char>('a' + i % 23);
            const auto member = fileio::gzip_stored_member(text);
            dstd::String bytes(reinterpret_cast<const char*>(member.data()),
                               member.size());
            std::size_t end = 0;
            CHECK(stored_payload(bytes, 0, end) == text);
            CHECK(end == bytes.size());
            const std::size_t blocks =
                std::max<std::size_t>(1, (size + 65534) / 65535);
            CHECK(bytes.size() == 10 + 5 * blocks + size + 8);
        }
    }

    TEST_CASE("a records header above one stored block is read back") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/big.pfw.gz";
        OutputOptions o = records_to(path);
        dstd::String args(HEADER_ARGS);
        args.pop_back();
        args += ",\"pad\":\"" + dstd::String(150000, 'x') + "\"}";
        o.header_args_json = args;
        auto summary = run(std::move(o), one(ints(0, 5)));
        REQUIRE(summary.has_value());
        CHECK(summary->rows == 5);
        auto got = ok(trace::table::read_table(path));
        CHECK(got.frame.num_rows() == 5);
    }

    TEST_CASE("a streamed header's nrows slot and CRC32 are patched") {
        TestEnvironment env(0);
        for (const bool compress : {true, false}) {
            CAPTURE(compress);
            const dstd::String path =
                env.dir() + (compress ? "/s.pfw.gz" : "/s.pfw");
            OutputOptions o = records_to(path);
            o.compress = compress;
            auto frames = one(ints(0, 7));
            frames.push_back(ints(7, 5));
            auto summary = run(std::move(o), std::move(frames));
            REQUIRE(summary.has_value());
            CHECK(summary->rows == 12);
            const dstd::String file = read_text(path);
            dstd::String first;
            if (compress) {
                std::size_t end = 0;
                first = stored_payload(file, 0, end);
            } else {
                first = file.substr(0, file.find('\n'));
            }
            CHECK(first.find("\"nrows\":12") != dstd::String::npos);
            CHECK(first.find("-1") == dstd::String::npos);
            auto got = ok(trace::table::read_table(path));
            CHECK(got.frame.num_rows() == 12);
        }
    }

    TEST_CASE("zero rows write a header and nothing else") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/zero.pfw.gz";
        auto summary = run(records_to(path), dstd::Vector<DataFrame>());
        REQUIRE(summary.has_value());
        CHECK(summary->rows == 0);
        auto got = ok(trace::table::read_table(path));
        CHECK(got.frame.num_rows() == 0);
    }

    TEST_CASE("mode error refuses a target and overwrite replaces it") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/t.pfw.gz";
        REQUIRE(run(records_to(path), one(ints(0, 3))).has_value());
        CHECK_FALSE(run(records_to(path), one(ints(0, 4))).has_value());
        CHECK(ok(trace::table::read_table(path)).frame.num_rows() == 3);
        OutputOptions o = records_to(path);
        o.mode = OutputMode::Overwrite;
        REQUIRE(run(std::move(o), one(ints(0, 4))).has_value());
        CHECK(ok(trace::table::read_table(path)).frame.num_rows() == 4);
    }

    TEST_CASE("the next write removes leftover staging siblings only") {
        TestEnvironment env(0);
        const std::uint64_t gone = dead_pid();
        const dstd::String tmp =
            env.dir() + dstd::fmt<"/.t.pfw.gz.tmp-{}-1">(gone);
        const dstd::String old =
            env.dir() + dstd::fmt<"/.t.pfw.gz.old-{}-2">(gone);
        const dstd::String other = env.dir() + "/.other.pfw.gz.tmp-9";
        REQUIRE(dstd::fs::create_directories(dstd::PathView(tmp)).has_value());
        REQUIRE(dstd::fs::write_file(tmp + "/x", "x"));
        REQUIRE(dstd::fs::write_file(old, "x"));
        REQUIRE(dstd::fs::write_file(other, "x"));
        REQUIRE(run(records_to(env.dir() + "/t.pfw.gz"), one(ints(0, 2)))
                    .has_value());
        CHECK_FALSE(
            has_name(env.dir(), dstd::String(dstd::PathView(tmp).filename())));
        CHECK_FALSE(
            has_name(env.dir(), dstd::String(dstd::PathView(old).filename())));
        CHECK(has_name(env.dir(), ".other.pfw.gz.tmp-9"));
        CHECK(has_name(env.dir(), "t.pfw.gz"));
    }

    TEST_CASE("a failed open leaves nothing behind") {
        TestEnvironment env(0);
        OutputOptions o = records_to(env.dir() + "/t.pfw");
        o.compress = true;
        CHECK_FALSE(run(std::move(o), one(ints(0, 1))).has_value());
        OutputOptions p = records_to(env.dir() + "/t.pfw.gz");
        p.part_size = 2;
        CHECK_FALSE(run(std::move(p), one(ints(0, 1))).has_value());
        CHECK(names_in(env.dir()).empty());
    }

    TEST_CASE("parts are numbered with five digits and hold part_size rows") {
        TestEnvironment env(0);
        const dstd::String dir = env.dir() + "/out";
        OutputOptions o = records_to(dir);
        o.part_size = 2;
        auto summary = run(std::move(o), one(ints(0, 23)));
        REQUIRE(summary.has_value());
        CHECK(summary->files.size() == 12);
        const auto names = names_in(dir);
        REQUIRE(names.size() == 12);
        CHECK(names.front() == "part-00000.pfw.gz");
        CHECK(names.back() == "part-00011.pfw.gz");
        for (std::size_t k = 0; k < summary->files.size(); ++k) {
            const dstd::String want =
                dstd::fmt<"{}/part-{:05}.pfw.gz">(dstd::StringView(dir), k);
            CHECK(summary->files[k] == want);
            auto part = ok(trace::table::read_table(summary->files[k]));
            CHECK(part.frame.num_rows() == (k == 11 ? 1 : 2));
        }
    }

    TEST_CASE("overwrite of a folder leaves one folder and no staging") {
        TestEnvironment env(0);
        const dstd::String dir = env.dir() + "/out";
        OutputOptions o = records_to(dir);
        o.part_size = 2;
        REQUIRE(run(o, one(ints(0, 6))).has_value());
        CHECK_FALSE(run(o, one(ints(0, 6))).has_value());
        o.mode = OutputMode::Overwrite;
        REQUIRE(run(o, one(ints(0, 3))).has_value());
        CHECK(names_in(dir).size() == 2);
        CHECK(names_in(env.dir()).size() == 1);
    }
    TEST_CASE("a failed second rename restores the old folder") {
        TestEnvironment env(0);
        const dstd::String target = env.dir() + "/out";
        const dstd::String aside = env.dir() + "/.out.old-1-1";
        REQUIRE(
            dstd::fs::create_directories(dstd::PathView(target)).has_value());
        REQUIRE(dstd::fs::write_file(target + "/keep", "old").has_value());
        auto done =
            replace_folder(env.dir() + "/missing-staging", target, aside);
        REQUIRE_FALSE(done.has_value());
        CHECK(read_text(target + "/keep") == "old");
        CHECK_FALSE(has_name(env.dir(), ".out.old-1-1"));
    }

    TEST_CASE("mode error refuses a target created after open") {
        TestEnvironment env(0);
        for (const bool folder : {false, true}) {
            CAPTURE(folder);
            const dstd::String target =
                env.dir() + (folder ? "/raced" : "/raced.pfw.gz");
            auto done = default_runtime().block_on(coro::named(
                "write_then_create",
                write_then_create(records_to(target), ints(0, 3), target)));
            CHECK_FALSE(done.has_value());
            CHECK(read_text(target) == "mine");
            for (const dstd::String& n : names_in(env.dir()))
                CHECK_FALSE(dstd::StringView(n).starts_with(".raced"));
        }
    }

    TEST_CASE("the sweep takes only ids of the form digits-digits") {
        TestEnvironment env(0);
        const std::uint64_t gone = dead_pid();
        const dstd::String sibling = env.dir() + "/a.tmp-x";
        OutputOptions live = records_to(sibling);
        auto opening = OutputWriter::open(std::move(live));
        REQUIRE(opening.has_value());
        REQUIRE(dstd::fs::create_directories(
                    dstd::PathView(env.dir() + "/.a.tmp-x.tmp-1-1"))
                    .has_value());
        REQUIRE(dstd::fs::create_directories(
                    dstd::PathView(env.dir() + "/.a.tmp-abc"))
                    .has_value());
        REQUIRE(dstd::fs::create_directories(
                    dstd::PathView(env.dir() + dstd::fmt<"/.a.tmp-{}-8">(gone)))
                    .has_value());
        REQUIRE(dstd::fs::write_file(
                    env.dir() + dstd::fmt<"/.a.tmp-{}-9.shard_0.gz">(gone), "x")
                    .has_value());
        REQUIRE(run(records_to(env.dir() + "/a"), one(ints(0, 2))).has_value());
        CHECK(has_name(env.dir(), ".a.tmp-x.tmp-1-1"));
        CHECK(has_name(env.dir(), ".a.tmp-abc"));
        CHECK_FALSE(has_name(env.dir(), dstd::fmt<".a.tmp-{}-8">(gone)));
        CHECK_FALSE(
            has_name(env.dir(), dstd::fmt<".a.tmp-{}-9.shard_0.gz">(gone)));
        OutputWriter other = std::move(*opening);
        auto closing =
            default_runtime().block_on(coro::named("close", other.close()));
        CHECK(closing.has_value());
    }

    TEST_CASE(
        "the sweep keeps the staging of a live process, takes a dead one") {
        TestEnvironment env(0);
        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0) ::_exit(0);
        int status = 0;
        REQUIRE(::waitpid(child, &status, 0) == child);
        const dstd::String alive =
            env.dir() +
            dstd::fmt<"/.a.tmp-{}-1">(static_cast<std::uint64_t>(::getppid()));
        const dstd::String dead =
            env.dir() +
            dstd::fmt<"/.a.tmp-{}-1">(static_cast<std::uint64_t>(child));
        REQUIRE(
            dstd::fs::create_directories(dstd::PathView(alive)).has_value());
        REQUIRE(dstd::fs::create_directories(dstd::PathView(dead)).has_value());
        REQUIRE(run(records_to(env.dir() + "/a"), one(ints(0, 2))).has_value());
        CHECK(has_name(env.dir(),
                       dstd::String(dstd::PathView(alive).filename())));
        CHECK_FALSE(
            has_name(env.dir(), dstd::String(dstd::PathView(dead).filename())));
    }

    TEST_CASE("a plain folder of parts is refused") {
        TestEnvironment env(0);
        OutputOptions o = records_to(env.dir() + "/out");
        o.compress = false;
        auto done = run(std::move(o), one(ints(0, 2)));
        REQUIRE_FALSE(done.has_value());
        CHECK(dstd::String(done.error().message())
                  .find("write a single .pfw file") != dstd::String::npos);
        CHECK(names_in(env.dir()).empty());
    }

    TEST_CASE("a write never sweeps the staging of a live writer") {
        TestEnvironment env(0);
        const dstd::String target = env.dir() + "/out";
        auto live = OutputWriter::open(records_to(target));
        REQUIRE(live.has_value());
        REQUIRE(run(records_to(target), one(ints(0, 2))).has_value());
        bool staged = false;
        for (const dstd::String& n : names_in(env.dir()))
            if (dstd::StringView(n).starts_with(".out.tmp-")) staged = true;
        CHECK(staged);
    }

    TEST_CASE("a header that disagrees with the frame is refused") {
        TestEnvironment env(0);
        OutputOptions rows = records_to(env.dir() + "/rows.pfw.gz");
        rows.known_rows = 5;
        CHECK_FALSE(run(std::move(rows), one(ints(0, 3))).has_value());
        dstd::Vector<double> values = {1.5, 2.5};
        DataFrame floats;
        floats.names.push_back(dstd::SharedString("i"));
        floats.columns.push_back(
            Series::flat(TypeId::Float64, values.data(), 2));
        CHECK_FALSE(
            run(records_to(env.dir() + "/dtype.pfw.gz"), one(std::move(floats)))
                .has_value());
        CHECK_FALSE(has_name(env.dir(), "rows.pfw.gz"));
        CHECK_FALSE(has_name(env.dir(), "dtype.pfw.gz"));
    }
    TEST_CASE("the fields come from the first batch and later ones must fit") {
        TestEnvironment env(0);
        constexpr dstd::StringView ARGS =
            "{\"format\":\"dftu.table\",\"format_version\":\"1.0\","
            "\"table\":\"t\",\"index\":[],\"index_names\":[],\"meta\":{}}";
        OutputOptions o = records_to(env.dir() + "/a.pfw.gz");
        o.header_args_json = dstd::String(ARGS);
        o.fields_from_first_batch = true;
        auto frames = one(ints(0, 3));
        frames.push_back(ints(3, 2));
        auto done = run(std::move(o), std::move(frames));
        REQUIRE(done.has_value());
        auto got = ok(trace::table::read_table(env.dir() + "/a.pfw.gz"));
        CHECK(got.frame.num_rows() == 5);
        CHECK(got.frame.names[0] == dstd::SharedString("i"));
        CHECK(got.frame.columns[0].type() == TypeId::Int64);

        OutputOptions bad = records_to(env.dir() + "/b.pfw.gz");
        bad.header_args_json = dstd::String(ARGS);
        bad.fields_from_first_batch = true;
        DataFrame other = ints(0, 2);
        other.names[0] = dstd::SharedString("j");
        auto frames2 = one(ints(0, 2));
        frames2.push_back(std::move(other));
        CHECK_FALSE(run(std::move(bad), std::move(frames2)).has_value());
        CHECK_FALSE(has_name(env.dir(), "b.pfw.gz"));

        OutputOptions none = records_to(env.dir() + "/z.pfw.gz");
        none.header_args_json = dstd::String(ARGS);
        none.fields_from_first_batch = true;
        auto empty = run(std::move(none), dstd::Vector<DataFrame>());
        REQUIRE(empty.has_value());
        CHECK(empty->rows == 0);
    }

    TEST_CASE("partitions are percent-encoded folders and read back whole") {
        TestEnvironment env(0);
        const dstd::String dir = env.dir() + "/out";
        dstd::Vector<dstd::String> keys;
        keys.push_back(dstd::String("a/b"));
        keys.push_back(dstd::String("c d"));
        keys.push_back(dstd::String("=%"));
        keys.push_back(dstd::String("\xC3\xA9"));
        keys.push_back(dstd::String(HIVE_DEFAULT_PARTITION));
        auto summary = run(
            keyed_to(dir),
            one(keyed(dstd::Span<const dstd::String>(keys.data(), keys.size()),
                      50)));
        REQUIRE(summary.has_value());
        CHECK(summary->rows == 50);
        CHECK(summary->files.size() == 5);
        const dstd::Vector<dstd::String> folders = names_in(dir);
        CHECK(folders.size() == 5);
        for (const dstd::StringView want :
             {"k=%3D%25", "k=%5F_HIVE_DEFAULT_PARTITION__", "k=%C3%A9",
              "k=a%2Fb", "k=c%20d"})
            CHECK(has_name(dir, want));
        std::int64_t total = 0;
        for (const dstd::String& f : summary->files)
            total += ok(trace::table::read_table(f)).frame.num_rows();
        CHECK(total == 50);
    }

    TEST_CASE("a write past max_partitions fails at the first excess key") {
        TestEnvironment env(0);
        dstd::Vector<dstd::String> keys;
        for (int k = 0; k < 6; ++k) keys.push_back(dstd::fmt<"g{}">(k));
        OutputOptions o = keyed_to(env.dir() + "/out");
        o.max_partitions = 5;
        auto summary = run(
            std::move(o),
            one(keyed(dstd::Span<const dstd::String>(keys.data(), keys.size()),
                      12)));
        REQUIRE_FALSE(summary.has_value());
        CHECK(summary.error().message().find("at least 6") !=
              dstd::String::npos);
        CHECK(names_in(env.dir()).empty());
    }

    TEST_CASE("more partitions than open files reopen parts for append") {
        TestEnvironment env(0);
        const dstd::String dir = env.dir() + "/out";
        dstd::Vector<dstd::String> keys;
        for (int k = 0; k < 12; ++k) keys.push_back(dstd::fmt<"g{}">(k));
        OutputOptions o = keyed_to(dir);
        o.max_open_files = 2;
        o.member_size = 400;
        auto summary = run(
            std::move(o),
            one(keyed(dstd::Span<const dstd::String>(keys.data(), keys.size()),
                      1200)));
        REQUIRE(summary.has_value());
        CHECK(summary->files.size() == 12);
        std::int64_t total = 0;
        for (const dstd::String& f : summary->files)
            total += ok(trace::table::read_table(f)).frame.num_rows();
        CHECK(total == 1200);
    }

    TEST_CASE("partition buffers stay under their share of the budget") {
        TestEnvironment env(0);
        dstd::Vector<dstd::String> keys;
        for (int k = 0; k < 200; ++k) keys.push_back(dstd::fmt<"g{}">(k));
        OutputOptions o = keyed_to(env.dir() + "/out");
        o.memory_budget = std::uint64_t{1} << 20;
        auto summary = run(
            std::move(o),
            one(keyed(dstd::Span<const dstd::String>(keys.data(), keys.size()),
                      60000)));
        REQUIRE(summary.has_value());
        CHECK(summary->rows == 60000);
        CHECK(summary->peak_buffer_bytes > 0);
        CHECK(summary->peak_buffer_bytes <= (std::uint64_t{1} << 18) + 256);
    }

    TEST_CASE(
        "a write that fails while its index is being fed leaves nothing") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/bad.pfw.gz";
        OutputOptions o = records_to(path);
        o.build_index = true;
        o.member_size = 2000;
        o.known_rows = 5;
        dstd::Vector<DataFrame> frames;
        frames.push_back(ints(0, 3000));
        CHECK_FALSE(run(std::move(o), std::move(frames)).has_value());
        CHECK_FALSE(has_name(env.dir(), "bad.pfw.gz"));
        CHECK_FALSE(has_name(env.dir(), ".dftindex"));
    }

    TEST_CASE("an index that cannot be written leaves the write a success") {
        TestEnvironment env(0);
        const dstd::String blocker = env.dir() + "/blocker";
        {
            auto made = dstd::File::open(dstd::PathView(blocker), [] {
                dstd::OpenOptions o;
                o.access = dstd::Access::ReadWrite;
                o.creation = dstd::Creation::OnlyIfNew;
                return o;
            }());
            REQUIRE(made.has_value());
        }
        const dstd::String path = env.dir() + "/ix.pfw.gz";
        OutputOptions o = records_to(path);
        o.build_index = true;
        o.index_dir = blocker + "/index";
        auto summary = run(std::move(o), one(ints(0, 3000)));
        REQUIRE(summary.has_value());
        CHECK(summary->files.size() == 1);
        CHECK(summary->unindexed.size() == 1);
        CHECK(ok(trace::table::read_table(path)).frame.num_rows() == 3000);
    }

    TEST_CASE("a bound write that fails with index messages in flight ends") {
        TestEnvironment env(0);
        for (int round = 0; round < 20; ++round) {
            OutputOptions o = records_to(env.dir() + "/late.pfw.gz");
            o.build_index = true;
            o.member_size = 2000;
            o.known_rows = 1;
            dstd::Vector<DataFrame> frames;
            frames.push_back(ints(0, 20000));
            auto done = default_runtime().block_on(coro::named(
                "bound_write",
                write_frames_bound(std::move(o), std::move(frames))));
            CHECK_FALSE(done.has_value());
        }
        CHECK_FALSE(has_name(env.dir(), "late.pfw.gz"));
        CHECK_FALSE(has_staging(env.dir()));
    }

    TEST_CASE("a plan that ends with a failure or a stop joins its scope") {
        TestEnvironment env(0);
        for (const EndOf end : {EndOf::Failure, EndOf::Stop}) {
            for (const char* partition : {"", "i"}) {
                const dstd::String target = env.dir() + "/ends";
                dstd::Vector<dstd::Pair<dstd::String, dstd::String>> options;
                options.emplace_back("member_size", "4000");
                if (*partition) options.emplace_back("partition_by", partition);
                auto plan = trace::writer::output_sink_plan(
                    dataframe::LazyFrame::scan(
                        dstd::make_shared<EndingSource>(6, end)),
                    OutputRows::Records,
                    *partition ? target : target + ".pfw.gz",
                    std::move(options));
                REQUIRE(plan.has_value());
                for (int round = 0; round < 5; ++round) {
                    auto done = default_runtime().block_on(
                        coro::named("ending_plan", plan->collect()));
                    CHECK_FALSE(done.has_value());
                }
                CHECK_FALSE(has_name(env.dir(), "ends"));
                CHECK_FALSE(has_name(env.dir(), "ends.pfw.gz"));
                CHECK_FALSE(has_staging(env.dir()));
            }
        }
    }

    TEST_CASE("a records write indexes its file while it is written") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/ix.pfw.gz";
        OutputOptions o = records_to(path);
        o.build_index = true;
        o.member_size = 2000;
        dstd::Vector<DataFrame> frames;
        frames.push_back(ints(0, 3000));
        frames.push_back(ints(3000, 3000));
        auto summary = run(std::move(o), std::move(frames));
        REQUIRE(summary.has_value());
        CHECK(has_name(env.dir(), ".dftindex"));
        auto got = ok(trace::table::read_table(path));
        CHECK(got.frame.num_rows() == 6000);
    }

    TEST_CASE("event lines are written as they are and indexed on the way") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/ev.pfw.gz";
        OutputOptions o = events_to(path);
        o.build_index = true;
        o.aggregation_tier = false;
        o.member_size = 1000;
        dstd::Vector<dstd::String> texts;
        for (int k = 0; k < 3; ++k) texts.push_back(event_text(k * 100, 100));
        auto summary = ok(run_texts(std::move(o), std::move(texts)));
        CHECK(summary.rows == 300);
        CHECK(summary.files.size() == 1);
        CHECK(summary.unindexed.empty());
        CHECK(has_name(env.dir(), ".dftindex"));
        dstd::Vector<dstd::String> want;
        for (std::int64_t i = 0; i < 300; ++i) want.push_back(event_line(i));
        std::sort(want.begin(), want.end());
        CHECK(lines_of(path) == want);
    }

    TEST_CASE("events with the aggregation tier are left to index_output") {
        TestEnvironment env(0);
        OutputOptions o = events_to(env.dir() + "/ev.pfw.gz");
        o.build_index = true;
        auto summary = ok(run_texts(std::move(o), one_text(event_text(0, 10))));
        CHECK(summary.unindexed == summary.files);
        CHECK_FALSE(has_name(env.dir(), ".dftindex"));
    }

    TEST_CASE("part_bytes ends each part at the first line end past it") {
        TestEnvironment env(0);
        const dstd::String dir = env.dir() + "/parts";
        const std::size_t line_size = event_line(100).size() + 1;
        OutputOptions o = events_to(dir);
        o.part_bytes = 2000;
        // Lines 100..399 all have the same width.
        dstd::Vector<dstd::String> texts;
        texts.push_back(event_text(100, 150));
        texts.push_back(event_text(250, 150));
        auto summary = ok(run_texts(std::move(o), std::move(texts)));
        const std::size_t per_part = (2000 + line_size - 1) / line_size;
        REQUIRE(summary.files.size() == (300 + per_part - 1) / per_part);
        std::size_t total = 0;
        for (std::size_t i = 0; i < summary.files.size(); ++i) {
            const std::size_t n = lines_of(summary.files[i]).size();
            if (i + 1 < summary.files.size()) CHECK(n == per_part);
            total += n;
        }
        CHECK(total == 300);
    }

    TEST_CASE("part_size counts the rows of event lines") {
        TestEnvironment env(0);
        OutputOptions o = events_to(env.dir() + "/parts");
        o.part_size = 70;
        auto summary =
            ok(run_texts(std::move(o), one_text(event_text(0, 200))));
        REQUIRE(summary.files.size() == 3);
        CHECK(lines_of(summary.files[0]).size() == 70);
        CHECK(lines_of(summary.files[1]).size() == 70);
        CHECK(lines_of(summary.files[2]).size() == 60);
    }

    TEST_CASE("one_file writes one file whatever the name") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/out.gz";
        OutputOptions o = events_to(path);
        o.one_file = true;
        auto summary = ok(run_texts(std::move(o), one_text(event_text(0, 5))));
        REQUIRE(summary.files.size() == 1);
        CHECK(summary.files[0] == path);
        auto st = dstd::fs::lstat(dstd::PathView(path));
        REQUIRE(st.has_value());
        CHECK(st->kind == dstd::fs::FileKind::File);
        CHECK(lines_of(path).size() == 5);
    }

    TEST_CASE("write_lines refuses records, partitions and a cut line") {
        TestEnvironment env(0);
        auto records = run_texts(records_to(env.dir() + "/r.pfw.gz"),
                                 one_text(event_text(0, 2)));
        expect_error(records, common::ErrorCode::InvalidArgument);
        OutputOptions keyed = events_to(env.dir() + "/k");
        keyed.partition_by.push_back(dstd::String("name"));
        expect_error(run_texts(std::move(keyed), one_text(event_text(0, 2))),
                     common::ErrorCode::InvalidArgument);
        dstd::String cut = event_text(0, 2);
        cut.pop_back();
        expect_error(run_texts(events_to(env.dir() + "/c.pfw.gz"),
                               one_text(std::move(cut))),
                     common::ErrorCode::InvalidArgument);
        CHECK_FALSE(has_name(env.dir(), "r.pfw.gz"));
        CHECK_FALSE(has_name(env.dir(), "c.pfw.gz"));
    }

    TEST_CASE("part_bytes is refused for a file and with partition_by") {
        TestEnvironment env(0);
        OutputOptions file = events_to(env.dir() + "/f.pfw.gz");
        file.part_bytes = 100;
        CHECK_FALSE(OutputWriter::open(std::move(file)).has_value());
        OutputOptions keyed = events_to(env.dir() + "/k");
        keyed.partition_by.push_back(dstd::String("name"));
        keyed.part_bytes = 100;
        CHECK_FALSE(OutputWriter::open(std::move(keyed)).has_value());
    }
}
