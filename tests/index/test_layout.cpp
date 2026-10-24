#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/index/store/key_codec.h>
#include <dftracer/utils/index/store/layout.h>
#include <doctest/doctest.h>

#include <string>
#include <string_view>

namespace layout = dftracer::utils::index::store::layout;
using dftracer::utils::append_u32_be;
using layout::Ext;

TEST_SUITE("index layout") {
    TEST_CASE("stored key and header bytes are big-endian and fixed") {
        using dftracer::utils::index::store::KeyCodec;
        using namespace std::string_view_literals;
        CHECK(KeyCodec::encode_be32(0x01020304U) == "\x01\x02\x03\x04"sv);
        CHECK(KeyCodec::encode_be64(0x0102030405060708ULL) ==
              "\x01\x02\x03\x04\x05\x06\x07\x08"sv);
        CHECK(KeyCodec::decode_be64("\x01\x02\x03\x04\x05\x06\x07\x08"sv) ==
              0x0102030405060708ULL);
        CHECK(layout::granule_key(Ext::Zonemap, layout::path_kind::DATA,
                                  0x01020304U, 0x0A0B0C0DU) ==
              "\x00\x04\x01\x01\x02\x03\x04\x0A\x0B\x0C\x0D"sv);
        CHECK(layout::with_header(Ext::Zonemap, 5, "ab") ==
              "\x04\x05\x00\x00\x00\x00\x00\x00\x02"
              "ab"sv);
        CHECK(layout::read_u16("\x01\x02"sv) == 0x0102);
        CHECK(layout::read_u32("\x01\x02\x03\x04"sv) == 0x01020304U);
        CHECK(layout::read_u64("\x01\x02\x03\x04\x05\x06\x07\x08"sv) ==
              0x0102030405060708ULL);
        const layout::ManifestEntry entry{3, 0xABCDEF0123456789ULL,
                                          layout::ExtStatus::Failed};
        const auto value = layout::encode_manifest(entry);
        const auto body =
            layout::payload(value, Ext::Host, layout::host::MANIFEST);
        REQUIRE(body.has_value());
        CHECK(body->substr(0, 12) ==
              "\x00\x00\x00\x03\xAB\xCD\xEF\x01\x23\x45\x67\x89"sv);
    }

    TEST_CASE("a manifest that does not decode is not a manifest") {
        const auto value = layout::encode_manifest(
            {layout::ext_version(Ext::Zonemap), 7, layout::ExtStatus::Ready});
        REQUIRE(layout::decode_manifest(value).has_value());
        CHECK_FALSE(layout::decode_manifest("").has_value());
        CHECK_FALSE(layout::decode_manifest(value.substr(0, value.size() - 1))
                        .has_value());
        auto other_payload_version = value;
        other_payload_version[2] = static_cast<char>(
            static_cast<unsigned char>(other_payload_version[2]) + 1);
        CHECK_FALSE(layout::decode_manifest(other_payload_version).has_value());
        auto bad_status = value;
        bad_status.back() = '\x7F';
        CHECK_FALSE(layout::decode_manifest(bad_status).has_value());
    }

    TEST_CASE("keys order by extension, kind, file, then granule") {
        namespace pk = layout::path_kind;
        CHECK(layout::granule_key(Ext::Zonemap, pk::DATA, 1, 9) <
              layout::granule_key(Ext::Zonemap, pk::DATA, 2, 0));
        CHECK(layout::granule_key(Ext::Zonemap, pk::DATA, 1, 2) <
              layout::granule_key(Ext::Zonemap, pk::DATA, 1, 10));
        CHECK(layout::file_prefix(Ext::Zonemap, pk::DATA, 0xFFFF) <
              layout::file_prefix(Ext::Zonemap, pk::FILE, 0));
        CHECK(layout::file_prefix(Ext::Members, 0, 7).size() ==
              layout::KEY_PREFIX_BYTES);
    }

    TEST_CASE("a path's keys are contiguous within a file") {
        namespace pk = layout::path_kind;
        const auto a = layout::path_prefix(Ext::Zonemap, pk::DATA, 1, "a");
        const auto ab = layout::path_prefix(Ext::Zonemap, pk::DATA, 1, "ab");
        auto a_last = a;
        append_u32_be(a_last, 0xFFFFFFFFU);
        CHECK(a_last < ab);
        CHECK(a_last < layout::prefix_end(a));
        CHECK_FALSE(ab < layout::prefix_end(a));
    }

    TEST_CASE("a file prefix range holds exactly that file's keys") {
        const auto prefix = layout::file_prefix(Ext::Zonemap, 3, 5);
        const auto end = layout::prefix_end(prefix);
        CHECK(prefix < layout::granule_key(Ext::Zonemap, 3, 5, 0xFFFFFFFFU));
        CHECK(layout::granule_key(Ext::Zonemap, 3, 5, 0xFFFFFFFFU) + "zz" <
              end);
        CHECK_FALSE(layout::file_prefix(Ext::Zonemap, 3, 6) < end);
        CHECK(layout::prefix_end("\x01\xFF") == "\x02");
    }

    TEST_CASE("value headers round-trip and reject another type") {
        const auto v = layout::with_header(Ext::Zonemap, 5, "payload");
        CHECK(v.size() == layout::VALUE_HEADER_BYTES + 7);
        CHECK(layout::payload(v, Ext::Zonemap, 5) == "payload");
        CHECK_FALSE(layout::payload(v, Ext::Zonemap, 6).has_value());
        CHECK_FALSE(layout::payload(v, Ext::Members, 5).has_value());
        CHECK_FALSE(layout::payload(v.substr(0, v.size() - 1), Ext::Zonemap, 5)
                        .has_value());
        CHECK_FALSE(layout::payload("short", Ext::Zonemap, 5).has_value());
    }

    TEST_CASE("manifest and file records round-trip") {
        const layout::ManifestEntry entry{3, 0xABCDEF0123456789ULL,
                                          layout::ExtStatus::Failed};
        auto decoded = layout::decode_manifest(layout::encode_manifest(entry));
        REQUIRE(decoded.has_value());
        CHECK(decoded->version == 3);
        CHECK(decoded->params_hash == entry.params_hash);
        CHECK(decoded->status == layout::ExtStatus::Failed);

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
