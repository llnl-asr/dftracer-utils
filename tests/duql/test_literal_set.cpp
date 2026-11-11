// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/literal_set.h>
#include <doctest/doctest.h>

#include <cstdint>

using namespace dftracer::utils;

namespace {

bool hit(const std::uint64_t* bits, std::size_t id) {
    return ((bits[id >> 6] >> (id & 63)) & 1) != 0;
}

}  // namespace

TEST_CASE("an empty set never scans") {
    const duql::LiteralSet set;
    CHECK_FALSE(set);
    CHECK(set.scan("abc") == nullptr);
    CHECK_FALSE(duql::LiteralSet::make({}));
}

TEST_CASE("scan sets one bit per literal found, past 64 literals") {
    if (!duql::LiteralSet::AVAILABLE) {
        CHECK_FALSE(
            duql::LiteralSet::make(dstd::Vector<dstd::StringView>{"a", "b"}));
        return;
    }
    dstd::Vector<dstd::String> owned;
    for (int i = 0; i < 70; ++i) owned.push_back(dstd::fmt<"<k{}>">(i));
    dstd::Vector<dstd::StringView> views(owned.begin(), owned.end());
    const duql::LiteralSet set = duql::LiteralSet::make(views);
    REQUIRE(set);
    CHECK(set.size() == 70);

    const std::uint64_t* bits = set.scan("x <k3> y <k64> z <k69><k3>");
    REQUIRE(bits != nullptr);
    for (std::size_t id = 0; id < 70; ++id)
        CHECK(hit(bits, id) == (id == 3 || id == 64 || id == 69));

    bits = set.scan("no literal here");
    REQUIRE(bits != nullptr);
    for (std::size_t id = 0; id < 70; ++id) CHECK_FALSE(hit(bits, id));

    bits = set.scan("");
    REQUIRE(bits != nullptr);
    for (std::size_t id = 0; id < 70; ++id) CHECK_FALSE(hit(bits, id));
}

TEST_CASE("a scan that finds every literal stops early and keeps all bits") {
    if (!duql::LiteralSet::AVAILABLE) return;
    const dstd::Vector<dstd::StringView> views{"read", "write"};
    const duql::LiteralSet set = duql::LiteralSet::make(views);
    REQUIRE(set);
    const std::uint64_t* bits = set.scan("write then read then write again");
    REQUIRE(bits != nullptr);
    CHECK(hit(bits, 0));
    CHECK(hit(bits, 1));
}
