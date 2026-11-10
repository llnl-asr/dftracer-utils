// SPDX-License-Identifier: MIT
// A plugin that declares its config keys gets them validated before it runs:
// an unknown key is a typo the caller must see, not a setting silently
// dropped. A plugin that declares none keeps the old unchecked behaviour.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/config.h>
#include <dftracer/utils/plugins/plugins.h>
#include <doctest/doctest.h>

#include <algorithm>

using namespace dftracer::utils;

#ifndef CONFIG_KEYS_PLUGIN_PATH
#error "CONFIG_KEYS_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

// The fixture declares `label` required and `stride` optional.
plugins::ConfigTree config_of(const dstd::String& json) {
    auto tree = plugins::ConfigTree::from_json_string(json);
    REQUIRE(tree.has_value());
    return std::move(*tree);
}

auto load(plugins::ConfigTree cfg) {
    return plugins::Plugins::builder()
        .add(CONFIG_KEYS_PLUGIN_PATH, std::move(cfg))
        .build();
}

bool has_substr(dstd::StringView hay, dstd::StringView needle) {
    return hay.find(needle) != dstd::StringView::npos;
}

}  // namespace

TEST_CASE("a declared config is accepted and reported by describe()") {
    auto set = load(config_of(R"({"label": "rows", "stride": 2})"));
    INFO((set.has_value() ? dstd::String{}
                          : dstd::String(set.error().message())));
    REQUIRE(set.has_value());

    auto info = set->describe();
    REQUIRE(info.size() == 1);
    const auto& keys = info[0].config_keys;
    CHECK(std::any_of(keys.begin(), keys.end(), [](const dstd::String& k) {
        return has_substr(k, "stride (int) - rows counted per row");
    }));
    CHECK(std::any_of(keys.begin(), keys.end(), [](const dstd::String& k) {
        return has_substr(k, "label (string, required)");
    }));
    // The C++ SDK reads `query` off every config, so it is declared too and a
    // caller passing one is not told it is unknown.
    CHECK(std::any_of(keys.begin(), keys.end(), [](const dstd::String& k) {
        return has_substr(k, "duql (string)");
    }));
}

TEST_CASE("an undeclared key fails the load instead of being ignored") {
    auto set = load(config_of(R"({"label": "rows", "strid": 2})"));
    REQUIRE(!set.has_value());
    CHECK(has_substr(set.error().message(), "unknown key 'strid'"));
}

TEST_CASE("a missing required key fails the load") {
    auto set = load(config_of(R"({"stride": 2})"));
    REQUIRE(!set.has_value());
    CHECK(has_substr(set.error().message(), "required key 'label' is missing"));
}

TEST_CASE("a declared key of the wrong kind fails the load") {
    auto set = load(config_of(R"({"label": 7})"));
    REQUIRE(!set.has_value());
    CHECK(has_substr(set.error().message(), "key 'label' must be string"));
}

TEST_CASE("a `query` key is accepted alongside the declared ones") {
    auto set = load(config_of(R"({"label": "rows", "duql": "dur > 0"})"));
    INFO((set.has_value() ? dstd::String{}
                          : dstd::String(set.error().message())));
    CHECK(set.has_value());
}

TEST_CASE("an unparseable `query` fails the load") {
    // `query` becomes the plugin's plan_duql. A predicate that does not parse
    // used to be logged and dropped, which cost the whole SET its index prune
    // and let this plugin fold over events its own predicate excluded. Both
    // are silent wrong answers, so the load fails instead.
    auto set = load(config_of(R"({"label": "rows", "duql": "cat =="})"));
    REQUIRE(!set.has_value());
    CHECK(has_substr(set.error().message(), "plan_duql"));
    CHECK(has_substr(set.error().message(), "does not parse"));
}

TEST_CASE("no config at all still fails when a key is required") {
    auto set = plugins::Plugins::builder().add(CONFIG_KEYS_PLUGIN_PATH).build();
    REQUIRE(!set.has_value());
    CHECK(has_substr(set.error().message(), "required key 'label' is missing"));
}

TEST_CASE("a config that is not JSON is a parse error") {
    auto tree = plugins::ConfigTree::from_json_string("{not json");
    REQUIRE(!tree.has_value());
    CHECK(common::errorcode_of(tree.error()) == common::ErrorCode::Parse);
    CHECK(has_substr(tree.error().message(), "failed to parse plugin config"));
}

TEST_CASE("a config whose root is not an object is an invalid argument") {
    auto tree = plugins::ConfigTree::from_json_string("[1, 2]");
    REQUIRE(!tree.has_value());
    CHECK(common::errorcode_of(tree.error()) ==
          common::ErrorCode::InvalidArgument);
    CHECK(has_substr(tree.error().message(),
                     "plugin config root must be a JSON object"));
}

TEST_CASE("a config file that is missing is an I/O error") {
    auto tree = plugins::ConfigTree::from_json_file(
        "/nonexistent/dftracer-utils-no-such-config.json");
    REQUIRE(!tree.has_value());
    CHECK(common::errorcode_of(tree.error()) == common::ErrorCode::Io);
}
