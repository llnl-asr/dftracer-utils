// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/build/resolver.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;

using namespace test_view_common;

namespace {

constexpr int MEMBER_EVENTS = 400;

dstd::String events(int first, int n, const char* cat, const char* name) {
    dstd::String text;
    for (int i = first; i < first + n; ++i) {
        text += R"({"id":)" + dstd::to_string(i) + R"(,"name":")" + name +
                R"(","cat":")" + cat + R"(","pid":1,"tid":1,"ts":)" +
                dstd::to_string(1000 + i) + R"(,"dur":5,"ph":"X"})" + "\n";
    }
    return text;
}

dstd::String read_bytes(const dstd::String& path) {
    auto file = dstd::File::open(dstd::PathView(path), {});
    REQUIRE(file.has_value());
    const std::uint64_t size = file->size().value();
    dstd::String bytes(static_cast<std::size_t>(size), '\0');
    const dstd::Span<std::uint8_t> out(
        reinterpret_cast<std::uint8_t*>(bytes.data()), bytes.size());
    REQUIRE(file->read_exact_at(0, out).has_value());
    return bytes;
}

void write_at(const dstd::String& path, std::uint64_t offset,
              const dstd::String& bytes) {
    dstd::OpenOptions options;
    options.access = dstd::Access::Write;
    options.creation = dstd::Creation::IfNeeded;
    auto file = dstd::File::open(dstd::PathView(path), options);
    REQUIRE(file.has_value());
    REQUIRE(
        file->write_all_at(offset, dstd::ByteView(bytes.data(), bytes.size()))
            .has_value());
}

void append_bytes(const dstd::String& path, const dstd::String& bytes) {
    const auto size = dstd::fs::file_size(dstd::PathView(path));
    write_at(path, size ? *size : 0, bytes);
}

dstd::String member(const dstd::String& dir, const dstd::String& text) {
    const dstd::String plain = dir + "/member.txt";
    const dstd::String gz = dir + "/member.txt.gz";
    {
        dftu_utils_test::FileOut ofs(plain);
        ofs << text;
    }
    REQUIRE(dftu_utils_test::compress_file_to_gzip(plain, gz));
    dstd::String bytes = read_bytes(gz);
    dstd::fs::remove(plain).discard();
    dstd::fs::remove(gz).discard();
    return bytes;
}

std::uint32_t crc32_of(const dstd::String& text) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const char c : text) {
        crc ^= static_cast<unsigned char>(c);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void put_u32_le(dstd::String& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFu));
}

// One stored deflate block, so the text shows in the file as it is.
dstd::String stored_member(const dstd::String& text) {
    REQUIRE(text.size() < 0xFFFFu);
    dstd::String out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
    const auto len = static_cast<std::uint16_t>(text.size());
    const auto nlen = static_cast<std::uint16_t>(~len);
    out.push_back('\x01');
    out.push_back(static_cast<char>(len & 0xFFu));
    out.push_back(static_cast<char>(len >> 8));
    out.push_back(static_cast<char>(nlen & 0xFFu));
    out.push_back(static_cast<char>(nlen >> 8));
    out += text;
    put_u32_le(out, crc32_of(text));
    put_u32_le(out, static_cast<std::uint32_t>(text.size()));
    return out;
}

void start_trace(const dstd::String& dir, const dstd::String& gz, int n,
                 const char* cat, const char* name) {
    append_bytes(gz,
                 member(dir, dstd::String("[\n") + events(0, n, cat, name)));
}

coro::CoroTask<std::size_t> build_tier(dstd::String gz) {
    index::build::ResolveAndBuildInput in;
    in.files.push_back(dstd::String(dstd::StringView(gz)));
    in.require_checkpoints = true;
    index::build::ResolverResult result;
    co_await coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        result = co_await index::build::resolve_and_build_index(std::move(in));
    });
    REQUIRE(result.failures.empty());
    co_return result.appended_count;
}

coro::CoroTask<index::build::ResolverResult> resolve_entries(
    index::build::ResolverInput in) {
    index::build::ResolverResult result;
    co_await coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        result = co_await index::build::Resolver{}(scope, in);
    });
    co_return result;
}

coro::CoroTask<index::build::ResolverResult> resolve_file(dstd::String gz) {
    index::build::ResolverInput in;
    in.files.emplace_back(gz);
    in.require_checkpoints = true;
    co_return co_await resolve_entries(std::move(in));
}

View dir_view(const dstd::String& dir) {
    return run(View::from_directory(dstd::String(dir), dstd::String("")));
}

std::size_t rows(const dstd::String& dir, const dstd::String& q) {
    return dftu_utils_test::ok(dir_view(dir).duql(q).collect().get())
        .num_rows();
}

std::int64_t count_of(const dstd::String& dir, const dstd::String& cat) {
    const auto df = dftu_utils_test::ok(dir_view(dir)
                                            .group_by({GroupKey::cat()})
                                            .agg({{AggOp::Count, "", "n"}})
                                            .collect()
                                            .get());
    for (std::int64_t i = 0; i < df.num_rows(); ++i)
        if (bstr(df, i, "cat") == cat)
            return static_cast<std::int64_t>(bnum(df, i, "n"));
    return 0;
}

coro::CoroTask<std::size_t> count_json(
    coro::AsyncGenerator<utilities::reader::JsonLine> gen) {
    std::size_t n = 0;
    while (auto line = co_await gen.next()) ++n;
    co_return n;
}

struct LineSink : ExportSink {
    dstd::String text;
    void write(dstd::StringView bytes) override {
        text.append(bytes.data(), bytes.size());
    }
};

// Every event of the view, in a fixed order.
dstd::Vector<dstd::String> all_lines(const dstd::String& dir) {
    LineSink sink;
    (void)dir_view(dir).sink_json(sink).get();
    dstd::Vector<dstd::String> lines;
    std::size_t start = 0;
    while (start < sink.text.size()) {
        std::size_t end = sink.text.find('\n', start);
        if (end == dstd::String::npos) end = sink.text.size();
        if (end > start)
            lines.emplace_back(sink.text.substr(start, end - start));
        start = end + 1;
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

// The same bytes in a new folder, indexed in one fresh build.
struct Rebuilt {
    TestEnvironment env{10};
    dstd::String dir;
    explicit Rebuilt(const dstd::String& grown) {
        REQUIRE(env.is_valid());
        dir = env.dir();
        const dstd::String gz = dir + "/t.pfw.gz";
        append_bytes(gz, read_bytes(grown));
        run(build_tier(gz));
    }
};

bool grows_in_place(const dstd::String& gz) {
    const auto result = run(resolve_file(gz));
    return !result.stale_detected && result.needs_checkpoint.size() == 1;
}

}  // namespace

TEST_SUITE("ViewIndexGrow") {
    TEST_CASE("each grown step gives what one fresh build of the bytes gives") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String dir = env.dir();
        const dstd::String gz = dir + "/t.pfw.gz";
        start_trace(dir, gz, MEMBER_EVENTS, "A", "first");
        run(build_tier(gz));

        int next = MEMBER_EVENTS;
        for (const char* cat : {"B", "A", "C"}) {
            const dstd::String name = dstd::String("name_") + cat;
            append_bytes(gz, member(dir, events(next, MEMBER_EVENTS, cat,
                                                name.c_str())));
            next += MEMBER_EVENTS;
            REQUIRE(grows_in_place(gz));
            CHECK(run(build_tier(gz)) == 1);

            const Rebuilt fresh(gz);
            CHECK(all_lines(dir) == all_lines(fresh.dir));
            for (const char* key : {"a", "b", "c"}) {
                CHECK(count_of(dir, key) == count_of(fresh.dir, key));
            }
            CHECK(rows(dir, dstd::String(R"(name == ")") + name + R"(")") ==
                  MEMBER_EVENTS);
        }
        CHECK(count_of(dir, "a") == 2 * MEMBER_EVENTS);
        CHECK(rows(dir, R"(ts >= 1000)") == 4 * MEMBER_EVENTS);
    }

    TEST_CASE("a query prunes the old members after a grow") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String dir = env.dir();
        const dstd::String gz = dir + "/t.pfw.gz";
        start_trace(dir, gz, MEMBER_EVENTS, "A", "first");
        run(build_tier(gz));
        append_bytes(gz, member(dir, events(MEMBER_EVENTS, MEMBER_EVENTS, "A",
                                            "second")));
        REQUIRE(grows_in_place(gz));
        CHECK(run(build_tier(gz)) == 1);

        struct NullSink : ExportSink {
            void write(dstd::StringView) override {}
        } sink;
        const auto stats = dftu_utils_test::ok(
            dir_view(dir).duql(R"(name == "second")").sink_json(sink).get());
        CHECK(stats.events_matched == MEMBER_EVENTS);
        CHECK(stats.chunks_skipped > 0);
    }

    TEST_CASE("a build that read past its scan leaves the file current") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String dir = env.dir();
        const dstd::String gz = dir + "/t.pfw.gz";
        start_trace(dir, gz, MEMBER_EVENTS, "A", "first");
        run(build_tier(gz));
        const dstd::String first_bytes = read_bytes(gz);
        common::InputFileStat scanned;
        REQUIRE(common::stat_input(gz, scanned));

        append_bytes(gz, member(dir, events(MEMBER_EVENTS, MEMBER_EVENTS, "B",
                                            "second")));
        CHECK(run(build_tier(gz)) == 1);

        const auto refresh = [&] {
            index::build::ResolverInput in;
            in.entries.push_back(runtime::fs::FoundFile{
                dstd::Path(gz), scanned.size,
                static_cast<std::uint64_t>(scanned.mtime_ns)});
            in.require_checkpoints = true;
            in.tail_is_current = true;
            return run(resolve_entries(std::move(in)));
        };
        const auto current = refresh();
        CHECK_FALSE(current.stale_detected);
        CHECK(current.needs_checkpoint.empty());
        CHECK(current.cached.size() == 1);

        REQUIRE(dstd::fs::write_file(gz, first_bytes).has_value());
        CHECK(refresh().stale_detected);
    }

    TEST_CASE("a member still being written waits for the next refresh") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String dir = env.dir();
        const dstd::String gz = dir + "/t.pfw.gz";
        start_trace(dir, gz, MEMBER_EVENTS, "A", "first");
        run(build_tier(gz));

        const dstd::String whole =
            member(dir, events(MEMBER_EVENTS, MEMBER_EVENTS, "B", "second"));
        const std::size_t half = whole.size() / 2;
        append_bytes(gz, whole.substr(0, half));
        CHECK(run(build_tier(gz)) == 0);
        CHECK(rows(dir, R"(ts >= 1000)") == MEMBER_EVENTS);
        CHECK(rows(dir, R"(name == "second")") == 0);

        append_bytes(gz, whole.substr(half));
        CHECK(run(build_tier(gz)) == 1);
        CHECK(rows(dir, R"(ts >= 1000)") == 2 * MEMBER_EVENTS);
        CHECK(rows(dir, R"(name == "second")") == MEMBER_EVENTS);
    }

    TEST_CASE("a hole stops at the verified end and fills later") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String dir = env.dir();
        const dstd::String gz = dir + "/t.pfw.gz";
        start_trace(dir, gz, MEMBER_EVENTS, "A", "first");
        run(build_tier(gz));
        const std::size_t old_size = read_bytes(gz).size();

        const dstd::String second =
            member(dir, events(MEMBER_EVENTS, MEMBER_EVENTS, "B", "second"));
        const dstd::String third =
            member(dir, events(2 * MEMBER_EVENTS, MEMBER_EVENTS, "C", "third"));
        append_bytes(gz, dstd::String(second.size(), '\0'));
        append_bytes(gz, third);
        CHECK(run(build_tier(gz)) == 0);
        CHECK(rows(dir, R"(ts >= 1000)") == MEMBER_EVENTS);
        CHECK(rows(dir, R"(name == "third")") == 0);

        write_at(gz, old_size, second);
        CHECK(run(build_tier(gz)) == 1);
        CHECK(rows(dir, R"(name == "second")") == MEMBER_EVENTS);
        CHECK(rows(dir, R"(name == "third")") == MEMBER_EVENTS);
        const Rebuilt fresh(gz);
        CHECK(all_lines(dir) == all_lines(fresh.dir));
    }

    TEST_CASE("a member with false header bytes in it keeps every event") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String dir = env.dir();
        const dstd::String gz = dir + "/t.pfw.gz";
        start_trace(dir, gz, MEMBER_EVENTS, "A", "first");
        run(build_tier(gz));

        dstd::String text;
        text += R"({"id":9000,"name":"false)";
        text += dstd::String("\x1f\x8b\x08\x00", 3);
        text += R"(","cat":"F","pid":1,"tid":1,"ts":9000,"dur":5,"ph":"X"})";
        text += "\n";
        append_bytes(gz, stored_member(text));
        append_bytes(gz, member(dir, events(MEMBER_EVENTS, MEMBER_EVENTS, "B",
                                            "after")));
        CHECK(run(build_tier(gz)) == 1);
        CHECK(rows(dir, R"(ts >= 1000)") == 2 * MEMBER_EVENTS + 1);
        CHECK(rows(dir, R"(name == "after")") == MEMBER_EVENTS);
        const Rebuilt fresh(gz);
        CHECK(all_lines(dir) == all_lines(fresh.dir));

        append_bytes(gz, stored_member(text));
        CHECK(run(build_tier(gz)) == 1);
        CHECK(rows(dir, R"(ts >= 1000)") == 2 * MEMBER_EVENTS + 2);
    }

    TEST_CASE("a line split over two members stays one event") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String dir = env.dir();
        const dstd::String gz = dir + "/t.pfw.gz";
        const dstd::String line(events(MEMBER_EVENTS, 1, "S", "split").c_str());
        const std::size_t cut = line.size() / 2;
        append_bytes(gz,
                     member(dir, dstd::String("[\n") +
                                     events(0, MEMBER_EVENTS, "A", "first") +
                                     line.substr(0, cut)));
        run(build_tier(gz));
        CHECK(rows(dir, R"(name == "split")") == 0);

        append_bytes(gz, member(dir, line.substr(cut)));
        run(build_tier(gz));
        const Rebuilt once(gz);
        CHECK(rows(once.dir, R"(name == "split")") == 1);
        CHECK(rows(dir, R"(name == "split")") == 1);
        utilities::reader::TraceReader reader({.file_path = gz});
        utilities::reader::ReadConfig config;
        config.query = R"(name == "split")";
        CHECK(dftu_utils_test::ok(count_json(reader.read_json(config)).get()) ==
              1);
        CHECK(rows(dir, R"(ts >= 1000)") == MEMBER_EVENTS + 1);
        const Rebuilt fresh(gz);
        CHECK(all_lines(dir) == all_lines(fresh.dir));
    }

    TEST_CASE("changed old bytes in a larger file force a full rebuild") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String dir = env.dir();
        const dstd::String gz = dir + "/t.pfw.gz";
        start_trace(dir, gz, MEMBER_EVENTS, "A", "first");
        run(build_tier(gz));

        dstd::fs::remove(gz).discard();
        start_trace(dir, gz, 2 * MEMBER_EVENTS, "B", "other");
        const auto result = run(resolve_file(gz));
        CHECK(result.stale_detected);
        run(build_tier(gz));
        CHECK(rows(dir, R"(name == "first")") == 0);
        CHECK(rows(dir, R"(name == "other")") == 2 * MEMBER_EVENTS);
    }

    TEST_CASE("a refresh with no new bytes leaves the index as it is") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String dir = env.dir();
        const dstd::String gz = dir + "/t.pfw.gz";
        start_trace(dir, gz, MEMBER_EVENTS, "A", "first");
        run(build_tier(gz));
        const auto result = run(resolve_file(gz));
        CHECK(result.needs_checkpoint.empty());
        CHECK_FALSE(result.stale_detected);
    }

    TEST_CASE("a line across a restart piece is found by its keys") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String dir = env.dir();
        const dstd::String gz = dir + "/t.pfw.gz";
        dstd::String text("[\n");
        constexpr int COUNT = 40000;
        for (int i = 0; i < COUNT; ++i) {
            const dstd::String name = dstd::String("n_") + dstd::to_string(i);
            text += events(i, 1, "A", name.c_str());
        }
        append_bytes(gz, member(dir, text));
        index::build::ResolveAndBuildInput in;
        in.files.emplace_back(gz);
        in.require_checkpoints = true;
        in.checkpoint_size = 32 * 1024;
        (void)run(index::build::resolve_and_build_index(std::move(in)));

        const auto snapshot = dftu_utils_test::index_snapshot(
            dstd::String(determine_index_path(gz, "")));
        const auto file = dftu_utils_test::require_indexed_file(snapshot, gz);
        const auto spans = common::unwrap(index::spans(snapshot, file));
        std::size_t split_lines = 0;
        for (const auto& span : spans) {
            if (!span.opens_mid_line) continue;
            ++split_lines;
            // Line 1 is the opening bracket, and event i is on line i + 2.
            const int id = static_cast<int>(span.first_line_num) - 2;
            REQUIRE(id >= 0);
            REQUIRE(id < COUNT);
            const dstd::String name = dstd::to_string(id);
            CHECK(rows(dir, dstd::String(R"(name == "n_)") + name + R"(")") ==
                  1);
            utilities::reader::TraceReader reader({.file_path = gz});
            utilities::reader::ReadConfig config;
            config.query = dstd::String(R"(name == "n_)") + name + R"(")";
            CHECK(dftu_utils_test::ok(
                      count_json(reader.read_json(config)).get()) == 1);
        }
        CHECK(split_lines > 0);
    }
}
