// SPDX-License-Identifier: MIT
// The index of a file a plugin file format reads: the Indexer builds one
// granule of summaries per chunk of the format (tests/plugins/
// chunked_format_plugin.c), records which format and settings built it, and a
// scan of the file reads only the chunks the index cannot rule out.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/format_index.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/index/plan/prune.h>
#include <dftracer/utils/trace/index/tables.h>
#include <dlfcn.h>
#include <doctest/doctest.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;
using trace::index::Indexer;
using trace::index::IndexerOptions;

#ifndef CHUNKED_FORMAT_PLUGIN_PATH
#error "CHUNKED_FORMAT_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef CHUNKED_ALT_PLUGIN_PATH
#error "CHUNKED_ALT_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef CHUNKED_V2_PLUGIN_PATH
#error "CHUNKED_V2_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef SDK_CHUNKED_PLUGIN_PATH
#error "SDK_CHUNKED_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef INDEX_COLUMNS_PLUGIN_PATH
#error "INDEX_COLUMNS_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

constexpr std::int64_t CHUNK_ROWS = 4;

// A plugin's counters. The test keeps its own reference to each plugin library
// for the whole run, so the counters stay readable after ~Plugins drops the
// host's reference.
template <class Fn>
Fn plugin_symbol(dstd::CStringView path, dstd::CStringView symbol) {
    static dstd::Vector<void*> handles;
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    REQUIRE(handle != nullptr);
    handles.push_back(handle);
    auto fn = reinterpret_cast<Fn>(dlsym(handle, symbol.c_str()));
    REQUIRE(fn != nullptr);
    return fn;
}

void reset_counters(dstd::CStringView plugin) {
    plugin_symbol<void (*)()>(plugin, "chunked_format_reset")();
}

// The chunks the plugin's scan() opened since the last reset, in order.
dstd::Vector<std::int64_t> scanned_chunks(dstd::CStringView plugin) {
    std::int64_t log[256];
    const std::int64_t n =
        plugin_symbol<std::int64_t (*)(std::int64_t*, std::int64_t)>(
            plugin, "chunked_format_scan_log")(log, 256);
    return dstd::Vector<std::int64_t>(log, log + n);
}

std::int64_t opened_files(dstd::CStringView plugin) {
    return plugin_symbol<std::int64_t (*)()>(plugin,
                                             "chunked_format_open_count")();
}

dstd::Optional<plugins::host::Plugins> load(
    std::initializer_list<const char*> paths) {
    plugins::host::Plugins::Builder builder;
    for (const char* path : paths) builder.add(dstd::String(path));
    auto built = builder.build();
    REQUIRE(built.has_value());
    return std::move(built).value();
}

struct Scratch {
    dstd::Path root = dftu_utils_test::make_unique_test_path("format_index");
    dstd::String data;
    dstd::String index;

    Scratch() {
        data = dstd::String((root / "data").str());
        index = dstd::String((root / "index").str());
        REQUIRE(dstd::fs::create_directories(dstd::PathView(data)).has_value());
        REQUIRE(
            dstd::fs::create_directories(dstd::PathView(index)).has_value());
    }
};

// `rows` rows from id `first`, as the rows format reads them: a magic line,
// then `<id> n<id>`. `offsets` gets the byte offset of each row's line.
dstd::String write_rows(dstd::StringView dir, dstd::StringView name,
                        std::int64_t first, std::int64_t rows,
                        dstd::StringView magic = "CHK1",
                        dstd::Vector<std::uint64_t>* offsets = nullptr) {
    dstd::String text(magic);
    text.push_back('\n');
    for (std::int64_t i = 0; i < rows; ++i) {
        if (offsets) offsets->push_back(text.size());
        const std::int64_t id = first + i;
        text += dstd::to_string(id) + " n" + dstd::to_string(id) + "\n";
    }
    const dstd::String path = dstd::String(dir) + "/" + dstd::String(name);
    REQUIRE(dstd::fs::write_file(dstd::PathView(path), text).has_value());
    return path;
}

IndexerOptions options_in(const Scratch& s) {
    IndexerOptions o;
    o.index_dir = s.index;
    return o;
}

Indexer open_indexer(dstd::Vector<dstd::String> paths,
                     const IndexerOptions& o) {
    return dftu_utils_test::ok(Indexer::open(std::move(paths), o));
}

trace::index::Snapshot snapshot_of(const trace::index::IndexStatus& status) {
    return dftu_utils_test::ok(
        trace::index::plan::open_snapshot(status.index_path));
}

trace::index::store::FormatRecord record_of(
    const trace::index::Snapshot& snapshot, dstd::StringView path) {
    const auto file = snapshot.file(fileio::get_logical_path(path));
    REQUIRE(file.has_value());
    auto record =
        dftu_utils_test::ok(trace::index::format_record(snapshot, *file));
    REQUIRE(record.has_value());
    return std::move(*record);
}

DataFrame run(const LazyFrame& lf) {
    return dftu_utils_test::ok(default_runtime().submit(lf.collect()).get());
}

dstd::Vector<std::int64_t> ids_of(const DataFrame& df) {
    if (df.num_rows() == 0) return {};
    Series column = df.column("id");
    REQUIRE(column.valid());
    if (!column.is_flat()) column = column.materialize();
    const std::int64_t* data = column.data<std::int64_t>();
    return data ? dstd::Vector<std::int64_t>(data, data + column.length())
                : dstd::Vector<std::int64_t>{};
}

Expr cmp_i64(CmpOp op, std::int32_t column, std::int64_t value) {
    dftu_scalar s;
    s.kind = DFTU_SCALAR_TAG_I64;
    s.value.i = value;
    return expr_cmp(op, col(column), s);
}

dstd::String show(dstd::Span<const std::int64_t> v) {
    dstd::String out = "[";
    for (std::size_t i = 0; i < v.size(); ++i)
        out += (i ? "," : "") + dstd::to_string(v[i]);
    return out + "]";
}

dstd::String show_names(dstd::Span<const dstd::String> v) {
    dstd::String out;
    for (const dstd::String& name : v) out += name + ",";
    return out;
}

dstd::Vector<std::int64_t> iota(std::int64_t first, std::int64_t last) {
    dstd::Vector<std::int64_t> out;
    for (std::int64_t i = first; i <= last; ++i) out.push_back(i);
    return out;
}

const trace::index::ExtensionEntry* entry_named(
    const trace::index::FileManifest& m, dstd::StringView name) {
    for (const auto& e : m.extensions)
        if (e.name == name) return &e;
    return nullptr;
}

}  // namespace

TEST_CASE("the indexer indexes a plugin-format file, a granule per chunk") {
    Scratch s;
    auto plugins = load({CHUNKED_FORMAT_PLUGIN_PATH});
    dstd::Vector<std::uint64_t> offsets;
    const dstd::String path =
        write_rows(s.data, "a.rows", 1, 16, "CHK1", &offsets);
    auto ix = open_indexer({path}, options_in(s));

    const auto before = dftu_utils_test::ok(ix.status());
    CHECK(before.total == 1);
    CHECK(before.needs_work.size() == 1);

    const auto built = dftu_utils_test::ok(ix.build());
    CHECK(built.ready.size() == 1);
    CHECK(built.needs_work.empty());
    CHECK(built.indexed == 1);

    const auto snapshot = snapshot_of(built);
    const auto record = record_of(snapshot, path);
    CHECK(record.format == "chunked_format.rows");
    CHECK(record.feeder == trace::index::store::FeederKind::Column);
    CHECK(record.chunk_count == 4);
    // The token of a chunk is the byte offset of its first row.
    REQUIRE(record.tokens.size() == 4);
    for (std::int64_t c = 0; c < 4; ++c)
        CHECK(record.tokens[static_cast<std::size_t>(c)] ==
              offsets[static_cast<std::size_t>(c * CHUNK_ROWS)]);

    const auto file = snapshot.file(fileio::get_logical_path(path));
    REQUIRE(file.has_value());
    for (auto ext : {trace::index::store::IndexExtension::Zonemap,
                     trace::index::store::IndexExtension::Bloom,
                     trace::index::store::IndexExtension::Counts}) {
        const auto granules = dftu_utils_test::ok(
            trace::index::granules(snapshot, *file, ext, "id"));
        CHECK(granules.size() == 4);
    }

    const auto manifest = dftu_utils_test::ok(ix.manifest());
    REQUIRE(manifest.size() == 1);
    for (const char* name :
         {"core.format", "zonemap", "bloom", "counts", "postings"}) {
        const auto* e = entry_named(manifest[0], name);
        REQUIRE(e != nullptr);
        CHECK_MESSAGE(e->current, name);
    }
}

TEST_CASE("a filter skips the chunks the index rules out, rows unchanged") {
    Scratch s;
    Scratch none;
    auto plugins = load({CHUNKED_FORMAT_PLUGIN_PATH});
    const dstd::String path = write_rows(s.data, "a.rows", 1, 16);
    auto ix = open_indexer({path}, options_in(s));
    dftu_utils_test::ok(ix.build());

    struct Case {
        const char* what;
        Expr filter;
        dstd::Vector<std::int64_t> rows;
        dstd::Vector<std::int64_t> chunks;
    };
    const dstd::Vector<std::int64_t> all_chunks = {0, 1, 2, 3};
    const dstd::Vector<Case> cases = {
        {"a range past the first chunks",
         cmp_i64(CmpOp::Gt, 0, 12),
         iota(13, 16),
         {3}},
        {"an equality", cmp_i64(CmpOp::Eq, 0, 6), {6}, {1}},
        {"a range inside one chunk",
         cmp_i64(CmpOp::Gt, 0, 4) & cmp_i64(CmpOp::Lt, 0, 9),
         iota(5, 8),
         {1}},
        {"a filter on another column",
         cmp_i64(CmpOp::Ge, 2, 130),
         iota(13, 16),
         {3}},
        {"a value in no chunk", cmp_i64(CmpOp::Gt, 0, 100), {}, {}},
        {"an OR the summaries cannot judge",
         cmp_i64(CmpOp::Lt, 0, 2) | cmp_i64(CmpOp::Gt, 0, 15),
         {1, 16},
         all_chunks},
    };
    for (const Case& c : cases) {
        CAPTURE(c.what);
        reset_counters(CHUNKED_FORMAT_PLUGIN_PATH);
        const DataFrame indexed =
            run(LazyFrame::scan_file(path, s.index).filter(c.filter));
        CHECK(show(scanned_chunks(CHUNKED_FORMAT_PLUGIN_PATH)) ==
              show(c.chunks));
        CHECK(show(ids_of(indexed)) == show(c.rows));

        // The same scan with no index for the file reads every chunk, and the
        // rows are the same.
        reset_counters(CHUNKED_FORMAT_PLUGIN_PATH);
        const DataFrame whole =
            run(LazyFrame::scan_file(path, none.index).filter(c.filter));
        CHECK(show(scanned_chunks(CHUNKED_FORMAT_PLUGIN_PATH)) ==
              show(all_chunks));
        CHECK(show(ids_of(whole)) == show(c.rows));
    }

    reset_counters(CHUNKED_FORMAT_PLUGIN_PATH);
    CHECK(run(LazyFrame::scan_file(path, s.index)).num_rows() == 16);
    CHECK(show(scanned_chunks(CHUNKED_FORMAT_PLUGIN_PATH)) == show(all_chunks));
}

TEST_CASE("a changed file is indexed again and no other file is touched") {
    Scratch s;
    auto plugins = load({CHUNKED_FORMAT_PLUGIN_PATH});
    const dstd::String a = write_rows(s.data, "a.rows", 1, 16);
    const dstd::String b = write_rows(s.data, "b.rows", 101, 8);
    auto ix = open_indexer({a, b}, options_in(s));
    const auto first = dftu_utils_test::ok(ix.build());
    CHECK(first.ready.size() == 2);
    const auto b_before = record_of(snapshot_of(first), b);

    // Two more chunks of rows: the size changes, whatever the mtime does.
    write_rows(s.data, "a.rows", 1, 24);
    const auto stale = dftu_utils_test::ok(ix.status());
    REQUIRE(stale.needs_work.size() == 1);
    CHECK(stale.needs_work[0] == a);
    REQUIRE(stale.ready.size() == 1);
    CHECK(stale.ready[0] == b);

    // The stale index is not used: every chunk is read, and the new rows are
    // there.
    reset_counters(CHUNKED_FORMAT_PLUGIN_PATH);
    const DataFrame rows =
        run(LazyFrame::scan_file(a, s.index).filter(cmp_i64(CmpOp::Gt, 0, 20)));
    CHECK(scanned_chunks(CHUNKED_FORMAT_PLUGIN_PATH) ==
          dstd::Vector<std::int64_t>{0, 1, 2, 3, 4, 5});
    CHECK(show(ids_of(rows)) == show(iota(21, 24)));

    reset_counters(CHUNKED_FORMAT_PLUGIN_PATH);
    const auto rebuilt = dftu_utils_test::ok(ix.build());
    CHECK(rebuilt.indexed == 1);
    CHECK(rebuilt.ready.size() == 2);
    CHECK(opened_files(CHUNKED_FORMAT_PLUGIN_PATH) == 1);

    const auto snapshot = snapshot_of(rebuilt);
    CHECK(record_of(snapshot, a).chunk_count == 6);
    const auto b_after = record_of(snapshot, b);
    CHECK(b_after.tokens == b_before.tokens);
    CHECK(b_after.params_hash == b_before.params_hash);

    reset_counters(CHUNKED_FORMAT_PLUGIN_PATH);
    const DataFrame skipped =
        run(LazyFrame::scan_file(a, s.index).filter(cmp_i64(CmpOp::Gt, 0, 20)));
    CHECK(scanned_chunks(CHUNKED_FORMAT_PLUGIN_PATH) ==
          dstd::Vector<std::int64_t>{5});
    CHECK(show(ids_of(skipped)) == show(iota(21, 24)));
}

TEST_CASE("another owner or other settings index the file again") {
    Scratch s;
    const dstd::String path = write_rows(s.data, "a.rows", 1, 16);
    {
        auto plugins = load({CHUNKED_FORMAT_PLUGIN_PATH});
        auto ix = open_indexer({path}, options_in(s));
        dftu_utils_test::ok(ix.build());
        CHECK(dftu_utils_test::ok(ix.status()).needs_work.empty());

        // The same format with other settings is a new index.
        IndexerOptions tuned = options_in(s);
        tuned.bloom->false_positive_rate = 0.05;
        auto other = open_indexer({path}, tuned);
        CHECK(dftu_utils_test::ok(other.status()).needs_work.size() == 1);
        dftu_utils_test::ok(other.build());
        CHECK(dftu_utils_test::ok(other.status()).needs_work.empty());
        // The original settings are now the stale ones.
        CHECK(dftu_utils_test::ok(ix.status()).needs_work.size() == 1);
        dftu_utils_test::ok(ix.build());
    }

    // Another plugin's format claims the file: its index is the other
    // format's, so it is indexed again.
    auto plugins = load({CHUNKED_ALT_PLUGIN_PATH});
    auto ix = open_indexer({path}, options_in(s));
    const auto stale = dftu_utils_test::ok(ix.status());
    CHECK(stale.needs_work.size() == 1);
    reset_counters(CHUNKED_ALT_PLUGIN_PATH);
    const auto built = dftu_utils_test::ok(ix.build());
    CHECK(built.ready.size() == 1);
    CHECK(opened_files(CHUNKED_ALT_PLUGIN_PATH) == 1);
    CHECK(record_of(snapshot_of(built), path).format == "chunked_alt.rows");
    CHECK(dftu_utils_test::ok(ix.status()).needs_work.empty());
}

TEST_CASE("rebuild_extension builds the summary a format file lacks") {
    Scratch s;
    auto plugins = load({CHUNKED_FORMAT_PLUGIN_PATH});
    const dstd::String path = write_rows(s.data, "a.rows", 1, 16);
    IndexerOptions zonemap_only = options_in(s);
    zonemap_only.extensions = {"zonemap"};
    auto ix = open_indexer({path}, zonemap_only);
    const auto built = dftu_utils_test::ok(ix.build());
    using trace::index::store::IndexExtension;
    const auto has = [&](const trace::index::IndexStatus& status,
                         IndexExtension ext) {
        const auto snapshot = snapshot_of(status);
        const auto file = snapshot.file(fileio::get_logical_path(path));
        REQUIRE(file.has_value());
        const auto state = trace::index::extension_state(snapshot, *file, ext);
        return state.has_value() && state->ready;
    };
    CHECK(has(built, IndexExtension::Zonemap));
    CHECK(!has(built, IndexExtension::Counts));

    reset_counters(CHUNKED_FORMAT_PLUGIN_PATH);
    const auto rebuilt = dftu_utils_test::ok(ix.rebuild_extension("counts"));
    CHECK(opened_files(CHUNKED_FORMAT_PLUGIN_PATH) == 1);
    CHECK(has(rebuilt, IndexExtension::Zonemap));
    CHECK(has(rebuilt, IndexExtension::Counts));
    // The options still name only the zonemap, and a file with more summaries
    // than that is current.
    CHECK(dftu_utils_test::ok(ix.status()).needs_work.empty());
}

TEST_CASE("a newer plugin of the same name indexes its files again") {
    Scratch s;
    const dstd::String path = write_rows(s.data, "a.rows", 1, 16);
    const Expr filter = cmp_i64(CmpOp::Gt, 0, 12);
    {
        auto plugins = load({CHUNKED_FORMAT_PLUGIN_PATH});
        auto ix = open_indexer({path}, options_in(s));
        const auto built = dftu_utils_test::ok(ix.build());
        CHECK(record_of(snapshot_of(built), path).plugin_version == "1.0");
        CHECK(dftu_utils_test::ok(ix.status()).needs_work.empty());
    }

    // The tokens and the format name are unchanged, but the plugin that wrote
    // the index is not the one that reads the file now.
    auto plugins = load({CHUNKED_V2_PLUGIN_PATH});
    auto ix = open_indexer({path}, options_in(s));
    CHECK(dftu_utils_test::ok(ix.status()).needs_work.size() == 1);
    reset_counters(CHUNKED_V2_PLUGIN_PATH);
    CHECK(
        show(ids_of(run(LazyFrame::scan_file(path, s.index).filter(filter)))) ==
        show(iota(13, 16)));
    CHECK(show(scanned_chunks(CHUNKED_V2_PLUGIN_PATH)) ==
          show(dstd::Vector<std::int64_t>{0, 1, 2, 3}));

    const auto rebuilt = dftu_utils_test::ok(ix.build());
    CHECK(rebuilt.indexed == 1);
    CHECK(record_of(snapshot_of(rebuilt), path).plugin_version == "2.0");
    CHECK(dftu_utils_test::ok(ix.status()).needs_work.empty());
    reset_counters(CHUNKED_V2_PLUGIN_PATH);
    CHECK(
        show(ids_of(run(LazyFrame::scan_file(path, s.index).filter(filter)))) ==
        show(iota(13, 16)));
    CHECK(show(scanned_chunks(CHUNKED_V2_PLUGIN_PATH)) ==
          show(dstd::Vector<std::int64_t>{3}));
}

TEST_CASE("a stale check of one folder keeps the index of another") {
    Scratch s;
    auto plugins = load({CHUNKED_FORMAT_PLUGIN_PATH});
    REQUIRE(dstd::fs::create_directories(dstd::PathView(s.data + "/d1"))
                .has_value());
    REQUIRE(dstd::fs::create_directories(dstd::PathView(s.data + "/d2"))
                .has_value());
    const dstd::String a = write_rows(s.data + "/d1", "a.rows", 1, 16);
    const dstd::String b = write_rows(s.data + "/d2", "b.rows", 101, 16);
    {
        auto both = open_indexer({a, b}, options_in(s));
        CHECK(dftu_utils_test::ok(both.build()).ready.size() == 2);
    }
    const auto b_before = record_of(
        snapshot_of(
            dftu_utils_test::ok(open_indexer({b}, options_in(s)).status())),
        b);

    // The first folder changes; an Indexer over it alone builds it.
    write_rows(s.data + "/d1", "a.rows", 1, 20);
    reset_counters(CHUNKED_FORMAT_PLUGIN_PATH);
    auto first = open_indexer({a}, options_in(s));
    CHECK(dftu_utils_test::ok(first.status()).needs_work.size() == 1);
    dftu_utils_test::ok(first.build());
    CHECK(opened_files(CHUNKED_FORMAT_PLUGIN_PATH) == 1);

    auto second = open_indexer({b}, options_in(s));
    const auto status = dftu_utils_test::ok(second.status());
    CHECK(status.needs_work.empty());
    CHECK(status.ready.size() == 1);
    const auto b_after = record_of(snapshot_of(status), b);
    CHECK(b_after.tokens == b_before.tokens);
    CHECK(dftu_utils_test::ok(second.files()).size() == 1);

    reset_counters(CHUNKED_FORMAT_PLUGIN_PATH);
    CHECK(ids_of(run(LazyFrame::scan_file(b, s.index)
                         .filter(cmp_i64(CmpOp::Gt, 0, 112)))) ==
          iota(113, 116));
    CHECK(scanned_chunks(CHUNKED_FORMAT_PLUGIN_PATH) ==
          dstd::Vector<std::int64_t>{3});
}

TEST_CASE("a format with no chunk tokens is one chunk to the index") {
    Scratch s;
    auto plugins = load({CHUNKED_FORMAT_PLUGIN_PATH});
    const dstd::String path = write_rows(s.data, "p.rows", 1, 16, "PLN1");
    auto ix = open_indexer({path}, options_in(s));
    const auto built = dftu_utils_test::ok(ix.build());
    const auto snapshot = snapshot_of(built);
    const auto record = record_of(snapshot, path);
    CHECK(record.format == "chunked_format.plain");
    CHECK(record.tokens.empty());
    CHECK(record.chunk_count == 4);
    const auto file = snapshot.file(fileio::get_logical_path(path));
    REQUIRE(file.has_value());
    CHECK(dftu_utils_test::ok(trace::index::granules(
                                  snapshot, *file,
                                  trace::index::store::IndexExtension::Zonemap,
                                  "id"))
              .size() == 1);

    // The index can rule the whole file out and cannot rule one chunk out.
    reset_counters(CHUNKED_FORMAT_PLUGIN_PATH);
    CHECK(run(LazyFrame::scan_file(path, s.index)
                  .filter(cmp_i64(CmpOp::Gt, 0, 100)))
              .num_rows() == 0);
    CHECK(scanned_chunks(CHUNKED_FORMAT_PLUGIN_PATH).empty());
    reset_counters(CHUNKED_FORMAT_PLUGIN_PATH);
    CHECK(ids_of(run(LazyFrame::scan_file(path, s.index)
                         .filter(cmp_i64(CmpOp::Gt, 0, 12)))) == iota(13, 16));
    CHECK(scanned_chunks(CHUNKED_FORMAT_PLUGIN_PATH) ==
          dstd::Vector<std::int64_t>{0, 1, 2, 3});
}

// Rewrites `rows` ids from `first` in place with the same byte size as before
// and an mtime a minute later, so only the stat's mtime says the file changed.
static void rewrite_same_size(dstd::StringView dir, dstd::StringView name,
                              std::int64_t first, std::int64_t rows,
                              dstd::StringView magic) {
    const dstd::String path = write_rows(dir, name, first, rows, magic);
    struct stat st;
    REQUIRE(::stat(path.c_str(), &st) == 0);
    struct timeval times[2];
    times[0].tv_sec = times[1].tv_sec = st.st_mtime + 60;
    times[0].tv_usec = times[1].tv_usec = 0;
    REQUIRE(::utimes(path.c_str(), times) == 0);
}

TEST_CASE("a scan of a rewritten file ignores its stale index") {
    for (const char* magic : {"CHK1", "PLN1"}) {
        CAPTURE(magic);
        Scratch s;
        Scratch none;
        auto plugins = load({CHUNKED_FORMAT_PLUGIN_PATH});
        const dstd::String path = write_rows(s.data, "r.rows", 10, 16, magic);
        auto ix = open_indexer({path}, options_in(s));
        dftu_utils_test::ok(ix.build());

        // The index proves that no id is above 50.
        reset_counters(CHUNKED_FORMAT_PLUGIN_PATH);
        CHECK(run(LazyFrame::scan_file(path, s.index)
                      .filter(cmp_i64(CmpOp::Gt, 0, 50)))
                  .num_rows() == 0);
        CHECK(scanned_chunks(CHUNKED_FORMAT_PLUGIN_PATH).empty());

        // The same chunk boundaries and token values, other rows.
        rewrite_same_size(s.data, "r.rows", 60, 16, magic);
        reset_counters(CHUNKED_FORMAT_PLUGIN_PATH);
        const DataFrame got = run(LazyFrame::scan_file(path, s.index)
                                      .filter(cmp_i64(CmpOp::Gt, 0, 50)));
        CHECK(show(scanned_chunks(CHUNKED_FORMAT_PLUGIN_PATH)) ==
              show(dstd::Vector<std::int64_t>{0, 1, 2, 3}));
        const DataFrame whole = run(LazyFrame::scan_file(path, none.index)
                                        .filter(cmp_i64(CmpOp::Gt, 0, 50)));
        CHECK(show(ids_of(got)) == show(iota(60, 75)));
        CHECK(show(ids_of(got)) == show(ids_of(whole)));
    }
}

static dstd::Vector<dstd::String> column_names(const DataFrame& df) {
    dstd::Vector<dstd::String> names;
    for (const Series& column : df.columns)
        names.push_back(dstd::String(column.name()));
    return names;
}

TEST_CASE(
    "a scan the index rules out whole gives what a scan of no index does") {
    Scratch s;
    Scratch none;
    auto plugins = load({CHUNKED_FORMAT_PLUGIN_PATH});
    const dstd::String path = write_rows(s.data, "e.rows", 1, 16);
    auto ix = open_indexer({path}, options_in(s));
    dftu_utils_test::ok(ix.build());
    const Expr nothing = cmp_i64(CmpOp::Gt, 0, 100);

    struct Query {
        const char* what;
        LazyFrame (*shape)(const LazyFrame&);
    };
    const Query queries[] = {
        {"a collect", [](const LazyFrame& lf) { return lf; }},
        {"a projection",
         [](const LazyFrame& lf) {
             return lf.select({dstd::String("name"), dstd::String("id")});
         }},
        {"a count",
         [](const LazyFrame& lf) {
             return lf.group_by(
                 dstd::Vector<dstd::String>{},
                 dstd::Vector<GroupAgg>{{Agg::Count, "id", "n"}});
         }},
        {"a group_by",
         [](const LazyFrame& lf) {
             return lf.group_by(
                 dstd::String("name"),
                 dstd::Vector<GroupAgg>{{Agg::Count, "id", "n"},
                                        {Agg::Sum, "id", "total"}});
         }},
    };
    for (const Query& q : queries) {
        CAPTURE(q.what);
        reset_counters(CHUNKED_FORMAT_PLUGIN_PATH);
        const DataFrame got =
            run(q.shape(LazyFrame::scan_file(path, s.index).filter(nothing)));
        CHECK(scanned_chunks(CHUNKED_FORMAT_PLUGIN_PATH).empty());
        const DataFrame want = run(
            q.shape(LazyFrame::scan_file(path, none.index).filter(nothing)));
        CHECK(got.num_rows() == want.num_rows());
        CHECK(show_names(column_names(got)) == show_names(column_names(want)));
    }
}

TEST_CASE("plugin column extensions get a granule per chunk") {
    Scratch s;
    auto plugins =
        load({CHUNKED_FORMAT_PLUGIN_PATH, INDEX_COLUMNS_PLUGIN_PATH});
    const dstd::String path = write_rows(s.data, "a.rows", 1, 16);
    auto ix = open_indexer({path}, options_in(s));
    const auto built = dftu_utils_test::ok(ix.build());
    const auto manifest = dftu_utils_test::ok(ix.manifest());
    REQUIRE(manifest.size() == 1);
    const auto* range = entry_named(manifest[0], "index_columns.range");
    REQUIRE(range != nullptr);
    CHECK(range->ready);
    CHECK(range->current);

    const auto snapshot = snapshot_of(built);
    const auto file = snapshot.file(fileio::get_logical_path(path));
    REQUIRE(file.has_value());
    const auto data = dftu_utils_test::ok(
        trace::index::plugin(snapshot, *file, "index_columns.range"));
    REQUIRE(data.has_value());
    CHECK(data->granules.size() == 4);
    CHECK(data->file.has_value());
    CHECK(dftu_utils_test::ok(ix.status()).needs_work.empty());
}

TEST_CASE("a trace the built-in reader indexes is not a format's index") {
    Scratch s;
    dftu_utils_test::TestEnvironment env(40);
    const dstd::String gz = env.create_dft_test_gzip_file(40);
    auto plugins = load({CHUNKED_FORMAT_PLUGIN_PATH});
    const dstd::String path = write_rows(s.data, "a.rows", 1, 16);

    // One build indexes both kinds of file, each by its own reader.
    auto ix = open_indexer({gz, path}, options_in(s));
    const auto before = dftu_utils_test::ok(ix.status());
    CHECK(before.total == 2);
    CHECK(before.needs_work.size() == 2);
    const auto built = dftu_utils_test::ok(ix.build());
    CHECK(built.ready.size() == 2);
    CHECK(built.needs_work.empty());

    const auto snapshot = snapshot_of(built);
    const auto trace_file = snapshot.file(fileio::get_logical_path(gz));
    REQUIRE(trace_file.has_value());
    CHECK_FALSE(
        dftu_utils_test::ok(trace::index::format_record(snapshot, *trace_file))
            .has_value());
    CHECK(record_of(snapshot, path).format == "chunked_format.rows");

    // A format that claims a file the built-in reader indexed finds no
    // current index of its own.
    trace::index::FormatIndexSettings settings;
    settings.config.extensions = trace::index::store::PRUNING_EXTENSIONS;
    common::InputFileStat stat;
    REQUIRE(common::stat_input(gz, stat));
    const trace::index::SourceStat now{static_cast<std::uint64_t>(stat.mtime),
                                       stat.size, 0};
    CHECK_FALSE(dftu_utils_test::ok(trace::index::format_file_current(
        snapshot, fileio::get_logical_path(gz), now, "chunked_format.rows",
        "1.0", settings)));
}

TEST_CASE("the C ABI scan reads the index of index_dir and skips chunks") {
    Scratch s;
    auto plugins = load({CHUNKED_FORMAT_PLUGIN_PATH});
    const dstd::String path = write_rows(s.data, "c.rows", 1, 16);
    auto ix = open_indexer({path}, options_in(s));
    dftu_utils_test::ok(ix.build());

    dftu_scalar bound;
    bound.kind = DFTU_SCALAR_TAG_I64;
    bound.value.i = 12;
    dftu_expr* id = dftu_expr_col(0);
    dftu_expr* pred = dftu_expr_cmp(DFTU_CMP_GT, id, bound);
    REQUIRE(pred != nullptr);

    dftu_lazyframe* scan = nullptr;
    REQUIRE(dftu_lazyframe_scan_file(path.c_str(), s.index.c_str(),
                                     s.index.size(), &scan) == 0);
    dftu_lazyframe* filtered = dftu_lazyframe_filter(scan, pred);
    REQUIRE(filtered != nullptr);
    reset_counters(CHUNKED_FORMAT_PLUGIN_PATH);
    dftu_dataframe* out = dftu_lazyframe_collect(filtered, -1);
    REQUIRE(out != nullptr);
    CHECK(dftu_dataframe_num_rows(out) == 4);
    CHECK(show(scanned_chunks(CHUNKED_FORMAT_PLUGIN_PATH)) ==
          show(dstd::Vector<std::int64_t>{3}));
    dftu_dataframe_free(out);
    dftu_lazyframe_free(filtered);
    dftu_lazyframe_free(scan);

    // Without index_dir the index beside the file does not exist: every chunk.
    REQUIRE(dftu_lazyframe_scan_file(path.c_str(), nullptr, 0, &scan) == 0);
    filtered = dftu_lazyframe_filter(scan, pred);
    reset_counters(CHUNKED_FORMAT_PLUGIN_PATH);
    out = dftu_lazyframe_collect(filtered, -1);
    REQUIRE(out != nullptr);
    CHECK(dftu_dataframe_num_rows(out) == 4);
    CHECK(show(scanned_chunks(CHUNKED_FORMAT_PLUGIN_PATH)) ==
          show(dstd::Vector<std::int64_t>{0, 1, 2, 3}));
    dftu_dataframe_free(out);
    dftu_lazyframe_free(filtered);
    dftu_lazyframe_free(scan);

    CHECK(dftu_lazyframe_scan_file(path.c_str(), nullptr, 3, &scan) != 0);
    dftu_expr_free(pred);
    dftu_expr_free(id);
}

TEST_CASE("an SDK format with a chunk_token hook is indexed and skipped") {
    Scratch s;
    auto plugins = load({SDK_CHUNKED_PLUGIN_PATH});
    dstd::String text("SDKC\n");
    for (int i = 1; i <= 16; ++i) text += dstd::to_string(i) + "\n";
    const dstd::String path = dstd::String(s.data) + "/s.sdk";
    REQUIRE(dstd::fs::write_file(dstd::PathView(path), text).has_value());
    auto ix = open_indexer({path}, options_in(s));
    dftu_utils_test::ok(ix.build());

    const dstd::String log = dstd::String(s.data) + "/scanned.log";
    REQUIRE(setenv("SDK_CHUNKED_LOG", log.c_str(), 1) == 0);
    const DataFrame got = run(
        LazyFrame::scan_file(path, s.index).filter(cmp_i64(CmpOp::Gt, 0, 12)));
    unsetenv("SDK_CHUNKED_LOG");
    auto logged = dstd::fs::read_file(dstd::PathView(log));
    REQUIRE(logged.has_value());
    CHECK(dstd::String(*logged) == "3\n");
    CHECK(show(ids_of(got)) == show(iota(13, 16)));
}
