// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/string.h>
#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <type_traits>

using namespace dftracer::utils;

static_assert(!std::is_constructible_v<dstd::CStringView, dstd::StringView>,
              "a StringView promises no terminator");
static_assert(std::is_convertible_v<dstd::CStringView, dstd::StringView>);

TEST_CASE("CStringView from a literal ends in a terminator") {
    constexpr dstd::CStringView v = "trace.pfw";
    static_assert(v.size() == 9);
    CHECK(v.c_str()[v.size()] == '\0');
    CHECK(std::strcmp(v.c_str(), "trace.pfw") == 0);
}

TEST_CASE("CStringView from strings points at their buffer") {
    dstd::String owned("a/b/c");
    dstd::CStringView from_dstd = owned;
    CHECK(from_dstd.c_str() == owned.c_str());
    CHECK(from_dstd.size() == 5);

    std::string legacy("xyz");  // keep std: CStringView takes a std::string
    dstd::CStringView from_std = legacy;
    CHECK(from_std.c_str() == legacy.c_str());
    CHECK(from_std.view() == "xyz");
}

TEST_CASE("CStringView from a ConstString") {
    static constexpr dstd::ConstString<16> name("index");
    constexpr dstd::CStringView v = name;
    static_assert(v.size() == 5);
    CHECK(v.c_str()[5] == '\0');
}

TEST_CASE("CStringView default is an empty C string") {
    dstd::CStringView v;
    CHECK(v.empty());
    CHECK(v.c_str()[0] == '\0');
    CHECK(v == dstd::CStringView(""));
}
