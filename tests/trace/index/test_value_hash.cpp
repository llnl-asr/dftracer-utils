// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash/value_hash.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/extensions/bloom_filter.h>
#include <dftracer/utils/trace/index/extensions/kinds/payloads.h>
#include <dftracer/utils/trace/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/trace/index/plan/chunk_pruner.h>
#include <dftracer/utils/trace/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/trace/index/store/layout.h>
#include <dftracer/utils/trace/index/store/store_layout.h>
#include <dftracer/utils/trace/index/tables.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cmath>
#include <cstdint>
#include <limits>

using namespace dftracer::utils;
using trace::index::extensions::BloomFilter;
using trace::index::extensions::ScalableBloomFilter;
using trace::index::store::IndexExtension;

namespace {

constexpr std::int64_t INT64_TOP = std::numeric_limits<std::int64_t>::max();
constexpr std::uint64_t UINT64_TOP = std::numeric_limits<std::uint64_t>::max();

}  // namespace

TEST_SUITE("value hash") {
    TEST_CASE("a whole double hashes as the integer it equals") {
        CHECK(dstd::hash::value_hash_double(42.0) ==
              dstd::hash::value_hash_int64(42));
        CHECK(dstd::hash::value_hash_double(-7.0) ==
              dstd::hash::value_hash_int64(-7));
        CHECK(dstd::hash::value_hash_uint64(42) ==
              dstd::hash::value_hash_int64(42));
        CHECK(dstd::hash::value_hash_bool(true) ==
              dstd::hash::value_hash_int64(1));
        CHECK(dstd::hash::value_hash_double(-9223372036854775808.0) ==
              dstd::hash::value_hash_int64(
                  std::numeric_limits<std::int64_t>::min()));
    }

    TEST_CASE("a fraction never equals an integer") {
        CHECK(dstd::hash::value_hash_double(42.5) !=
              dstd::hash::value_hash_int64(42));
        CHECK(dstd::hash::value_hash_double(42.5) !=
              dstd::hash::value_hash_int64(43));
        CHECK(dstd::hash::value_hash_double(42.5) ==
              dstd::hash::value_hash_double(42.5));
    }

    TEST_CASE("negative zero is zero") {
        CHECK(dstd::hash::value_hash_double(-0.0) ==
              dstd::hash::value_hash_double(0.0));
        CHECK(dstd::hash::value_hash_double(-0.0) ==
              dstd::hash::value_hash_int64(0));
    }

    TEST_CASE("every NaN is one value") {
        const double quiet = std::numeric_limits<double>::quiet_NaN();
        const double other = -std::numeric_limits<double>::quiet_NaN();
        const double signaling = std::numeric_limits<double>::signaling_NaN();
        CHECK(dstd::hash::value_hash_double(quiet) ==
              dstd::hash::value_hash_double(other));
        CHECK(dstd::hash::value_hash_double(quiet) ==
              dstd::hash::value_hash_double(signaling));
        CHECK(dstd::hash::value_hash_double(quiet) !=
              dstd::hash::value_hash_double(0.0));
        CHECK(dstd::hash::value_hash_double(quiet) !=
              dstd::hash::value_hash_null());
    }

    TEST_CASE("a uint64 above int64 max is its own value") {
        const std::uint64_t above = static_cast<std::uint64_t>(INT64_TOP) + 1;
        CHECK(dstd::hash::value_hash_uint64(above) !=
              dstd::hash::value_hash_int64(
                  std::numeric_limits<std::int64_t>::min()));
        CHECK(dstd::hash::value_hash_uint64(above) !=
              dstd::hash::value_hash_int64(-1));
        CHECK(dstd::hash::value_hash_uint64(UINT64_TOP) !=
              dstd::hash::value_hash_int64(-1));
        CHECK(dstd::hash::value_hash_uint64(UINT64_TOP) !=
              dstd::hash::value_hash_uint64(UINT64_TOP - 1));
        CHECK(dstd::hash::value_hash_uint64(static_cast<std::uint64_t>(
                  INT64_TOP)) == dstd::hash::value_hash_int64(INT64_TOP));
        // 2^63 as a double is the integer 2^63.
        CHECK(dstd::hash::value_hash_double(9223372036854775808.0) ==
              dstd::hash::value_hash_uint64(above));
    }

    TEST_CASE("a string never equals a number") {
        CHECK(dstd::hash::value_hash_string("42") !=
              dstd::hash::value_hash_int64(42));
        CHECK(dstd::hash::value_hash_string("42") !=
              dstd::hash::value_hash_double(42.0));
        CHECK(dstd::hash::value_hash_string("true") !=
              dstd::hash::value_hash_bool(true));
        CHECK(dstd::hash::value_hash_string("") !=
              dstd::hash::value_hash_null());
        CHECK(dstd::hash::value_hash_string("42") ==
              dstd::hash::value_hash_string("42"));
    }
}

TEST_SUITE("bloom hash version") {
    TEST_CASE("a filter holds a value by its canonical hash") {
        ScalableBloomFilter bf(64, 0.01);
        bf.add_hashed(dstd::hash::value_hash_int64(42));
        bf.add_hashed(dstd::hash::value_hash_double(42.5));
        bf.add_hashed(dstd::hash::value_hash_double(-0.0));
        bf.add_hashed(dstd::hash::value_hash_double(std::nan("")));
        bf.add_hashed(dstd::hash::value_hash_uint64(UINT64_TOP));
        bf.add("text");

        CHECK(bf.possibly_contains_hashed(dstd::hash::value_hash_double(42.0)));
        CHECK(bf.possibly_contains_hashed(dstd::hash::value_hash_int64(42)));
        CHECK(bf.possibly_contains_hashed(dstd::hash::value_hash_double(42.5)));
        CHECK(bf.possibly_contains_hashed(dstd::hash::value_hash_int64(0)));
        CHECK(bf.possibly_contains_hashed(dstd::hash::value_hash_double(0.0)));
        CHECK(bf.possibly_contains_hashed(
            dstd::hash::value_hash_double(-std::nan(""))));
        CHECK(bf.possibly_contains_hashed(
            dstd::hash::value_hash_uint64(UINT64_TOP)));
        CHECK(bf.possibly_contains("text"));

        CHECK_FALSE(
            bf.possibly_contains_hashed(dstd::hash::value_hash_int64(43)));
        CHECK_FALSE(
            bf.possibly_contains_hashed(dstd::hash::value_hash_double(41.5)));
        CHECK_FALSE(
            bf.possibly_contains_hashed(dstd::hash::value_hash_int64(-1)));
        CHECK_FALSE(bf.possibly_contains("42"));
        CHECK_FALSE(bf.possibly_contains("0"));
    }

    TEST_CASE("the hash version is stored after the three header words") {
        BloomFilter bf(64, 0.01);
        const auto blob = bf.serialize();
        REQUIRE(blob.size() > 13);
        CHECK(blob[12] == dstd::hash::VALUE_HASH_VERSION);
        auto loaded = BloomFilter::from_blob(blob.data(), blob.size());
        REQUIRE(loaded.has_value());
        CHECK(loaded->hash_version() == dstd::hash::VALUE_HASH_VERSION);
        CHECK(loaded->has_known_hash_version());
    }

    TEST_CASE("a filter of another hash version may contain anything") {
        BloomFilter bf(64, 0.01);
        bf.add("present");
        auto blob = bf.serialize();
        CHECK_FALSE(bf.possibly_contains("absent"));
        blob[12] =
            static_cast<unsigned char>(dstd::hash::VALUE_HASH_VERSION + 1);
        auto loaded = BloomFilter::from_blob(blob.data(), blob.size());
        REQUIRE(loaded.has_value());
        CHECK_FALSE(loaded->has_known_hash_version());
        CHECK(loaded->possibly_contains("absent"));
        CHECK(loaded->possibly_contains("other"));
    }

    TEST_CASE(
        "a stored chain with an unknown hash version may contain anything") {
        ScalableBloomFilter chain(64, 0.01);
        chain.add("present");
        const auto blob = chain.serialize();
        // The chain header, one level header, then the level's 13 byte header.
        constexpr std::size_t VERSION_AT = 24 + 12 + 12;
        REQUIRE(blob.size() > VERSION_AT);
        REQUIRE(blob[VERSION_AT] == dstd::hash::VALUE_HASH_VERSION);

        auto stored =
            dftracer::utils::trace::index::extensions::kinds::encode_bloom(
                dstd::Span<const unsigned char>(blob.data(), blob.size()), 1);
        stored[8 + VERSION_AT] =
            static_cast<char>(dstd::hash::VALUE_HASH_VERSION + 1);
        auto decoded =
            dftracer::utils::trace::index::extensions::kinds::decode_bloom(
                stored);
        REQUIRE(decoded.has_value());
        CHECK_FALSE(decoded->has_known_hash_version());
        CHECK(decoded->possibly_contains("absent"));
        CHECK(
            decoded->possibly_contains_hashed(dstd::hash::value_hash_int64(5)));
    }
}

namespace {

enum class CatalogKind { None, Numbers, Mixed };

// Four chunks of one path "size": chunk 0 holds the int 42, chunk 1 the
// doubles 42.5, -0.0 and NaN, chunk 2 the string "42", chunk 3 the uint64
// maximum.
dstd::String build_typed_index(CatalogKind kind, dstd::CStringView name) {
    const dstd::String dir =
        dstd::String(dftu_utils_test::make_unique_test_path(name).str());
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String index_path = dir + "/test.pfw.gz.idx";
    const dstd::String file_path = "/fake/test.pfw.gz";

    auto index = dftu_utils_test::open_test_index(index_path);
    auto writer = dftu_utils_test::register_test_file(
        index, dftu_utils_test::index_key(file_path), 12345);

    const auto put = [&](std::uint64_t chunk, auto&& fill) {
        ScalableBloomFilter bf(64, 0.01);
        fill(bf);
        const auto blob = bf.serialize();
        dftu_utils_test::put_chunk_bloom(
            writer, chunk, "size",
            dstd::Span<const unsigned char>(blob.data(), blob.size()),
            bf.entry_count());
    };
    put(0, [](ScalableBloomFilter& bf) {
        bf.add_hashed(dstd::hash::value_hash_int64(42));
    });
    put(1, [](ScalableBloomFilter& bf) {
        bf.add_hashed(dstd::hash::value_hash_double(42.5));
        bf.add_hashed(dstd::hash::value_hash_double(-0.0));
        bf.add_hashed(dstd::hash::value_hash_double(std::nan("")));
    });
    put(2, [](ScalableBloomFilter& bf) { bf.add("42"); });
    put(3, [](ScalableBloomFilter& bf) {
        bf.add_hashed(dstd::hash::value_hash_uint64(UINT64_TOP));
    });
    trace::index::put_path(writer, IndexExtension::Bloom, "size");

    dstd::Vector<trace::index::gzip::GzipMemberRecord> members;
    for (std::uint64_t chunk = 0; chunk < 4; ++chunk) {
        trace::index::schemas::dft::ChunkStatistics stats;
        stats.total_events = 2;
        trace::index::put_chunk_statistics(writer, chunk, stats);
        trace::index::put_gzip_member(writer,
                                      trace::index::gzip::GzipMemberRecord{
                                          .member_idx = chunk,
                                          .c_offset = chunk * 100,
                                          .c_size = 100,
                                          .uc_offset = chunk * 200,
                                          .uc_size = 200,
                                          .first_line_num = chunk * 2 + 1,
                                          .last_line_num = chunk * 2 + 2,
                                      });
    }

    if (kind != CatalogKind::None) {
        using trace::index::store::PathType;
        trace::index::store::PathStat stat;
        const auto bit = [](PathType t) {
            return static_cast<std::uint8_t>(1U << static_cast<unsigned>(t));
        };
        if (kind == CatalogKind::Numbers) {
            stat.type = PathType::Double;
            stat.seen = bit(PathType::Int) | bit(PathType::Double);
        } else {
            stat.type = PathType::Mixed;
            stat.seen = bit(PathType::Int) | bit(PathType::String);
        }
        stat.count = 8;
        trace::index::put_catalog_path(writer, "args.size", stat);
        writer.finish(IndexExtension::Catalog, 0);
    }

    dftu_utils_test::mark_built(writer);
    dftu_utils_test::commit_test_build(writer);
    return index_path;
}

dstd::Vector<std::uint64_t> candidates(dstd::StringView index_path,
                                       dstd::StringView query_text) {
    auto q = duql::Query::from_string(query_text);
    REQUIRE(q.has_value());
    trace::index::plan::ChunkPrunerInput input{
        dstd::String(index_path), "/fake/test.pfw.gz", std::move(*q), nullptr};
    trace::index::plan::ChunkPruner pruner;
    auto out = dftu_utils_test::ok(pruner(input).get());
    CHECK(out.success);
    return out.candidate_checkpoints;
}

using Chunks = dstd::Vector<std::uint64_t>;

}  // namespace

TEST_SUITE("typed pruning") {
    TEST_CASE("an int column matches a double literal and the reverse") {
        const auto idx = build_typed_index(CatalogKind::Numbers, "vh_numbers");
        CHECK(candidates(idx, "args.size == 42") == Chunks{0});
        CHECK(candidates(idx, "args.size == 42.0") == Chunks{0});
        CHECK(candidates(idx, "args.size == 42.5") == Chunks{1});
        CHECK(candidates(idx, "args.size in [42.0, 42.5]") == Chunks{0, 1});
    }

    TEST_CASE("42.5 never matches 42") {
        const auto idx = build_typed_index(CatalogKind::Numbers, "vh_fraction");
        CHECK(candidates(idx, "args.size == 42").size() == 1);
        CHECK(candidates(idx, "args.size == 43.5").empty());
        CHECK(candidates(idx, "args.size == 41.5").empty());
    }

    TEST_CASE("negative zero equals zero") {
        const auto idx = build_typed_index(CatalogKind::Numbers, "vh_zero");
        CHECK(candidates(idx, "args.size == 0") == Chunks{1});
        CHECK(candidates(idx, "args.size == 0.0") == Chunks{1});
        CHECK(candidates(idx, "args.size == -0.0") == Chunks{1});
    }

    TEST_CASE("a uint64 above int64 max is found") {
        const auto idx = build_typed_index(CatalogKind::Numbers, "vh_uint64");
        // The duql parser reads an integer literal as an int64, so the
        // uint64 maximum is probed at the filter level above.
        CHECK(candidates(idx, "args.size == 9223372036854775807").empty());
        CHECK(candidates(idx, "args.size == -1").empty());
    }

    TEST_CASE("a number does not match a string unless the path is a string") {
        const auto numbers =
            build_typed_index(CatalogKind::Numbers, "vh_not_string");
        CHECK(candidates(numbers, "args.size == 42") == Chunks{0});
        CHECK(candidates(numbers, R"(args.size == "42")") == Chunks{2});

        const auto mixed = build_typed_index(CatalogKind::Mixed, "vh_string");
        CHECK(candidates(mixed, "args.size == 42") == Chunks{0, 2});
        CHECK(candidates(mixed, R"(args.size == "42")") == Chunks{2});
    }

    TEST_CASE("without a catalog a number probes both forms") {
        const auto idx = build_typed_index(CatalogKind::None, "vh_no_catalog");
        CHECK(candidates(idx, "args.size == 42") == Chunks{0, 2});
    }
}

TEST_SUITE("old index") {
    TEST_CASE("a bloom copy at an older format version is not current") {
        dftu_utils_test::TestEnvironment env;
        REQUIRE(env.is_valid());
        const dstd::String root = env.dir() + "/.dftindex";
        auto opened = trace::index::Index::open(root);
        REQUIRE(opened.has_value());
        const dstd::String path = "/traces/old.pfw.gz";
        auto build = dftu_utils_test::register_test_file(*opened, path, 5);
        dftu_utils_test::commit_test_build(build);

        const std::uint32_t current =
            trace::index::store::layout::ext_version(IndexExtension::Bloom);
        REQUIRE(current >= 1);
        auto store = dftracer::utils::store::Store::open(root + "/store");
        REQUIRE(store.has_value());
        dftracer::utils::store::NamespaceSpec spec;
        const dstd::String name =
            trace::index::store::store_layout::namespace_name(
                IndexExtension::Bloom);
        spec.name.assign(name.data(), name.size());
        spec.format_version = current - 1;
        auto ns = store->ns(spec);
        REQUIRE(ns.has_value());
        auto batch =
            store->batch(*ns, dftracer::utils::store::ScopeKey::file(path));
        batch.put(dstd::ByteView("\x02\x00", 2), dstd::ByteView());
        auto committed = dftracer::utils::default_runtime()
                             .submit(store->commit(std::move(batch)))
                             .get();
        REQUIRE(committed.has_value());

        const auto snapshot = dftu_utils_test::index_take(opened->snapshot());
        const auto file = snapshot.file(path);
        REQUIRE(file.has_value());
        CHECK_FALSE(trace::index::extension_current(snapshot, *file,
                                                    IndexExtension::Bloom));
        CHECK(snapshot.extension(*file, IndexExtension::Bloom, 0) !=
              trace::index::Freshness::Current);
        CHECK(trace::index::extension_current(snapshot, *file,
                                              IndexExtension::Members));
    }

    TEST_CASE("a zonemap copy at an older format version is not current") {
        dftu_utils_test::TestEnvironment env;
        REQUIRE(env.is_valid());
        const dstd::String root = env.dir() + "/.dftindex";
        auto opened = trace::index::Index::open(root);
        REQUIRE(opened.has_value());
        const dstd::String path = "/traces/old.pfw.gz";
        auto build = dftu_utils_test::register_test_file(*opened, path, 5);
        dftu_utils_test::commit_test_build(build);

        const std::uint32_t current =
            trace::index::store::layout::ext_version(IndexExtension::Zonemap);
        REQUIRE(current >= 1);
        auto store = dftracer::utils::store::Store::open(root + "/store");
        REQUIRE(store.has_value());
        dftracer::utils::store::NamespaceSpec spec;
        const dstd::String name =
            trace::index::store::store_layout::namespace_name(
                IndexExtension::Zonemap);
        spec.name.assign(name.data(), name.size());
        spec.format_version = current - 1;
        auto ns = store->ns(spec);
        REQUIRE(ns.has_value());
        auto batch =
            store->batch(*ns, dftracer::utils::store::ScopeKey::file(path));
        batch.put(dstd::ByteView("\x02\x00", 2), dstd::ByteView());
        auto committed = dftracer::utils::default_runtime()
                             .submit(store->commit(std::move(batch)))
                             .get();
        REQUIRE(committed.has_value());

        const auto snapshot = dftu_utils_test::index_take(opened->snapshot());
        const auto file = snapshot.file(path);
        REQUIRE(file.has_value());
        CHECK_FALSE(trace::index::extension_current(snapshot, *file,
                                                    IndexExtension::Zonemap));
        CHECK(snapshot.extension(*file, IndexExtension::Zonemap, 0) !=
              trace::index::Freshness::Current);
        CHECK(trace::index::extension_current(snapshot, *file,
                                              IndexExtension::Members));
    }
}
