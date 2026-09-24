#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/store/layout.h>
#include <doctest/doctest.h>

#include <string>

namespace layout = dftracer::utils::index::store::layout;
using layout::Ext;

TEST_SUITE("index layout") {
    TEST_CASE("keys order by extension, kind, file, then granule") {
        namespace pk = layout::path_kind;
        CHECK(layout::granule_key(Ext::ZONEMAP, pk::DATA, 1, 9) <
              layout::granule_key(Ext::ZONEMAP, pk::DATA, 2, 0));
        CHECK(layout::granule_key(Ext::ZONEMAP, pk::DATA, 1, 2) <
              layout::granule_key(Ext::ZONEMAP, pk::DATA, 1, 10));
        CHECK(layout::file_prefix(Ext::ZONEMAP, pk::DATA, 0xFFFF) <
              layout::file_prefix(Ext::ZONEMAP, pk::FILE, 0));
        CHECK(layout::file_prefix(Ext::MEMBERS, 0, 7).size() ==
              layout::KEY_PREFIX_BYTES);
    }

    TEST_CASE("a path's keys are contiguous within a file") {
        namespace pk = layout::path_kind;
        const auto a = layout::path_prefix(Ext::ZONEMAP, pk::DATA, 1, "a");
        const auto ab = layout::path_prefix(Ext::ZONEMAP, pk::DATA, 1, "ab");
        auto a_last = a;
        layout::append_u32(a_last, 0xFFFFFFFFU);
        CHECK(a_last < ab);
        CHECK(a_last < layout::prefix_end(a));
        CHECK_FALSE(ab < layout::prefix_end(a));
    }

    TEST_CASE("a file prefix range holds exactly that file's keys") {
        const auto prefix = layout::file_prefix(Ext::ZONEMAP, 3, 5);
        const auto end = layout::prefix_end(prefix);
        CHECK(prefix < layout::granule_key(Ext::ZONEMAP, 3, 5, 0xFFFFFFFFU));
        CHECK(layout::granule_key(Ext::ZONEMAP, 3, 5, 0xFFFFFFFFU) + "zz" <
              end);
        CHECK_FALSE(layout::file_prefix(Ext::ZONEMAP, 3, 6) < end);
        CHECK(layout::prefix_end("\x01\xFF") == "\x02");
    }

    TEST_CASE("value headers round-trip and reject another type") {
        const auto v = layout::with_header(Ext::ZONEMAP, 5, "payload");
        CHECK(v.size() == layout::VALUE_HEADER_BYTES + 7);
        CHECK(layout::payload(v, Ext::ZONEMAP, 5) == "payload");
        CHECK_FALSE(layout::payload(v, Ext::ZONEMAP, 6).has_value());
        CHECK_FALSE(layout::payload(v, Ext::MEMBERS, 5).has_value());
        CHECK_FALSE(layout::payload(v.substr(0, v.size() - 1), Ext::ZONEMAP, 5)
                        .has_value());
        CHECK_FALSE(layout::payload("short", Ext::ZONEMAP, 5).has_value());
    }

    TEST_CASE("manifest and file records round-trip") {
        const layout::ManifestEntry entry{3, 0xABCDEF0123456789ULL,
                                          layout::ExtStatus::FAILED};
        auto decoded = layout::decode_manifest(layout::encode_manifest(entry));
        REQUIRE(decoded.has_value());
        CHECK(decoded->version == 3);
        CHECK(decoded->params_hash == entry.params_hash);
        CHECK(decoded->status == layout::ExtStatus::FAILED);

        const layout::FileRecord record{42, 1, 2, 3};
        auto file =
            layout::decode_file_record(layout::encode_file_record(record));
        REQUIRE(file.has_value());
        CHECK(file->file_id == 42);
        CHECK(file->mtime == 1);
        CHECK(file->hash == 2);
        CHECK(file->size == 3);
        CHECK_FALSE(layout::decode_file_record("not a record").has_value());
    }
}
