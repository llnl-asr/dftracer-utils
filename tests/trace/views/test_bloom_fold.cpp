#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/internal/ipc.h>
#include <dftracer/utils/index/build/index_fold_driver.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/extensions/rowset_fold.h>
#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/source.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <simdjson.h>
#include <testing_runtime.h>

#include <algorithm>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "test_view_common.h"

using namespace dftracer::utils::trace::views::detail;
using namespace dftracer::utils::index::build;
using namespace dftracer::utils::index::extensions;
using dftracer::utils::StringIntern;
using dftracer::utils::index::extensions::ScalableBloomFilter;

namespace {

std::string create_bloom_trace(TestEnvironment& env, int n,
                               std::size_t member_bytes) {
    std::string pfw = env.get_dir() + "/bloom.pfw";
    {
        std::ofstream ofs(pfw);
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
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz, member_bytes);
    fs::remove(pfw);
    return gz;
}

int file_id_of(dftracer::utils::index::store::IndexDatabase& db,
               const std::string& gz) {
    return db.get_file_info_id(
        dftracer::utils::index::store::internal::get_logical_path(gz));
}

bool file_bloom_contains(dftracer::utils::index::store::IndexDatabase& db,
                         int fid, std::string_view dim,
                         std::string_view value) {
    auto fb = db.path_file_value(
        fid, dftracer::utils::index::store::IndexExtension::BLOOM, dim);
    REQUIRE(fb.has_value());
    auto bloom = dftracer::utils::index::extensions::kinds::decode_bloom(*fb);
    REQUIRE(bloom.has_value());
    return bloom->possibly_contains(value);
}

std::string padded(std::string s) {
    s.resize(s.size() + simdjson::SIMDJSON_PADDING, '\0');
    return s;
}

std::vector<dftracer::utils::index::IndexedRowSet> dftracer_rowsets() {
    return dftracer::utils::index::indexed_rowsets(
        dftracer::utils::index::get_schema("dftracer"));
}

// The path the stored `files` row set of `fid` gives for `fhash`, or "".
std::string stored_path(const dftracer::utils::index::store::IndexDatabase& db,
                        int fid, std::string_view fhash) {
    const auto bytes = db.rowset(fid, "files");
    if (!bytes) return {};
    const auto f = dftracer::utils::dataframe::frame_from_ipc(*bytes);
    if (!f) return {};
    const auto keys = f->column("fhash").materialize();
    const auto paths = f->column("path").materialize();
    for (std::int64_t r = 0; r < f->num_rows(); ++r)
        if (keys.string_at(r) == fhash) return std::string(paths.string_at(r));
    return {};
}

std::vector<FoldEvent> events_of(const std::vector<std::string>& lines,
                                 StringIntern& intern) {
    std::vector<FoldEvent> out;
    out.reserve(lines.size());
    simdjson::dom::parser parser;
    for (const auto& l : lines) {
        std::string buf = padded(l);
        auto doc = parser.parse(buf.data(), l.size(), false);
        REQUIRE_FALSE(doc.error());
        out.push_back(extract_fold_event(doc.value_unsafe(), intern, true));
    }
    return out;
}

}  // namespace

TEST_SUITE("BloomFold") {
    // A build that fails after spilling drops its fold; the runs go with it.
    // Runs handed off by finish_spilled belong to the caller.
    TEST_CASE("spill runs live until handed off or dropped") {
        TestEnvironment env(10);
        StringIntern intern;
        std::vector<std::string> lines;
        for (int i = 0; i < 8; ++i)
            lines.push_back(R"({"ph":"X","name":"read","cat":"POSIX","pid":1,)"
                            R"("tid":1,"ts":)" +
                            std::to_string(i) +
                            R"(,"dur":1,"args":{"k":"v"}})");
        auto events = events_of(lines, intern);
        auto drive = [&](BloomFold& fold) {
            for (std::uint64_t cp = 0; cp < 4; ++cp) {
                ScanUnit unit;
                unit.file_path = "f.pfw.gz";
                unit.index_path = env.get_dir() + "/.dftindex";
                unit.checkpoint_idx = cp;
                fold.step(FoldBatch{std::span<const FoldEvent>(events), unit});
            }
        };
        const std::string dropped = env.get_dir() + "/dropped";
        {
            BloomFold fold(intern);
            fold.enable_spill(1, dropped, 1);
            drive(fold);
            CHECK(fold.spill_runs() == 3);
            CHECK(fs::exists(dropped));
        }
        CHECK_FALSE(fs::exists(dropped));

        const std::string kept = env.get_dir() + "/kept";
        std::size_t runs = 0;
        {
            BloomFold fold(intern);
            fold.enable_spill(1, kept, 1);
            drive(fold);
            runs = fold.finish_spilled(1).size();
        }
        CHECK(runs == 4);
        CHECK(fs::exists(kept));
    }

    // An unfiltered export rides the fold along, so the pruner index it could
    // not build before is now a byproduct: has_bloom flips and the written
    // filters contain every value the trace carried (no false negatives).
    TEST_CASE("builds a functional pruner index lazily") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_bloom_trace(env, 60, /*member_bytes=*/900);
        std::string index_path = determine_index_path(gz, "");
        {
            StringSink s;
            View::from_file(gz, index_path).sink_json(s).get();
        }

        dftracer::utils::index::store::IndexDatabase db(
            index_path, dftracer::utils::index::store::IndexOpenMode::ReadOnly);
        int fid = file_id_of(db, gz);
        REQUIRE(db.pruning_tier_current(fid));

        for (auto v : {"read", "write"})
            CHECK(file_bloom_contains(db, fid, "name", v));
        for (auto v : {"POSIX", "STDIO"})
            CHECK(file_bloom_contains(db, fid, "cat", v));
        for (auto v : {"1", "2", "3", "4"})
            CHECK(file_bloom_contains(db, fid, "pid", v));
        for (auto v : {"fh0", "fh1", "fh2", "fh3", "fh4"})
            CHECK(file_bloom_contains(db, fid, "fhash", v));
        CHECK(file_bloom_contains(db, fid, "hhash", "hh1"));

        auto name_chunks = db.path_granules(
            fid, dftracer::utils::index::store::IndexExtension::BLOOM, "name");
        REQUIRE(name_chunks.size() > 1);
        for (const auto& [chunk, bytes] : name_chunks) {
            auto bloom =
                dftracer::utils::index::extensions::kinds::decode_bloom(bytes);
            REQUIRE(bloom.has_value());
            CHECK(bloom->num_entries() <= 2);
        }
    }

    // Only a file the scan read whole may publish a pruner index, and an index
    // that already has one is authoritative.
    TEST_CASE("coverage gate and single write") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_bloom_trace(env, 20, /*member_bytes=*/2000);
        std::string index_path = determine_index_path(gz, "");
        {
            // A filtered export builds the members but attaches no index fold,
            // so the index starts without a bloom for the fold to establish.
            StringSink s;
            View::from_file(gz, index_path)
                .duql(R"(cat == "POSIX")")
                .sink_json(s)
                .get();
            dftracer::utils::index::store::IndexDatabase db(
                index_path,
                dftracer::utils::index::store::IndexOpenMode::ReadOnly);
            REQUIRE_FALSE(db.pruning_tier_current(file_id_of(db, gz)));
        }

        ScanUnit unit;
        unit.file_path = gz;
        unit.index_path = index_path;
        unit.checkpoint_idx = 0;

        std::vector<std::string> lines;
        for (int i = 0; i < 8; ++i)
            lines.push_back(R"({"ph":"X","name":"read","cat":"POSIX","pid":)" +
                            std::to_string(1 + i % 3) + R"(,"tid":1,"ts":)" +
                            std::to_string(i) +
                            R"(,"dur":1,"args":{"fhash":"fh1"}})");

        auto drive = [&](CoverageSet cov) {
            StringIntern intern;
            auto events = events_of(lines, intern);
            BloomFold fold(intern);
            FoldBatch batch{std::span<const FoldEvent>(events), unit};
            fold.step(batch);
            fold.seal_unit(unit);
            return fold.finalize(cov).get();
        };

        CoverageSet member_only;
        member_only.add(gz, 0);
        CHECK_FALSE(drive(member_only));
        {
            dftracer::utils::index::store::IndexDatabase db(
                index_path,
                dftracer::utils::index::store::IndexOpenMode::ReadOnly);
            CHECK_FALSE(db.pruning_tier_current(file_id_of(db, gz)));
        }

        CoverageSet whole_file;
        whole_file.add_file(gz);
        CHECK(drive(whole_file));
        {
            dftracer::utils::index::store::IndexDatabase db(
                index_path,
                dftracer::utils::index::store::IndexOpenMode::ReadOnly);
            CHECK(db.pruning_tier_current(file_id_of(db, gz)));
        }

        // Already present: a second ride-along writes nothing.
        CHECK_FALSE(drive(whole_file));
    }

    // The streaming index build owns its write transaction, so the folds write
    // into a caller-provided sink (here a RocksDB writer) rather than opening
    // their own index. Both artifacts must come back queryable.
    TEST_CASE("write writes queryable bloom and hash") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_bloom_trace(env, 20, /*member_bytes=*/2000);
        std::string index_path = determine_index_path(gz, "");
        {
            StringSink s;
            View::from_file(gz, index_path)
                .duql(R"(cat == "POSIX")")
                .sink_json(s)
                .get();
        }

        ScanUnit unit;
        unit.file_path = gz;
        unit.index_path = index_path;
        unit.checkpoint_idx = 0;

        std::vector<std::string> lines = {
            R"({"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"/data/a.bin","value":"fh1"}})",
            R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1,"dur":1,"args":{"fhash":"fh1"}})",
            R"({"ph":"X","name":"write","cat":"STDIO","pid":2,"tid":3,"ts":2,"dur":1,"args":{"fhash":"fh1"}})",
        };
        StringIntern intern;
        auto events = events_of(lines, intern);
        BloomFold bloom(intern);
        RowSetFold dict(intern, dftracer_rowsets());
        FoldBatch fb{std::span<const FoldEvent>(events), unit};
        bloom.step(fb);
        dict.step(fb);
        bloom.seal_unit(unit);
        dict.seal_unit(unit);

        int fid = -1;
        {
            dftracer::utils::index::store::IndexDatabase db(index_path);
            fid = db.get_file_info_id(
                dftracer::utils::index::store::internal::get_logical_path(gz));
            REQUIRE(fid >= 0);
            auto w = db.begin_write();
            bloom.write(*w, fid);
            dict.write(*w, fid);
            w->commit();
        }

        dftracer::utils::index::store::IndexDatabase rd(
            index_path, dftracer::utils::index::store::IndexOpenMode::ReadOnly);
        CHECK(file_bloom_contains(rd, fid, "name", "read"));
        CHECK(file_bloom_contains(rd, fid, "name", "write"));
        CHECK(file_bloom_contains(rd, fid, "fhash", "fh1"));
        CHECK(stored_path(rd, fid, "fh1") == "/data/a.bin");
    }

    // IndexFoldDriver parses member plaintext into the folds. Feeding it split
    // across two on_chunk calls (mid-line) exercises the cross-chunk reassembly
    // it inherits from parse_buffer: no event may be lost at the seam.
    TEST_CASE("IndexFoldDriver harvests folds across a chunk boundary") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_bloom_trace(env, 20, /*member_bytes=*/2000);
        std::string index_path = determine_index_path(gz, "");
        {
            StringSink s;
            View::from_file(gz, index_path)
                .duql(R"(cat == "POSIX")")
                .sink_json(s)
                .get();
        }

        std::string text =
            std::string(
                R"({"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"/data/a.bin","value":"fh1"}})") +
            "\n" +
            R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1,"dur":1,"args":{"fhash":"fh1"}})" +
            "\n" +
            R"({"ph":"X","name":"write","cat":"STDIO","pid":2,"tid":3,"ts":2,"dur":1,"args":{"fhash":"fh1"}})" +
            "\n";

        StringIntern intern;
        BloomFold bloom(intern);
        RowSetFold dict(intern, dftracer_rowsets());
        std::array<Fold*, 2> fp{&bloom, &dict};
        IndexFoldDriver drv(intern, fp, gz, index_path);
        // Split mid-way (lands inside the "read" line) to force reassembly.
        std::size_t half = text.size() / 2;
        drv.on_chunk(text.data(), half, /*cp=*/0).get();
        drv.on_chunk(text.data() + half, text.size() - half, /*cp=*/0).get();
        drv.seal();

        int fid = -1;
        {
            dftracer::utils::index::store::IndexDatabase db(index_path);
            fid = db.get_file_info_id(
                dftracer::utils::index::store::internal::get_logical_path(gz));
            REQUIRE(fid >= 0);
            auto w = db.begin_write();
            bloom.write(*w, fid);
            dict.write(*w, fid);
            w->commit();
        }

        dftracer::utils::index::store::IndexDatabase rd(
            index_path, dftracer::utils::index::store::IndexOpenMode::ReadOnly);
        // All three events survived the seam: both names, both cats, the fhash,
        // and the FH row of the files row set.
        CHECK(file_bloom_contains(rd, fid, "name", "read"));
        CHECK(file_bloom_contains(rd, fid, "name", "write"));
        CHECK(file_bloom_contains(rd, fid, "cat", "STDIO"));
        CHECK(file_bloom_contains(rd, fid, "fhash", "fh1"));
        CHECK(stored_path(rd, fid, "fh1") == "/data/a.bin");
    }

    // Schemaless column harvest: nested-object and array args surface as dotted
    // leaf columns with their types, and View::columns()/column_info() read
    // them back from the index with no trace scan.
    TEST_CASE("schemaless columns and schema over nested args") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string pfw = env.get_dir() + "/schema.pfw";
        {
            std::ofstream ofs(pfw);
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
        std::string gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        fs::remove(pfw);
        std::string index_path = determine_index_path(gz, "");
        {
            StringSink s;
            View::from_file(gz, index_path).sink_json(s).get();
        }

        View v = View::from_file(gz, index_path);
        auto cols = v.columns();
        auto has = [&](const std::string& c) {
            return std::find(cols.begin(), cols.end(), c) != cols.end();
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

        std::unordered_map<std::string, std::string> ty;
        for (const auto& ci : v.column_info()) ty[ci.name] = ci.type;
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
    using dftracer::utils::index::schemas::dft::BloomCore;

    TEST_CASE("merging a chunk's states keeps numeric min and max") {
        BloomCore::ChunkIndexerConfig config;
        BloomCore::ChunkState a, b;
        BloomCore::init_chunk_state(a, config, {});
        BloomCore::init_chunk_state(b, config, {});
        BloomCore::PidTidCache cache;
        BloomCore::observe_data(a, cache, "read", "POSIX", 9, 1, 9, 9, true, "",
                                "", "");
        BloomCore::observe_data(b, cache, "read", "POSIX", 10, 1, 10, 10, true,
                                "", "", "");
        BloomCore::merge_chunk_state(a, b);
        const auto& ts = a.fixed_dim_stats[BloomCore::FD_TS];
        CHECK(ts.min_value == "9");
        CHECK(ts.max_value == "10");
    }

    TEST_CASE("an extra dimension keeps min and max in its own type") {
        BloomCore::ChunkIndexerConfig config;
        BloomCore::ChunkState c;
        BloomCore::init_chunk_state(c, config, {"size", "mode", "mix"});
        BloomCore::observe_extra(c, 0, std::int64_t{9});
        BloomCore::observe_extra(c, 0, std::int64_t{10});
        BloomCore::observe_extra(c, 0, 2.5);
        BloomCore::observe_extra(c, 1, std::string_view("rw"));
        BloomCore::observe_extra(c, 1, std::string_view("r"));
        BloomCore::observe_extra(c, 2, std::int64_t{1});
        BloomCore::observe_extra(c, 2, std::string_view("x"));

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
        BloomCore::ChunkIndexerConfig config;
        BloomCore::ChunkState a, b, c;
        for (auto* s : {&a, &b, &c})
            BloomCore::init_chunk_state(*s, config, {"n"});
        BloomCore::observe_extra(a, 0, std::int64_t{9});
        BloomCore::observe_extra(b, 0, std::int64_t{10});
        BloomCore::merge_chunk_state(a, b);
        CHECK(a.extra_dim_stats[0].min_value == "9");
        CHECK(a.extra_dim_stats[0].max_value == "10");
        BloomCore::observe_extra(c, 0, std::string_view("x"));
        BloomCore::merge_chunk_state(a, c);
        CHECK(a.extra_dim_stats[0].value_type == "mixed");
        CHECK(a.extra_dim_stats[0].min_value.empty());
    }
}

TEST_SUITE("BloomFold - auto fields") {
    // Four members; member m holds size m*100+{0,1,2}, mode "m<m>", and
    // fname unique per event.
    static std::string create_auto_trace(TestEnvironment & env) {
        std::string pfw = env.get_dir() + "/auto.pfw";
        {
            std::ofstream ofs(pfw);
            for (int m = 0; m < 4; ++m)
                for (int i = 0; i < 60; ++i)
                    ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
                        << m * 1000 + i << R"(,"dur":5,"args":{"size":)"
                        << m * 100 + i % 3 << R"(,"mode":"m)" << m
                        << R"(","fname":"/f/)" << m << "/" << i << R"("}})"
                        << "\n";
        }
        std::string gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz, 4000);
        fs::remove(pfw);
        return gz;
    }

    TEST_CASE("auto fields keep numeric min/max and capped string blooms") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_auto_trace(env);
        dftracer::utils::index::build::ResolveAndBuildInput input;
        input.files = {gz};
        input.require_bloom = true;
        input.bloom_config.extra_dimensions.clear();
        input.bloom_config.path_budget = 1024;
        input.bloom_config.auto_max_distinct = 8;
        std::string index_path;
        dftu_utils_test::run_coro([&](dftracer::utils::CoroScope& scope)
                                      -> dftracer::utils::coro::CoroTask<void> {
            auto res =
                co_await dftracer::utils::index::build::resolve_and_build_index(
                    &scope, input);
            index_path = res.index_path;
        });
        dftracer::utils::index::store::IndexDatabase db(
            index_path, dftracer::utils::index::store::IndexOpenMode::ReadOnly);
        const int fid = file_id_of(db, gz);
        using dftracer::utils::index::store::IndexExtension;
        auto dims = db.extension_paths(fid, IndexExtension::ZONEMAP);
        for (auto d : {"size", "mode", "fname"})
            CHECK(std::find(dims.begin(), dims.end(), d) != dims.end());

        auto size = db.path_granules(fid, IndexExtension::ZONEMAP, "size");
        REQUIRE(size.size() >= 2);
        for (const auto& [chunk, bytes] : size) {
            auto zone =
                dftracer::utils::index::extensions::kinds::decode_zone(bytes);
            REQUIRE(zone.has_value());
            CHECK(zone->value_type == "int");
            CHECK_FALSE(zone->min.empty());
        }
        CHECK(dftu_utils_test::bloom_chunks(db, fid, "size") == 0);
        CHECK_FALSE(dftu_utils_test::has_file_bloom(db, fid, "size"));

        CHECK(dftu_utils_test::bloom_chunks(db, fid, "mode") == size.size());
        CHECK(dftu_utils_test::has_file_bloom(db, fid, "mode"));
        CHECK(dftu_utils_test::bloom_chunks(db, fid, "fname") < size.size());
        CHECK_FALSE(dftu_utils_test::has_file_bloom(db, fid, "fname"));
    }
}
