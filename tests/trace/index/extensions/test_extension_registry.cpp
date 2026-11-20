// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/extensions/extension_registry.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/tables.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstdint>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::index;
using namespace dftracer::utils::trace::index::extensions;

namespace {

constexpr dstd::StringView NAME = "test.fake";

struct Knobs {
    std::uint64_t max_payload = 0;
    std::uint64_t oversize_granule = UINT64_MAX;
    std::uint64_t fail_step_granule = UINT64_MAX;
    bool null_query = false;
    bool file_check = true;
    int finish_file_calls = 0;
    std::uint64_t current_granule = 0;
};

class FakeQuery final : public ExtensionQuery {
   public:
    bool may_match(dstd::StringView payload) const override {
        return payload.size() > 0 && payload[0] == 'g';
    }
    bool file_may_match(dstd::StringView) const override { return true; }
};

class FakeBuilder final : public ExtensionBuilder {
   public:
    explicit FakeBuilder(Knobs& k) : k_(k) {}

    dstd::Result<void> step(dstd::Span<const dstd::StringView> lines) override {
        if (k_.current_granule == k_.fail_step_granule)
            return common::make_error<"step refused">(
                common::ErrorCode::Internal);
        lines_ += lines.size();
        return {};
    }
    dstd::Result<void> finish_granule(std::uint64_t granule,
                                      ExtensionOut& out) override {
        if (granule == k_.oversize_granule) {
            out.append("head");
            out.append(dstd::String(2 * 1024 * 1024, 'x'));
        } else
            out.append(dstd::String("g") + dstd::int_text(lines_));
        lines_ = 0;
        return {};
    }
    bool has_finish_file() const override { return true; }
    dstd::Result<void> finish_file(ExtensionOut& out) override {
        ++k_.finish_file_calls;
        out.append("file");
        return {};
    }

   private:
    Knobs& k_;
    std::uint64_t lines_ = 0;
};

class FakeExtension final : public Extension {
   public:
    explicit FakeExtension(Knobs& k) : k_(k) {}

    dstd::StringView name() const override { return NAME; }
    std::uint32_t version() const override { return 3; }
    std::uint64_t params_hash() const override { return 9; }
    std::uint64_t max_payload() const override { return k_.max_payload; }
    bool has_file_check() const override { return k_.file_check; }
    dstd::Result<dstd::UniquePtr<ExtensionBuilder>> make_builder()
        const override {
        return dstd::UniquePtr<ExtensionBuilder>(
            dstd::make_unique<FakeBuilder>(k_));
    }
    dstd::Result<dstd::UniquePtr<ExtensionQuery>> compile(
        const ExtensionLeaf&) const override {
        if (k_.null_query) return dstd::UniquePtr<ExtensionQuery>();
        return dstd::UniquePtr<ExtensionQuery>(dstd::make_unique<FakeQuery>());
    }

   private:
    Knobs& k_;
};

struct Registered {
    Knobs knobs;
    Registered() {
        REQUIRE(register_extension(
                    ExtensionPtr(dstd::make_shared<FakeExtension>(knobs)))
                    .has_value());
    }
    ~Registered() { unregister_extension(NAME); }
};

dstd::Expected<void, dstd::Error> run(
    coro::CoroTask<dstd::Expected<void, dstd::Error>> task) {
    return default_runtime().submit(std::move(task)).get();
}

// Feeds three chunks through the registered extensions into a file build and
// reads back what the build stored.
PluginData build_three_chunks(Registered& reg, dstd::String& dir) {
    auto opened = Index::open(dir + "/.dftindex");
    REQUIRE(opened.has_value());
    SourceStat stat;
    stat.mtime = 1;
    stat.size = 2;
    auto build = std::move(*opened->build("/traces/a.pfw.gz", stat));
    put_file_metadata(build, 4096, 10, 100, false);
    gzip::GzipMemberRecord member;
    member.c_size = 50;
    member.uc_size = 100;
    member.first_line_num = 1;
    member.last_line_num = 10;
    put_gzip_member(build, member);
    build.finish(trace::index::store::IndexExtension::Members, 11);

    ExtensionBuilders builders(
        trace::index::extensions::registered_extensions());
    const dstd::StringView lines[] = {"{}", "{}"};
    dftracer::utils::dstd::StringIntern intern;
    for (std::uint64_t g = 0; g < 3; ++g) {
        reg.knobs.current_granule = g;
        builders.step(g, dstd::Span<const dstd::StringView>(lines, 2), {},
                      intern);
    }
    builders.finish();
    builders.write(build);
    REQUIRE(run(build.commit()).has_value());

    auto snapshot = opened->snapshot();
    REQUIRE(snapshot.has_value());
    const auto file = snapshot->file("/traces/a.pfw.gz");
    REQUIRE(file.has_value());
    auto copies = plugins(*snapshot, *file);
    REQUIRE(copies.has_value());
    if (copies->empty()) return {};
    CHECK((*copies)[0].version == 3);
    CHECK((*copies)[0].params_hash == 9);
    auto data = plugin(*snapshot, *file, NAME);
    REQUIRE(data.has_value());
    REQUIRE(data->has_value());
    return std::move(**data);
}

}  // namespace

TEST_CASE("an extension is found by name until it is unregistered") {
    {
        Registered reg;
        const auto found = find_extension(NAME);
        REQUIRE(found != nullptr);
        CHECK(found->name() == NAME);
        CHECK(trace::index::extensions::registered_extensions().size() == 1);
        CHECK(found->current(trace::index::store::ExtensionState{3, 9, true}));
        CHECK_FALSE(
            found->current(trace::index::store::ExtensionState{4, 9, true}));
        CHECK_FALSE(
            found->current(trace::index::store::ExtensionState{3, 8, true}));
        CHECK_FALSE(
            found->current(trace::index::store::ExtensionState{3, 9, false}));
    }
    CHECK(find_extension(NAME) == nullptr);
    CHECK(trace::index::extensions::registered_extensions().empty());
}

TEST_CASE("a second extension under a taken name is refused") {
    Registered reg;
    Knobs other;
    const auto again = register_extension(
        ExtensionPtr(dstd::make_shared<FakeExtension>(other)));
    CHECK_FALSE(again.has_value());
    CHECK(trace::index::extensions::registered_extensions().size() == 1);
}

TEST_CASE("the builders store one payload per chunk and one for the file") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    Registered reg;
    dstd::String dir = env.dir();
    const auto data = build_three_chunks(reg, dir);
    REQUIRE(data.granules.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(data.granules[i].first == i);
        CHECK(data.granules[i].second == "g2");
    }
    CHECK(data.file.value_or("") == "file");
}

TEST_CASE("a payload over max_payload is stored as nothing") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    Registered reg;
    reg.knobs.oversize_granule = 1;
    dstd::String dir = env.dir();
    const auto data = build_three_chunks(reg, dir);
    REQUIRE(data.granules.size() == 2);
    CHECK(data.granules[0].first == 0);
    CHECK(data.granules[1].first == 2);
}

TEST_CASE("a payload past a custom max_payload is stored as nothing") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    Registered reg;
    reg.knobs.max_payload = 1;
    dstd::String dir = env.dir();
    const auto data = build_three_chunks(reg, dir);
    CHECK(data.granules.empty());
    CHECK_FALSE(data.file.has_value());
}

TEST_CASE("a failing step stores nothing for the chunk and skips the file") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    Registered reg;
    reg.knobs.fail_step_granule = 1;
    dstd::String dir = env.dir();
    const auto data = build_three_chunks(reg, dir);
    REQUIRE(data.granules.size() == 2);
    CHECK(data.granules[0].first == 0);
    CHECK(data.granules[1].first == 2);
    CHECK_FALSE(data.file.has_value());
    CHECK(reg.knobs.finish_file_calls == 0);
}

TEST_CASE("a compile that returns no query means no pruning") {
    Registered reg;
    const auto ext = find_extension(NAME);
    REQUIRE(ext != nullptr);
    const dstd::String value = "x";
    ExtensionLeaf leaf;
    leaf.path = "name";
    leaf.value.kind = ExtensionValue::Kind::String;
    leaf.value.str = &value;

    auto query = ext->compile(leaf);
    REQUIRE(query.has_value());
    REQUIRE(*query != nullptr);
    CHECK((*query)->may_match("g1"));
    CHECK_FALSE((*query)->may_match("zzz"));

    reg.knobs.null_query = true;
    auto none = ext->compile(leaf);
    REQUIRE(none.has_value());
    CHECK(*none == nullptr);
}

namespace {

using dataframe::Series;
using dataframe::TypeId;
using trace::views::detail::FoldEvent;

constexpr dstd::StringView COLUMNS_NAME = "test.columns";

// What a column extension was handed for one chunk.
struct Seen {
    dstd::String path;
    TypeId type = TypeId::Unknown;
    std::int64_t rows = 0;
    std::int64_t nulls = 0;
    std::int64_t first = 0;
};

struct ColumnLog {
    dstd::Vector<Seen> seen;
    dstd::Vector<std::uint64_t> lines;
    dstd::Vector<dstd::String> wanted;
    bool lines_too = false;
    bool fail_column = false;
};

class ColumnBuilder final : public ExtensionBuilder {
   public:
    explicit ColumnBuilder(ColumnLog& log) : log_(log) {}
    dstd::Result<void> step(dstd::Span<const dstd::StringView> lines) override {
        log_.lines.push_back(lines.size());
        return {};
    }
    dstd::Result<void> step_column(dstd::StringView name,
                                   const Series& column) override {
        if (log_.fail_column)
            return common::make_error<"column refused">(
                common::ErrorCode::Internal);
        Seen s;
        s.path = dstd::String(name);
        s.type = column.type();
        s.rows = column.length();
        s.nulls = column.null_count();
        if (column.type() == TypeId::Int64 && column.length() > 0)
            s.first = column.data<std::int64_t>()[0];
        log_.seen.push_back(std::move(s));
        return {};
    }
    dstd::Result<void> finish_granule(std::uint64_t granule,
                                      ExtensionOut& out) override {
        out.append(dstd::String("g") + dstd::int_text(granule));
        return {};
    }
    bool has_finish_file() const override { return true; }
    dstd::Result<void> finish_file(ExtensionOut& out) override {
        out.append("file");
        return {};
    }

   private:
    ColumnLog& log_;
};

class ColumnsExtension final : public Extension {
   public:
    explicit ColumnsExtension(ColumnLog& log) : log_(log) {
        for (const auto& w : log.wanted) columns_.push_back(w);
    }
    dstd::StringView name() const override { return COLUMNS_NAME; }
    std::uint32_t version() const override { return 1; }
    std::uint64_t params_hash() const override { return 1; }
    std::uint64_t max_payload() const override { return 0; }
    bool has_file_check() const override { return true; }
    dstd::Span<const dstd::String> columns() const override {
        return {columns_.data(), columns_.size()};
    }
    bool takes_lines() const override { return log_.lines_too; }
    dstd::Result<dstd::UniquePtr<ExtensionBuilder>> make_builder()
        const override {
        return dstd::UniquePtr<ExtensionBuilder>(
            dstd::make_unique<ColumnBuilder>(log_));
    }
    dstd::Result<dstd::UniquePtr<ExtensionQuery>> compile(
        const ExtensionLeaf&) const override {
        return dstd::UniquePtr<ExtensionQuery>();
    }

   private:
    ColumnLog& log_;
    dstd::Vector<dstd::String> columns_;
};

// An event with name, pid and ts, and the args `v` (int) and `s` (string id).
FoldEvent make_event(dftracer::utils::dstd::StringIntern& intern,
                     dstd::StringView name, std::int64_t v,
                     dstd::Optional<dstd::StringView> s) {
    FoldEvent e;
    e.name_id = intern.get_or_insert(name);
    e.pid = 7;
    e.tid = 8;
    e.ts = static_cast<std::uint64_t>(v);
    e.args.push_back({intern.get_or_insert("v"), FoldEvent::ArgValue(v)});
    if (s)
        e.args.push_back({intern.get_or_insert("s"),
                          FoldEvent::ArgValue(static_cast<std::uint32_t>(
                              intern.get_or_insert(*s)))});
    return e;
}

// Feeds `chunks` (each a list of pieces of events) to a column extension and
// returns what the build stored.
PluginData build_columns(
    ColumnLog& log, dstd::String& dir,
    dstd::Span<const dstd::Vector<dstd::Vector<FoldEvent>>> chunks,
    dftracer::utils::dstd::StringIntern& intern) {
    auto opened = Index::open(dir + "/.dftindex");
    REQUIRE(opened.has_value());
    SourceStat stat;
    stat.mtime = 1;
    stat.size = 2;
    auto build = std::move(*opened->build("/traces/a.pfw.gz", stat));
    put_file_metadata(build, 4096, 10, 100, false);
    gzip::GzipMemberRecord member;
    member.c_size = 50;
    member.uc_size = 100;
    member.first_line_num = 1;
    member.last_line_num = 10;
    put_gzip_member(build, member);
    build.finish(trace::index::store::IndexExtension::Members, 11);

    ExtensionBuilders builders(
        {ExtensionPtr(dstd::make_shared<ColumnsExtension>(log))});
    const dstd::StringView lines[] = {"{}"};
    for (std::uint64_t g = 0; g < chunks.size(); ++g)
        for (const auto& piece : chunks[g])
            builders.step(
                g, dstd::Span<const dstd::StringView>(lines, 1),
                dstd::Span<const FoldEvent>(piece.data(), piece.size()),
                intern);
    builders.finish();
    builders.write(build);
    REQUIRE(run(build.commit()).has_value());
    auto snapshot = opened->snapshot();
    REQUIRE(snapshot.has_value());
    const auto file = snapshot->file("/traces/a.pfw.gz");
    REQUIRE(file.has_value());
    auto data = plugin(*snapshot, *file, COLUMNS_NAME);
    REQUIRE(data.has_value());
    if (!data->has_value()) return {};
    return std::move(**data);
}

}  // namespace

TEST_CASE("a column extension gets one typed column per path and chunk") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    dftracer::utils::dstd::StringIntern intern;
    ColumnLog log;
    log.wanted = {"args.v", "args.s", "name", "args.absent"};
    // Chunk 0 comes in two pieces, as a chunk does when it is parsed in
    // pieces; `s` is on the second piece only.
    dstd::Vector<dstd::Vector<dstd::Vector<FoldEvent>>> chunks(2);
    chunks[0].resize(2);
    chunks[0][0].push_back(make_event(intern, "read", 5, dstd::nullopt));
    chunks[0][0].push_back(make_event(intern, "read", 6, dstd::nullopt));
    chunks[0][1].push_back(make_event(intern, "write", 7, "x"));
    chunks[1].resize(1);
    chunks[1][0].push_back(make_event(intern, "read", 9, "y"));
    dstd::String dir = env.dir();
    const auto data =
        build_columns(log, dir, {chunks.data(), chunks.size()}, intern);

    // The extension did not take lines, so it saw none.
    CHECK(log.lines.empty());
    REQUIRE(log.seen.size() == 6);
    auto find = [&](dstd::StringView path, std::size_t from) -> const Seen* {
        for (std::size_t i = from; i < log.seen.size(); ++i)
            if (log.seen[i].path == path) return &log.seen[i];
        return nullptr;
    };
    const Seen* v0 = find("args.v", 0);
    REQUIRE(v0);
    CHECK(v0->type == TypeId::Int64);
    CHECK(v0->rows == 3);
    CHECK(v0->nulls == 0);
    CHECK(v0->first == 5);
    const Seen* s0 = find("args.s", 0);
    REQUIRE(s0);
    CHECK(s0->type == TypeId::String);
    CHECK(s0->rows == 3);
    CHECK(s0->nulls == 2);
    const Seen* n0 = find("name", 0);
    REQUIRE(n0);
    CHECK(n0->type == TypeId::String);
    CHECK(find("args.absent", 0) == nullptr);
    // The second chunk is its own column of one row.
    const Seen* v1 = find("args.v", 3);
    REQUIRE(v1);
    CHECK(v1->rows == 1);
    CHECK(v1->first == 9);
    REQUIRE(data.granules.size() == 2);
    CHECK(data.file.value_or("") == "file");
}

TEST_CASE("an extension with columns and lines gets both") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    dftracer::utils::dstd::StringIntern intern;
    ColumnLog log;
    log.wanted = {"*"};
    log.lines_too = true;
    dstd::Vector<dstd::Vector<dstd::Vector<FoldEvent>>> chunks(1);
    chunks[0].resize(1);
    chunks[0][0].push_back(make_event(intern, "read", 5, "x"));
    dstd::String dir = env.dir();
    const auto data =
        build_columns(log, dir, {chunks.data(), chunks.size()}, intern);
    CHECK(log.lines.size() == 1);
    // name, pid, tid, ts, args.v, args.s
    CHECK(log.seen.size() == 6);
    CHECK(data.granules.size() == 1);
}

TEST_CASE("a path with two types in a chunk stores nothing for the chunk") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    dftracer::utils::dstd::StringIntern intern;
    ColumnLog log;
    log.wanted = {"args.v"};
    FoldEvent mixed = make_event(intern, "read", 1, dstd::nullopt);
    mixed.args.clear();
    mixed.args.push_back({intern.get_or_insert("v"), FoldEvent::ArgValue(2.5)});
    dstd::Vector<dstd::Vector<dstd::Vector<FoldEvent>>> chunks(2);
    chunks[0].resize(1);
    chunks[0][0].push_back(make_event(intern, "read", 1, dstd::nullopt));
    chunks[0][0].push_back(std::move(mixed));
    chunks[1].resize(1);
    chunks[1][0].push_back(make_event(intern, "read", 3, dstd::nullopt));
    dstd::String dir = env.dir();
    const auto data =
        build_columns(log, dir, {chunks.data(), chunks.size()}, intern);
    // Chunk 0 is not stored, so it is never pruned; chunk 1 is stored. The
    // file payload is left out, since it would not cover chunk 0.
    REQUIRE(data.granules.size() == 1);
    CHECK(data.granules[0].first == 1);
    CHECK_FALSE(data.file.has_value());
    REQUIRE(log.seen.size() == 1);
    CHECK(log.seen[0].rows == 1);
}

TEST_CASE("a failing step_column stores nothing for the chunk") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    dftracer::utils::dstd::StringIntern intern;
    ColumnLog log;
    log.wanted = {"args.v"};
    log.fail_column = true;
    dstd::Vector<dstd::Vector<dstd::Vector<FoldEvent>>> chunks(1);
    chunks[0].resize(1);
    chunks[0][0].push_back(make_event(intern, "read", 1, dstd::nullopt));
    dstd::String dir = env.dir();
    const auto data =
        build_columns(log, dir, {chunks.data(), chunks.size()}, intern);
    CHECK(data.granules.empty());
    CHECK_FALSE(data.file.has_value());
}

TEST_CASE("a frame goes to step_column and a lines-only extension is failed") {
    ColumnLog log;
    log.wanted = {"a", "c"};
    ExtensionBuilders builders(
        {ExtensionPtr(dstd::make_shared<ColumnsExtension>(log))});
    const std::int64_t a[] = {1, 2, 3};
    const double b[] = {0.5, 1.5, 2.5};
    const std::int32_t c[] = {1, 2, 3};
    dataframe::DataFrame frame;
    frame.names.emplace_back(dstd::StringView("a"));
    frame.names.emplace_back(dstd::StringView("b"));
    frame.columns.push_back(Series::flat_i64(a, 3));
    frame.columns.push_back(Series::flat_f64(b, 3));
    builders.step_frame(0, frame);
    // b was not asked for.
    REQUIRE(log.seen.size() == 1);
    CHECK(log.seen[0].path == "a");
    CHECK(log.seen[0].rows == 3);

    // A column of a type the plugin ABI does not carry fails the chunk.
    dataframe::DataFrame narrow;
    narrow.names.emplace_back(dstd::StringView("c"));
    narrow.columns.push_back(Series::flat(TypeId::Int32, c, 3));
    builders.step_frame(1, narrow);
    CHECK(log.seen.size() == 1);
}
