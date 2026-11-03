// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/store/internal/index_encoding.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/tables.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <testing_utilities.h>

#include <cstdint>
#include <initializer_list>

namespace dstd = dftracer::utils::dstd;
namespace encoding = dftracer::utils::index::store::internal::encoding;
namespace idx = dftracer::utils::index;
namespace layout = dftracer::utils::index::store::layout;
namespace store = dftracer::utils::index::store;
using layout::Ext;

namespace {

dftracer::utils::dstd::String kind_key(std::uint8_t kind) {
    return dftracer::utils::dstd::String(1, static_cast<char>(kind));
}

/// Pins that the key range of one kind holds no key of another kind of the
/// same namespace, which is what lets a reader scan one kind's prefix.
void check_separate_ranges(std::initializer_list<std::uint8_t> kinds) {
    for (const std::uint8_t kind : kinds) {
        const dftracer::utils::dstd::String begin = kind_key(kind);
        const dftracer::utils::dstd::String end = layout::prefix_end(begin);
        dftracer::utils::dstd::String deepest = begin;
        deepest.append(16, '\xFF');
        CHECK(deepest < end);
        for (const std::uint8_t other : kinds) {
            if (other == kind) continue;
            const dftracer::utils::dstd::String other_key = kind_key(other);
            const bool inside = other_key >= begin && other_key < end;
            CHECK_FALSE(inside);
        }
    }
}

dftracer::utils::dstd::Optional<dftu_utils_test::IndexRecord> find_record(
    const dftracer::utils::dstd::Vector<dftu_utils_test::IndexRecord>& records,
    const dftracer::utils::dstd::String& name,
    const dftracer::utils::dstd::String& key) {
    for (const auto& record : records)
        if (record.name == name && record.key == key) return record;
    return dftracer::utils::dstd::nullopt;
}

}  // namespace

TEST_SUITE("index layout") {
    TEST_CASE("integers read big-endian") {
        using namespace std::string_view_literals;
        CHECK(layout::read_u16("\x01\x02"sv) == 0x0102);
        CHECK(layout::read_u32("\x01\x02\x03\x04"sv) == 0x01020304U);
        CHECK(layout::read_u64("\x01\x02\x03\x04\x05\x06\x07\x08"sv) ==
              0x0102030405060708ULL);
        CHECK_THROWS(layout::read_u32("\x01\x02\x03"sv));
    }

    TEST_CASE("a stored key is its kind byte then its big-endian tail") {
        dftu_utils_test::TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dftracer::utils::dstd::String root =
            dftracer::utils::dstd::String(env.get_dir().data(),
                                          env.get_dir().size()) +
            "/.dftindex";
        const dftracer::utils::dstd::String trace = "/traces/layout.pfw.gz";
        const idx::gzip::GzipMemberRecord member{1, 4096, 512, 100, 4096, 7, 8};
        idx::SourceStat stat;
        stat.hash = 1;
        auto build = dftu_utils_test::index_take(
            dftu_utils_test::open_test_index(root).build(
                dftracer::utils::dstd::StringView(trace.data(), trace.size()),
                stat));
        idx::put_file_metadata(build, 0, 0, 0, false);
        idx::put_gzip_member(build, member);
        idx::put_path_granule(build, store::IndexExtension::Zonemap, "cat", 2,
                              "zone");
        dftu_utils_test::mark_built(build);
        dftu_utils_test::commit_test_build(build);

        const auto records = dftu_utils_test::dump_index(root);
        dftracer::utils::dstd::String member_key =
            kind_key(layout::members::MEMBER);
        member_key.append("\x00\x00\x00\x01", 4);
        const auto member_record =
            find_record(records, "dftu.idx.core.members", member_key);
        REQUIRE(member_record.has_value());
        CHECK(member_record->value ==
              encoding::encode_gzip_member_value(member));

        dftracer::utils::dstd::String zone_key =
            kind_key(layout::path_kind::DATA);
        zone_key.append("cat");
        zone_key.push_back('\0');
        zone_key.append("\x00\x00\x00\x02", 4);
        const auto zone_record =
            find_record(records, "dftu.idx.zonemap", zone_key);
        REQUIRE(zone_record.has_value());
        CHECK(zone_record->value == "zone");
    }

    TEST_CASE("one kind's key range holds no key of another kind") {
        check_separate_ranges({layout::members::MEMBER,
                               layout::members::METADATA,
                               layout::members::RESTART});
        check_separate_ranges({layout::path_kind::PATHS,
                               layout::path_kind::DATA,
                               layout::path_kind::FILE});
        check_separate_ranges({layout::path_kind::PATHS,
                               layout::path_kind::IN_FILE,
                               layout::path_kind::IN_GRANULE});
        check_separate_ranges({layout::stats_kind::CHUNK_STATS,
                               layout::stats_kind::FILE_SCALAR_STATS,
                               layout::stats_kind::FILE_PID_TID_COUNTS});
        check_separate_ranges({layout::agg_kind::PARTIAL,
                               layout::agg_kind::META, layout::agg_kind::DICT,
                               layout::agg_kind::COLUMNS});
    }

    TEST_CASE("a prefix range ends at the first larger key") {
        using namespace std::string_view_literals;
        CHECK(layout::prefix_end("\x01\xFF"sv) == "\x02"sv);
        CHECK(layout::prefix_end("\x01"sv) == "\x02"sv);
        const dftracer::utils::dstd::String all_ff(3, '\xFF');
        CHECK(layout::prefix_end(all_ff) ==
              all_ff + dftracer::utils::dstd::String(8, '\xFF'));
        const dftracer::utils::dstd::String path_key =
            kind_key(layout::path_kind::DATA) + "a" + '\0';
        dftracer::utils::dstd::String last = path_key;
        last.append(4, '\xFF');
        CHECK(last < layout::prefix_end(path_key));
        const dftracer::utils::dstd::String next_path =
            kind_key(layout::path_kind::DATA) + "ab";
        CHECK_FALSE(next_path < layout::prefix_end(path_key));
    }

    TEST_CASE("chunk metadata round-trips and keeps an over-cap list unknown") {
        store::ChunkMetadata metadata;
        metadata.records = 0x0102030405060708ULL;
        metadata.names =
            dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>{
                "hhash", "name"};
        metadata.paths = dftracer::utils::dstd::nullopt;
        const dftracer::utils::dstd::String body =
            layout::encode_chunk_metadata(metadata);
        const auto decoded = layout::decode_chunk_metadata(body);
        REQUIRE(decoded.has_value());
        CHECK(decoded->records == metadata.records);
        REQUIRE(decoded->names.has_value());
        CHECK(*decoded->names == *metadata.names);
        CHECK_FALSE(decoded->paths.has_value());
        CHECK_FALSE(layout::decode_chunk_metadata("").has_value());
        CHECK_FALSE(
            layout::decode_chunk_metadata(body.substr(0, body.size() - 1))
                .has_value());
        CHECK_FALSE(layout::decode_chunk_metadata(body + "x").has_value());
    }

    TEST_CASE("a catalog path stat is ten bytes and rejects another type") {
        const store::PathStat stat{store::PathType::String, 0x28, 7};
        const dftracer::utils::dstd::String body =
            layout::encode_path_stat(stat);
        CHECK(body == dftracer::utils::dstd::String(
                          "\x05\x28\x00\x00\x00\x00\x00\x00\x00\x07", 10));
        const auto decoded = layout::decode_path_stat(body);
        REQUIRE(decoded.has_value());
        CHECK(decoded->type == store::PathType::String);
        CHECK(decoded->seen == 0x28);
        CHECK(decoded->count == 7);
        CHECK_FALSE(layout::decode_path_stat(body.substr(0, 9)).has_value());
        CHECK_FALSE(layout::decode_path_stat(body + "x").has_value());
        const dftracer::utils::dstd::String unknown_type(
            "\x09\x00\x00\x00\x00\x00\x00\x00\x00\x07", 10);
        CHECK_FALSE(layout::decode_path_stat(unknown_type).has_value());
    }
}
