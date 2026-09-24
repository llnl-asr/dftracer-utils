#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/trace/indexing/resolve_and_build.h>
#include <dftracer/utils/trace/indexing/scalable_bloom_filter.h>
#include <dftracer/utils/trace/views/bloom_fold.h>
#include <dftracer/utils/trace/views/dict_fold.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/index_fold_driver.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <doctest/doctest.h>
#include <simdjson.h>
#include <testing_runtime.h>

#include <algorithm>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "test_view_common.h"

using namespace dftracer::utils::trace::views::detail;
using dftracer::utils::StringIntern;
namespace idx = dftracer::utils::utilities::indexer;
using dftracer::utils::trace::indexing::ScalableBloomFilter;

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

int file_id_of(idx::IndexDatabase& db, const std::string& gz) {
    return db.get_file_info_id(idx::internal::get_logical_path(gz));
}

bool file_bloom_contains(idx::IndexDatabase& db, int fid, std::string_view dim,
                         std::string_view value) {
    auto fb = db.query_file_bloom_filter(fid, dim);
    REQUIRE(fb.has_value());
    auto bloom = ScalableBloomFilter::from_blob(fb->bloom_data.data(),
                                                fb->bloom_data.size());
    return bloom.possibly_contains(value);
}

std::string padded(std::string s) {
    s.resize(s.size() + simdjson::SIMDJSON_PADDING, '\0');
    return s;
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
            View::from_file(gz, index_path)
                .emit_all_metadata(true)
                .export_json(s)
                .get();
        }

        idx::IndexDatabase db(index_path, idx::IndexOpenMode::ReadOnly);
        int fid = file_id_of(db, gz);
        REQUIRE(db.has_bloom_data(fid));

        for (auto v : {"read", "write"})
            CHECK(file_bloom_contains(db, fid, "name", v));
        for (auto v : {"POSIX", "STDIO"})
            CHECK(file_bloom_contains(db, fid, "cat", v));
        for (auto v : {"1", "2", "3", "4"})
            CHECK(file_bloom_contains(db, fid, "pid", v));
        for (auto v : {"fh0", "fh1", "fh2", "fh3", "fh4"})
            CHECK(file_bloom_contains(db, fid, "fhash", v));
        CHECK(file_bloom_contains(db, fid, "hhash", "hh1"));

        auto name_chunks = db.query_chunk_bloom_filters(fid, "name");
        REQUIRE(name_chunks.size() > 1);
        for (const auto& c : name_chunks) CHECK(c.num_entries <= 2);
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
                .query(R"(cat == "POSIX")")
                .export_json(s)
                .get();
            idx::IndexDatabase db(index_path, idx::IndexOpenMode::ReadOnly);
            REQUIRE_FALSE(db.has_bloom_data(file_id_of(db, gz)));
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
            idx::IndexDatabase db(index_path, idx::IndexOpenMode::ReadOnly);
            CHECK_FALSE(db.has_bloom_data(file_id_of(db, gz)));
        }

        CoverageSet whole_file;
        whole_file.add_file(gz);
        CHECK(drive(whole_file));
        {
            idx::IndexDatabase db(index_path, idx::IndexOpenMode::ReadOnly);
            CHECK(db.has_bloom_data(file_id_of(db, gz)));
        }

        // Already present: a second ride-along writes nothing.
        CHECK_FALSE(drive(whole_file));
    }

    // The streaming index build owns its write transaction, so the folds write
    // into a caller-provided sink (here a RocksDB writer) rather than opening
    // their own index. Both artifacts must come back queryable.
    TEST_CASE("write_to_sink writes queryable bloom and hash") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_bloom_trace(env, 20, /*member_bytes=*/2000);
        std::string index_path = determine_index_path(gz, "");
        {
            StringSink s;
            View::from_file(gz, index_path)
                .query(R"(cat == "POSIX")")
                .export_json(s)
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
        DictFold dict(intern);
        FoldBatch fb{std::span<const FoldEvent>(events), unit};
        bloom.step(fb);
        dict.step(fb);
        bloom.seal_unit(unit);
        dict.seal_unit(unit);

        int fid = -1;
        {
            idx::IndexDatabase db(index_path);
            fid = db.get_file_info_id(idx::internal::get_logical_path(gz));
            REQUIRE(fid >= 0);
            auto w = db.begin_write();
            bloom.write_to_sink(*w, fid);
            dict.write_to_sink(*w);
            w->commit();
        }

        idx::IndexDatabase rd(index_path, idx::IndexOpenMode::ReadOnly);
        CHECK(file_bloom_contains(rd, fid, "name", "read"));
        CHECK(file_bloom_contains(rd, fid, "name", "write"));
        CHECK(file_bloom_contains(rd, fid, "fhash", "fh1"));
        auto file_hashes =
            rd.query_hash_table(idx::IndexDatabase::HashType::FILE);
        REQUIRE(file_hashes.count("fh1") == 1);
        CHECK(file_hashes.at("fh1") == "/data/a.bin");
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
                .query(R"(cat == "POSIX")")
                .export_json(s)
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
        DictFold dict(intern);
        std::array<Fold*, 2> fp{&bloom, &dict};
        IndexFoldDriver drv(intern, fp, gz, index_path);
        // Split mid-way (lands inside the "read" line) to force reassembly.
        std::size_t half = text.size() / 2;
        drv.on_chunk(text.data(), half, /*cp=*/0).get();
        drv.on_chunk(text.data() + half, text.size() - half, /*cp=*/0).get();
        drv.seal();

        int fid = -1;
        {
            idx::IndexDatabase db(index_path);
            fid = db.get_file_info_id(idx::internal::get_logical_path(gz));
            REQUIRE(fid >= 0);
            auto w = db.begin_write();
            bloom.write_to_sink(*w, fid);
            dict.write_to_sink(*w);
            w->commit();
        }

        idx::IndexDatabase rd(index_path, idx::IndexOpenMode::ReadOnly);
        // All three events survived the seam: both names, both cats, the fhash,
        // and the FH dictionary entry.
        CHECK(file_bloom_contains(rd, fid, "name", "read"));
        CHECK(file_bloom_contains(rd, fid, "name", "write"));
        CHECK(file_bloom_contains(rd, fid, "cat", "STDIO"));
        CHECK(file_bloom_contains(rd, fid, "fhash", "fh1"));
        auto file_hashes =
            rd.query_hash_table(idx::IndexDatabase::HashType::FILE);
        REQUIRE(file_hashes.count("fh1") == 1);
        CHECK(file_hashes.at("fh1") == "/data/a.bin");
    }

    // Schemaless column harvest: nested-object and array args surface as dotted
    // leaf columns with their types, and View::columns()/schema() read them
    // back from the index with no trace scan.
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
            View::from_file(gz, index_path)
                .emit_all_metadata(true)
                .export_json(s)
                .get();
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
        // Nested object -> dotted leaves; array -> representative index 0.
        CHECK(has("pos.x"));
        CHECK(has("pos.y"));
        CHECK(has("tags.0"));
        // Lifted hashes and their resolved.* aliases.
        CHECK(has("fhash"));
        CHECK(has("hhash"));
        CHECK(has("resolved.fpath"));
        CHECK(has("resolved.hostname"));

        std::unordered_map<std::string, std::string> ty;
        for (const auto& ci : v.schema()) ty[ci.name] = ci.type;
        CHECK(ty["pid"] == "int64");
        CHECK(ty["ts"] == "int64");
        CHECK(ty["hostname"] == "string");
        CHECK(ty["rate"] == "float64");
        CHECK(ty["pos.x"] == "int64");
        CHECK(ty["tags.0"] == "string");
        // size is int (1024) in one event and float (2.5) in another; the fold
        // widens it to float64.
        CHECK(ty["size"] == "float64");
        CHECK(ty["resolved.fpath"] == "string");
    }
}

TEST_SUITE("BloomCore") {
    using dftracer::utils::trace::visitors::BloomCore;

    TEST_CASE("merging a chunk's states keeps numeric min and max") {
        BloomCore::ChunkIndexerConfig config;
        BloomCore::ChunkState a, b;
        BloomCore::init_chunk_state(a, config, {});
        BloomCore::init_chunk_state(b, config, {});
        BloomCore::PidTidCache cache;
        BloomCore::observe_data(a, cache, config, "read", "POSIX", 9, 1, 9, 9,
                                true, "", "", "");
        BloomCore::observe_data(b, cache, config, "read", "POSIX", 10, 1, 10,
                                10, true, "", "", "");
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
        namespace ti = dftracer::utils::trace::indexing;
        ti::ResolveAndBuildInput input;
        input.files = {gz};
        input.require_bloom = true;
        input.bloom_config.extra_dimensions.clear();
        input.bloom_config.auto_fields = true;
        input.bloom_config.auto_max_distinct = 8;
        std::string index_path;
        dftu_utils_test::run_coro([&](dftracer::utils::CoroScope& scope)
                                      -> dftracer::utils::coro::CoroTask<void> {
            auto res = co_await ti::resolve_and_build_index(&scope, input);
            index_path = res.index_path;
        });
        idx::IndexDatabase db(index_path, idx::IndexOpenMode::ReadOnly);
        const int fid = file_id_of(db, gz);
        auto dims = db.query_index_dimensions(fid);
        for (auto d : {"size", "mode", "fname", "@auto"})
            CHECK(std::find(dims.begin(), dims.end(), d) != dims.end());

        auto size = db.query_chunk_dimension_stats_for_dimension(fid, "size");
        REQUIRE(size.size() >= 2);
        for (const auto& s : size) {
            CHECK(s.value_type == "int");
            CHECK_FALSE(s.min_value.empty());
        }
        CHECK(db.query_chunk_bloom_filters(fid, "size").empty());
        CHECK_FALSE(db.query_file_bloom_filter(fid, "size").has_value());

        CHECK(db.query_chunk_bloom_filters(fid, "mode").size() == size.size());
        CHECK(db.query_file_bloom_filter(fid, "mode").has_value());
        CHECK(db.query_chunk_bloom_filters(fid, "fname").size() < size.size());
        CHECK_FALSE(db.query_file_bloom_filter(fid, "fname").has_value());
    }
}
