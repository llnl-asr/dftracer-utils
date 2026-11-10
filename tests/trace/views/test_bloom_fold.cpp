// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/internal/frame_native.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/index_fold_driver.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/extensions/rowset_fold.h>
#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/source.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <memory/fresh_process.h>
#include <simdjson.h>
#include <task_result.h>
#include <testing_runtime.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <span>
#include <unordered_map>

using namespace dftracer::utils;

using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

using namespace dftracer::utils::trace::views::detail;
using namespace dftracer::utils::index::build;
using namespace dftracer::utils::index::extensions;

namespace {

FoldEvent extract_fold_event(simdjson::dom::element root,
                             dstd::StringIntern& intern, bool needs_args,
                             const dstd::Vector<dstd::String>* extra = nullptr,
                             bool capture_schema = false) {
    FoldEvent ev;
    extract_fold_event_into(ev, root, intern, needs_args, extra,
                            capture_schema);
    return ev;
}

dstd::String create_bloom_trace(TestEnvironment& env, int n,
                                std::size_t member_bytes) {
    dstd::String pfw = env.dir() + "/bloom.pfw";
    {
        dftu_utils_test::FileOut ofs(pfw);
        for (int i = 0; i < n; ++i) {
            ofs << R"({"ph":"X","name":")" << (i % 2 ? "read" : "write")
                << R"(","cat":")" << (i % 3 ? "POSIX" : "STDIO")
                << R"(","pid":)" << (1 + i % 4) << R"(,"tid":)" << (1 + i % 7)
                << R"(,"ts":)" << (1000 + i * 100) << R"(,"dur":)" << (10 + i)
                << R"(,"args":{"fhash":"fh)" << (i % 5)
                << R"(","hhash":"hh1"}})"
                << "\n";
        }
    }
    dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz, member_bytes);
    dstd::fs::remove(pfw).discard();
    return gz;
}

// A FileBuild for the indexed file `logical_path` of the index at `index_path`.
dftracer::utils::index::FileBuild rebuild_file(const dstd::String& index_path,
                                               dstd::StringView logical_path) {
    auto index = dftracer::utils::index::Index::open(
        dftracer::utils::index::normalize_index_root(index_path));
    REQUIRE(index);
    auto build = dftracer::utils::index::rebuild(
        *index, dstd::StringView(logical_path.data(), logical_path.size()));
    REQUIRE(build);
    return std::move(*build);
}

bool file_bloom_contains(const dftracer::utils::index::Snapshot& snapshot,
                         const dftracer::utils::index::FileRef& file,
                         dstd::StringView dim, dstd::StringView value) {
    auto fb = dftu_utils_test::index_take(dftracer::utils::index::path_file(
        snapshot, file, dftracer::utils::index::store::IndexExtension::Bloom,
        dim));
    REQUIRE(fb.has_value());
    auto bloom = dftracer::utils::index::extensions::kinds::decode_bloom(
        dstd::StringView(fb->data(), fb->size()));
    REQUIRE(bloom.has_value());
    return bloom->possibly_contains(value);
}

dstd::String padded(dstd::String s) {
    s.resize(s.size() + simdjson::SIMDJSON_PADDING, '\0');
    return s;
}

dstd::Vector<dftracer::utils::index::IndexedRowSet> dftracer_rowsets() {
    return dftracer::utils::index::indexed_rowsets(
        dftracer::utils::index::get_schema("dftracer"));
}

// The path the stored `files` row set of `file` gives for `fhash`, or "".
dstd::String stored_path(const dftracer::utils::index::Snapshot& snapshot,
                         const dftracer::utils::index::FileRef& file,
                         dstd::StringView fhash) {
    const auto bytes = dftu_utils_test::index_take(
        dftracer::utils::index::rowset(snapshot, file, "files"));
    if (!bytes) return {};
    const auto f = dftracer::utils::dataframe::frame_from_native(
        dstd::StringView(bytes->data(), bytes->size()));
    if (!f) return {};
    const auto keys = f->column("fhash").materialize();
    const auto paths = f->column("path").materialize();
    for (std::int64_t r = 0; r < f->num_rows(); ++r)
        if (keys.string_at(r) == fhash) return dstd::String(paths.string_at(r));
    return {};
}

// Registers the fold's file, since a reader sees a file only through its
// Members copy, and starts the build the fold writes into. The build must
// exist before the fold spills: its commit publishes the record the tier
// hangs off.
dftracer::utils::index::FileBuild start_build(const dstd::String& root) {
    auto index = dftracer::utils::index::Index::open(
        dftracer::utils::index::normalize_index_root(root));
    REQUIRE(index);
    auto registered = dftu_utils_test::register_test_file(*index, "f.pfw.gz");
    dftu_utils_test::commit_test_build(registered);
    auto build = index->build("f.pfw.gz", {});
    REQUIRE(build);
    return std::move(*build);
}

dstd::Vector<FoldEvent> events_of(const dstd::Vector<dstd::String>& lines,
                                  dstd::StringIntern& intern) {
    dstd::Vector<FoldEvent> out;
    out.reserve(lines.size());
    simdjson::dom::parser parser;
    for (const auto& l : lines) {
        dstd::String buf = padded(l);
        auto doc = parser.parse(buf.data(), l.size(), false);
        REQUIRE_FALSE(doc.error());
        out.push_back(extract_fold_event(doc.value_unsafe(), intern, true));
    }
    return out;
}

}  // namespace

TEST_SUITE("BloomFold") {
    // A build that fails after spilling drops its fold, and the runs it staged
    // go with it: nothing of them is readable. Runs of a build that commits
    // come with the rest of the tier.
    TEST_CASE("spill runs live until their build commits or is dropped") {
        TestEnvironment env(10);
        dstd::StringIntern intern;
        dstd::Vector<dstd::String> lines;
        for (int i = 0; i < 8; ++i)
            lines.push_back(R"({"ph":"X","name":"read","cat":"POSIX","pid":1,)"
                            R"("tid":1,"ts":)" +
                            dstd::to_string(i) +
                            R"(,"dur":1,"args":{"k":"v"}})");
        auto events = events_of(lines, intern);
        constexpr std::uint64_t CHUNKS = 4 + BloomFold::SPILL_LAG;
        auto drive = [&](BloomFold& fold) {
            // A chunk spills once SPILL_LAG later checkpoints have started.
            for (std::uint64_t cp = 0; cp < CHUNKS; ++cp) {
                ScanUnit unit;
                unit.file_path = "f.pfw.gz";
                unit.index_path = env.dir() + "/.dftindex";
                unit.checkpoint_idx = cp;
                dftu_utils_test::ok(fold.step(
                    FoldBatch{dstd::Span<const FoldEvent>(events), unit}));
            }
        };
        const dstd::String dropped = env.dir() + "/dropped";
        {
            auto build = start_build(dropped);
            BloomFold fold(intern);
            fold.enable_spill(1, build);
            drive(fold);
            // The spills fill one run, far under the run size floor.
            CHECK(fold.spill_runs() == 1);
            dftu_utils_test::ok(fold.write_run().get());
            CHECK(fold.spill_runs() == 1);
        }
        {
            const auto snapshot =
                dftu_utils_test::index_snapshot(dropped + "/.dftindex");
            const auto file = snapshot.file("f.pfw.gz");
            REQUIRE(file.has_value());
            CHECK_FALSE(dftu_utils_test::pruning_tier_current(snapshot, *file));
            CHECK(dftu_utils_test::index_take(
                      dftracer::utils::index::chunk_stats(snapshot, *file))
                      .empty());
        }

        const dstd::String kept = env.dir() + "/kept";
        {
            auto build = start_build(kept);
            BloomFold fold(intern);
            fold.enable_spill(1, build);
            drive(fold);
            CHECK(fold.spill_runs() == 1);
            // The run goes into the build and is staged; the commit publishes
            // it with the chunks the fold still holds.
            dftu_utils_test::ok(fold.write_run().get());
            dftu_utils_test::ok(fold.write(build));
            REQUIRE(build.commit().get());
        }
        // The staged tier holds every chunk the fold saw, as the tier a fold
        // that fit in memory writes does.
        const dstd::String memory = env.dir() + "/memory";
        {
            auto build = start_build(memory);
            BloomFold fold(intern);
            drive(fold);
            CHECK(fold.spill_runs() == 0);
            dftu_utils_test::ok(fold.write(build));
            REQUIRE(build.commit().get());
        }
        auto chunks = [](const dstd::String& path) {
            const auto snapshot =
                dftu_utils_test::index_snapshot(path + "/.dftindex");
            const auto file = snapshot.file("f.pfw.gz");
            REQUIRE(file.has_value());
            return dftu_utils_test::index_take(
                dftracer::utils::index::chunk_stats(snapshot, *file));
        };
        const auto spilled = chunks(kept);
        const auto held = chunks(memory);
        REQUIRE(spilled.size() == CHUNKS);
        REQUIRE(held.size() == CHUNKS);
        for (std::size_t i = 0; i < spilled.size(); ++i) {
            CHECK(spilled[i].checkpoint_idx == held[i].checkpoint_idx);
            CHECK(spilled[i].stats.total_events == held[i].stats.total_events);
        }
    }

    // An unfiltered export rides the fold along, so the pruner index it could
    // not build before is now a byproduct: has_bloom flips and the written
    // filters contain every value the trace carried (no false negatives).
    TEST_CASE("builds a functional pruner index lazily") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_bloom_trace(env, 60, /*member_bytes=*/900);
        dstd::String index_path = dstd::String(determine_index_path(gz, ""));
        {
            StringSink s;
            dftu_utils_test::ok(
                View::from_file(dstd::String(gz), dstd::String(index_path))
                    .sink_json(s)
                    .get());
        }

        const auto snapshot = dftu_utils_test::index_snapshot(index_path);
        const auto file = dftu_utils_test::require_indexed_file(snapshot, gz);
        REQUIRE(dftu_utils_test::pruning_tier_current(snapshot, file));

        for (auto v : {"read", "write"})
            CHECK(file_bloom_contains(snapshot, file, "name", v));
        for (auto v : {"POSIX", "STDIO"})
            CHECK(file_bloom_contains(snapshot, file, "cat", v));
        for (auto v : {"1", "2", "3", "4"})
            CHECK(file_bloom_contains(snapshot, file, "pid", v));
        for (auto v : {"fh0", "fh1", "fh2", "fh3", "fh4"})
            CHECK(file_bloom_contains(snapshot, file, "fhash", v));
        CHECK(file_bloom_contains(snapshot, file, "hhash", "hh1"));

        auto name_chunks =
            dftu_utils_test::index_take(dftracer::utils::index::granules(
                snapshot, file,
                dftracer::utils::index::store::IndexExtension::Bloom, "name"));
        REQUIRE(name_chunks.size() > 1);
        for (const auto& [chunk, bytes] : name_chunks) {
            auto bloom =
                dftracer::utils::index::extensions::kinds::decode_bloom(
                    dstd::StringView(bytes.data(), bytes.size()));
            REQUIRE(bloom.has_value());
            CHECK(bloom->entry_count() <= 2);
        }
    }

    // Only a file the scan read whole may publish a pruner index, and an index
    // that already has one is authoritative.
    TEST_CASE("coverage gate and single write") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_bloom_trace(env, 20, /*member_bytes=*/2000);
        dstd::String index_path = dstd::String(determine_index_path(gz, ""));
        {
            // A filtered export builds the members but attaches no index fold,
            // so the index starts without a bloom for the fold to establish.
            StringSink s;
            dftu_utils_test::ok(
                View::from_file(dstd::String(gz), dstd::String(index_path))
                    .duql(R"(cat == "POSIX")")
                    .sink_json(s)
                    .get());
            const auto snapshot = dftu_utils_test::index_snapshot(index_path);
            REQUIRE_FALSE(dftu_utils_test::pruning_tier_current(
                snapshot, dftu_utils_test::require_indexed_file(snapshot, gz)));
        }

        ScanUnit unit;
        unit.file_path = gz;
        unit.index_path = index_path;
        unit.checkpoint_idx = 0;

        dstd::Vector<dstd::String> lines;
        for (int i = 0; i < 8; ++i)
            lines.push_back(R"({"ph":"X","name":"read","cat":"POSIX","pid":)" +
                            dstd::to_string(1 + i % 3) + R"(,"tid":1,"ts":)" +
                            dstd::to_string(i) +
                            R"(,"dur":1,"args":{"fhash":"fh1"}})");

        auto drive = [&](CoverageSet cov) {
            dstd::StringIntern intern;
            auto events = events_of(lines, intern);
            BloomFold fold(intern);
            FoldBatch batch{dstd::Span<const FoldEvent>(events), unit};
            dftu_utils_test::ok(fold.step(batch));
            fold.seal_unit(unit);
            return dftu_utils_test::ok(fold.finalize(cov).get());
        };

        CoverageSet member_only;
        member_only.add(gz, 0);
        CHECK_FALSE(drive(member_only));
        {
            const auto snapshot = dftu_utils_test::index_snapshot(index_path);
            CHECK_FALSE(dftu_utils_test::pruning_tier_current(
                snapshot, dftu_utils_test::require_indexed_file(snapshot, gz)));
        }

        CoverageSet whole_file;
        whole_file.add_file(gz);
        CHECK(drive(whole_file));
        {
            const auto snapshot = dftu_utils_test::index_snapshot(index_path);
            CHECK(dftu_utils_test::pruning_tier_current(
                snapshot, dftu_utils_test::require_indexed_file(snapshot, gz)));
        }

        // Already present: a second ride-along writes nothing.
        CHECK_FALSE(drive(whole_file));
    }

    // The streaming index build owns its write transaction, so the folds write
    // into a caller-provided sink (here an index write) rather than opening
    // their own index. Both artifacts must come back queryable.
    TEST_CASE("write writes queryable bloom and hash") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_bloom_trace(env, 20, /*member_bytes=*/2000);
        dstd::String index_path = dstd::String(determine_index_path(gz, ""));
        {
            StringSink s;
            dftu_utils_test::ok(
                View::from_file(dstd::String(gz), dstd::String(index_path))
                    .duql(R"(cat == "POSIX")")
                    .sink_json(s)
                    .get());
        }

        ScanUnit unit;
        unit.file_path = gz;
        unit.index_path = index_path;
        unit.checkpoint_idx = 0;

        dstd::Vector<dstd::String> lines = {
            R"({"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"/data/a.bin","value":"fh1"}})",
            R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1,"dur":1,"args":{"fhash":"fh1"}})",
            R"({"ph":"X","name":"write","cat":"STDIO","pid":2,"tid":3,"ts":2,"dur":1,"args":{"fhash":"fh1"}})",
        };
        dstd::StringIntern intern;
        auto events = events_of(lines, intern);
        BloomFold bloom(intern);
        RowSetFold dict(intern, dftracer_rowsets());
        FoldBatch fb{dstd::Span<const FoldEvent>(events), unit};
        dftu_utils_test::ok(bloom.step(fb));
        dftu_utils_test::ok(dict.step(fb));
        bloom.seal_unit(unit);
        dict.seal_unit(unit);

        {
            const auto logical =
                dftracer::utils::index::store::internal::get_logical_path(gz);
            auto build = rebuild_file(index_path, logical);
            dftu_utils_test::ok(bloom.write(build));
            dftu_utils_test::ok(dict.write(build));
            REQUIRE(build.commit().get());
        }

        const auto rd = dftu_utils_test::index_snapshot(index_path);
        const auto rfile = dftu_utils_test::require_indexed_file(rd, gz);
        CHECK(file_bloom_contains(rd, rfile, "name", "read"));
        CHECK(file_bloom_contains(rd, rfile, "name", "write"));
        CHECK(file_bloom_contains(rd, rfile, "fhash", "fh1"));
        CHECK(stored_path(rd, rfile, "fh1") == "/data/a.bin");
    }

    // IndexFoldDriver parses member plaintext into the folds. Feeding it split
    // across two on_chunk calls (mid-line) exercises the cross-chunk reassembly
    // it inherits from parse_buffer: no event may be lost at the seam.
    TEST_CASE("IndexFoldDriver harvests folds across a chunk boundary") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_bloom_trace(env, 20, /*member_bytes=*/2000);
        dstd::String index_path = dstd::String(determine_index_path(gz, ""));
        {
            StringSink s;
            dftu_utils_test::ok(
                View::from_file(dstd::String(gz), dstd::String(index_path))
                    .duql(R"(cat == "POSIX")")
                    .sink_json(s)
                    .get());
        }

        dstd::String text =
            dstd::String(
                R"({"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"/data/a.bin","value":"fh1"}})") +
            "\n" +
            R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1,"dur":1,"args":{"fhash":"fh1"}})" +
            "\n" +
            R"({"ph":"X","name":"write","cat":"STDIO","pid":2,"tid":3,"ts":2,"dur":1,"args":{"fhash":"fh1"}})" +
            "\n";

        dstd::StringIntern intern;
        BloomFold bloom(intern);
        RowSetFold dict(intern, dftracer_rowsets());
        std::array<Fold*, 2> fp{&bloom, &dict};
        IndexFoldDriver drv(intern, fp, dstd::String(dstd::StringView(gz)),
                            dstd::String(dstd::StringView(index_path)));
        // Split mid-way (lands inside the "read" line) to force reassembly.
        std::size_t half = text.size() / 2;
        dftu_utils_test::ok(drv.on_chunk(text.data(), half, /*cp=*/0).get());
        dftu_utils_test::ok(
            drv.on_chunk(text.data() + half, text.size() - half, /*cp=*/0)
                .get());
        drv.seal();

        {
            const auto logical =
                dftracer::utils::index::store::internal::get_logical_path(gz);
            auto build = rebuild_file(index_path, logical);
            dftu_utils_test::ok(bloom.write(build));
            dftu_utils_test::ok(dict.write(build));
            REQUIRE(build.commit().get());
        }

        const auto rd = dftu_utils_test::index_snapshot(index_path);
        const auto rfile = dftu_utils_test::require_indexed_file(rd, gz);
        // All three events survived the seam: both names, both cats, the fhash,
        // and the FH row of the files row set.
        CHECK(file_bloom_contains(rd, rfile, "name", "read"));
        CHECK(file_bloom_contains(rd, rfile, "name", "write"));
        CHECK(file_bloom_contains(rd, rfile, "cat", "STDIO"));
        CHECK(file_bloom_contains(rd, rfile, "fhash", "fh1"));
        CHECK(stored_path(rd, rfile, "fh1") == "/data/a.bin");
    }

    // Schemaless column harvest: nested-object and array args surface as dotted
    // leaf columns with their types, and View::columns()/column_info() read
    // them back from the index with no trace scan.
    TEST_CASE("schemaless columns and schema over nested args") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String pfw = env.dir() + "/schema.pfw";
        {
            dftu_utils_test::FileOut ofs(pfw);
            // hostname/size/rate are flat; pos is a nested object; tags is an
            // array; fhash/hhash are lifted hashes. Schema is harvested once
            // per event name, so a second "write" event carries size as a float
            // to exercise the cross-name type fold (int64 + float64 ->
            // float64).
            ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":2,"ts":100,"dur":5,)"
                << R"("args":{"hostname":"h1","size":1024,"rate":3.5,)"
                << R"("pos":{"x":1,"y":2},"tags":["a","b"],)"
                << R"("fhash":"fh1","hhash":"hh1"}})"
                << "\n";
            ofs << R"({"ph":"X","name":"write","cat":"POSIX","pid":1,"tid":2,"ts":200,"dur":6,)"
                << R"("args":{"size":2.5}})" << "\n";
        }
        dstd::String gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        dstd::fs::remove(pfw).discard();
        dstd::String index_path = dstd::String(determine_index_path(gz, ""));
        {
            StringSink s;
            dftu_utils_test::ok(
                View::from_file(dstd::String(gz), dstd::String(index_path))
                    .sink_json(s)
                    .get());
        }

        View v = View::from_file(dstd::String(gz), dstd::String(index_path));
        auto cols = v.columns();
        auto has = [&](const dstd::String& c) {
            return std::find(cols.begin(), cols.end(), dstd::String(c)) !=
                   cols.end();
        };
        // Base axis fields.
        CHECK(has("pid"));
        CHECK(has("tid"));
        CHECK(has("ts"));
        CHECK(has("dur"));
        // Top-level + flat args.
        CHECK(has("name"));
        CHECK(has("cat"));
        CHECK(has("hostname"));
        CHECK(has("size"));
        CHECK(has("rate"));
        // Nested object -> dotted leaves; array -> one leaf per element.
        CHECK(has("pos.x"));
        CHECK(has("pos.y"));
        CHECK(has("tags.0"));
        CHECK(has("tags.1"));
        // Lifted hashes; their names are arrows into the row sets.
        CHECK(has("fhash"));
        CHECK(has("hhash"));
        CHECK_FALSE(has("resolved.fhash.path"));

        dstd::HashMap<dstd::String, dstd::String> ty;
        for (const auto& ci : v.column_info())
            ty[dstd::String(ci.name)] = dstd::String(ci.type);
        CHECK(ty["pid"] == "int64");
        CHECK(ty["ts"] == "int64");
        CHECK(ty["hostname"] == "string");
        CHECK(ty["rate"] == "float64");
        CHECK(ty["pos.x"] == "int64");
        CHECK(ty["tags.0"] == "string");
        // size is int (1024) in one event and float (2.5) in another; the fold
        // widens it to float64.
        CHECK(ty["size"] == "float64");
    }
}

TEST_SUITE("BloomCore") {
    TEST_CASE("merging a chunk's states keeps numeric min and max") {
        index::schemas::dft::BloomCore::ChunkIndexerConfig config;
        index::schemas::dft::BloomCore::ChunkState a, b;
        index::schemas::dft::BloomCore::init_chunk_state(a, config, {});
        index::schemas::dft::BloomCore::init_chunk_state(b, config, {});
        index::schemas::dft::BloomCore::EventCache cache_a;
        index::schemas::dft::BloomCore::EventCache cache_b;
        index::schemas::dft::BloomCore::observe_data(
            a, cache_a, 0, 1, "read", "POSIX", 9, 1, 9, 9, true, "", "", "");
        index::schemas::dft::BloomCore::observe_data(
            b, cache_b, 0, 1, "read", "POSIX", 10, 1, 10, 10, true, "", "", "");
        index::schemas::dft::BloomCore::merge_chunk_state(a, b);
        const auto& ts =
            a.fixed_dim_stats[index::schemas::dft::BloomCore::FdTs];
        CHECK(ts.min_value == "9");
        CHECK(ts.max_value == "10");
    }

    TEST_CASE("an extra dimension keeps min and max in its own type") {
        index::schemas::dft::BloomCore::ChunkIndexerConfig config;
        index::schemas::dft::BloomCore::ChunkState c;
        index::schemas::dft::BloomCore::init_chunk_state(
            c, config, {"size", "mode", "mix"});
        index::schemas::dft::BloomCore::observe_extra(c, 0, std::int64_t{9});
        index::schemas::dft::BloomCore::observe_extra(c, 0, std::int64_t{10});
        index::schemas::dft::BloomCore::observe_extra(c, 0, 2.5);
        index::schemas::dft::BloomCore::observe_extra(c, 1,
                                                      dstd::StringView("rw"));
        index::schemas::dft::BloomCore::observe_extra(c, 1,
                                                      dstd::StringView("r"));
        index::schemas::dft::BloomCore::observe_extra(c, 2, std::int64_t{1});
        index::schemas::dft::BloomCore::observe_extra(c, 2,
                                                      dstd::StringView("x"));

        const auto& size = c.extra_dim_stats[0];
        CHECK(size.value_type == "double");
        CHECK(size.min_value == "2.500000");
        CHECK(size.max_value == "10");
        CHECK(c.extra_blooms[0].possibly_contains("10"));
        CHECK(c.extra_blooms[0].possibly_contains("2.500000"));

        const auto& mode = c.extra_dim_stats[1];
        CHECK(mode.value_type == "string");
        CHECK(mode.min_value == "r");
        CHECK(mode.max_value == "rw");

        const auto& mix = c.extra_dim_stats[2];
        CHECK(mix.value_type == "mixed");
        CHECK(mix.min_value.empty());
        CHECK(mix.max_value.empty());
    }

    TEST_CASE("merging an extra dimension joins its types") {
        index::schemas::dft::BloomCore::ChunkIndexerConfig config;
        index::schemas::dft::BloomCore::ChunkState a, b, c;
        for (auto* s : {&a, &b, &c})
            index::schemas::dft::BloomCore::init_chunk_state(*s, config, {"n"});
        index::schemas::dft::BloomCore::observe_extra(a, 0, std::int64_t{9});
        index::schemas::dft::BloomCore::observe_extra(b, 0, std::int64_t{10});
        index::schemas::dft::BloomCore::merge_chunk_state(a, b);
        CHECK(a.extra_dim_stats[0].min_value == "9");
        CHECK(a.extra_dim_stats[0].max_value == "10");
        index::schemas::dft::BloomCore::observe_extra(c, 0,
                                                      dstd::StringView("x"));
        index::schemas::dft::BloomCore::merge_chunk_state(a, c);
        CHECK(a.extra_dim_stats[0].value_type == "mixed");
        CHECK(a.extra_dim_stats[0].min_value.empty());
    }
}

TEST_SUITE("BloomFold - auto fields") {
    // Four members; member m holds size m*100+{0,1,2}, mode "m<m>", and
    // fname unique per event.
    static dstd::String create_auto_trace(TestEnvironment & env) {
        dstd::String pfw = env.dir() + "/auto.pfw";
        {
            dftu_utils_test::FileOut ofs(pfw);
            for (int m = 0; m < 4; ++m)
                for (int i = 0; i < 60; ++i)
                    ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
                        << m * 1000 + i << R"(,"dur":5,"args":{"size":)"
                        << m * 100 + i % 3 << R"(,"mode":"m)" << m
                        << R"(","fname":"/f/)" << m << "/" << i << R"("}})"
                        << "\n";
        }
        dstd::String gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz, 4000);
        dstd::fs::remove(pfw).discard();
        return gz;
    }

    TEST_CASE("auto fields keep numeric min/max and capped string blooms") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_auto_trace(env);
        dftracer::utils::index::build::ResolveAndBuildInput input;
        input.files = {dstd::String(dstd::StringView(gz))};
        input.require_bloom = true;
        input.bloom_config.extra_dimensions.clear();
        input.bloom_config.path_budget = 1024;
        input.bloom_config.auto_max_distinct = 8;
        dstd::String index_path;
        dftu_utils_test::run_coro([&](dftracer::utils::coro::Scope&)
                                      -> dftracer::utils::coro::CoroTask<void> {
            auto res =
                co_await dftracer::utils::index::build::resolve_and_build_index(
                    input);
            if (res) index_path = res->index_path;
        });
        const auto snapshot = dftu_utils_test::index_snapshot(index_path);
        const auto file = dftu_utils_test::require_indexed_file(snapshot, gz);
        auto dims = dftu_utils_test::index_take(dftracer::utils::index::paths(
            snapshot, file, index::store::IndexExtension::Zonemap));
        for (auto d : {"size", "mode", "fname"})
            CHECK(std::find(dims.begin(), dims.end(), d) != dims.end());

        auto size =
            dftu_utils_test::index_take(dftracer::utils::index::granules(
                snapshot, file, index::store::IndexExtension::Zonemap, "size"));
        REQUIRE(size.size() >= 2);
        for (const auto& [chunk, bytes] : size) {
            auto zone = dftracer::utils::index::extensions::kinds::decode_zone(
                dstd::StringView(bytes.data(), bytes.size()));
            REQUIRE(zone.has_value());
            CHECK(zone->value_type == "int");
            CHECK_FALSE(zone->min.empty());
        }
        CHECK(dftu_utils_test::bloom_chunks(snapshot, file, "size") == 0);
        CHECK_FALSE(dftu_utils_test::has_file_bloom(snapshot, file, "size"));

        CHECK(dftu_utils_test::bloom_chunks(snapshot, file, "mode") ==
              size.size());
        CHECK(dftu_utils_test::has_file_bloom(snapshot, file, "mode"));
        CHECK(dftu_utils_test::bloom_chunks(snapshot, file, "fname") <
              size.size());
        CHECK_FALSE(dftu_utils_test::has_file_bloom(snapshot, file, "fname"));
    }
}

TEST_CASE("numeric bounds keep the min and max text of the text path") {
    // Zeros of either sign print differently but compare equal.
    for (const auto& values : dstd::Vector<dstd::Vector<double>>{
             {0.0, -1e-7}, {1e-7, -1e-7}, {-1e-7, 1e-7}, {-1e-7, 0.0, 2e-7}}) {
        ChunkDimensionStats text_path;
        ChunkDimensionStats typed;
        text_path.value_type.clear();
        typed.value_type.clear();
        index::schemas::dft::BloomCore::NumericBounds bounds;
        for (double v : values) {
            index::schemas::dft::BloomCore::observe_value(text_path, v);
            index::schemas::dft::BloomCore::observe_value(typed, bounds, v);
        }
        CHECK(typed.min_value == text_path.min_value);
        CHECK(typed.max_value == text_path.max_value);
    }
    std::mt19937_64 rng(7);
    std::uniform_real_distribution<double> real(-1e3, 1e3);
    for (int round = 0; round < 200; ++round) {
        CAPTURE(round);
        ChunkDimensionStats text_path;
        ChunkDimensionStats typed;
        text_path.value_type.clear();
        typed.value_type.clear();
        index::schemas::dft::BloomCore::NumericBounds bounds;
        for (int i = 0; i < 300; ++i) {
            // Doubles that print to the same 6 decimals, integers, a
            // double between two integers, and the odd non-finite value.
            const int kind = static_cast<int>(rng() % 8);
            if (kind < 3) {
                const double base = std::floor(real(rng) * 1e3) / 1e3;
                const double v = base + static_cast<double>(rng() % 5) * 1e-7;
                index::schemas::dft::BloomCore::observe_value(text_path, v);
                index::schemas::dft::BloomCore::observe_value(typed, bounds, v);
            } else if (kind < 5) {
                const auto v = static_cast<std::int64_t>(rng() % 2001) - 1000;
                index::schemas::dft::BloomCore::observe_value(text_path, v);
                index::schemas::dft::BloomCore::observe_value(typed, bounds, v);
            } else if (kind < 6) {
                const auto v = static_cast<std::uint64_t>(rng() % 1000);
                index::schemas::dft::BloomCore::observe_value(text_path, v);
                index::schemas::dft::BloomCore::observe_value(typed, bounds, v);
            } else if (kind < 7) {
                const double v = static_cast<double>(rng() % 2001) - 1000.5;
                index::schemas::dft::BloomCore::observe_value(text_path, v);
                index::schemas::dft::BloomCore::observe_value(typed, bounds, v);
            } else if (round % 10 == 0) {
                const double v = rng() % 2
                                     ? std::numeric_limits<double>::infinity()
                                     : std::nan("");
                index::schemas::dft::BloomCore::observe_value(text_path, v);
                index::schemas::dft::BloomCore::observe_value(typed, bounds, v);
            }
            REQUIRE(typed.value_type == text_path.value_type);
            REQUIRE(typed.present == text_path.present);
            REQUIRE(typed.min_value == text_path.min_value);
            REQUIRE(typed.max_value == text_path.max_value);
        }
    }
}

TEST_SUITE("BloomFold - misuse") {
    TEST_CASE("a fold over an unvalidated config aborts") {
        auto result = fresh_process::run(
            "a fold over an unvalidated config ends the process");
        CHECK(result.aborted_with("the config was not validated"));
    }

    TEST_CASE("a fold over an unvalidated config ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        dstd::StringIntern intern;
        ChunkIndexerConfig config;
        config.stats_share = 0.0;
        BloomFold fold(intern, config);
    }
}
