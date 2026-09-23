// A plugin that declares its config keys gets them validated before it runs:
// an unknown key is a typo the caller must see, not a setting silently
// dropped. A plugin that declares none keeps the old unchecked behaviour.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/plugins/config.h>
#include <dftracer/utils/plugins/plugins.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#ifndef CONFIG_KEYS_PLUGIN_PATH
#error "CONFIG_KEYS_PLUGIN_PATH must be defined by CMake"
#endif

using dftracer::utils::plugins::ConfigTree;
using dftracer::utils::plugins::Plugins;

namespace {

// The fixture declares `label` required and `stride` optional.
ConfigTree config_of(const std::string& json) {
    return ConfigTree::from_json_string(json);
}

auto load(ConfigTree cfg) {
    return Plugins::builder()
        .add(CONFIG_KEYS_PLUGIN_PATH, std::move(cfg))
        .build();
}

bool has_substr(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("a declared config is accepted and reported by describe()") {
    auto set = load(config_of(R"({"label": "rows", "stride": 2})"));
    INFO((set.has_value() ? std::string{} : set.error().message));
    REQUIRE(set.has_value());

    auto info = set->describe();
    REQUIRE(info.size() == 1);
    const auto& keys = info[0].config_keys;
    CHECK(std::any_of(keys.begin(), keys.end(), [](const std::string& k) {
        return has_substr(k, "stride (int) - rows counted per row");
    }));
    CHECK(std::any_of(keys.begin(), keys.end(), [](const std::string& k) {
        return has_substr(k, "label (string, required)");
    }));
    // The C++ SDK reads `query` off every config, so it is declared too and a
    // caller passing one is not told it is unknown.
    CHECK(std::any_of(keys.begin(), keys.end(), [](const std::string& k) {
        return has_substr(k, "query (string)");
    }));
}

TEST_CASE("an undeclared key fails the load instead of being ignored") {
    auto set = load(config_of(R"({"label": "rows", "strid": 2})"));
    REQUIRE(!set.has_value());
    CHECK(has_substr(set.error().message, "unknown key 'strid'"));
}

TEST_CASE("a missing required key fails the load") {
    auto set = load(config_of(R"({"stride": 2})"));
    REQUIRE(!set.has_value());
    CHECK(has_substr(set.error().message, "required key 'label' is missing"));
}

TEST_CASE("a declared key of the wrong kind fails the load") {
    auto set = load(config_of(R"({"label": 7})"));
    REQUIRE(!set.has_value());
    CHECK(has_substr(set.error().message, "key 'label' must be string"));
}

TEST_CASE("a `query` key is accepted alongside the declared ones") {
    auto set = load(config_of(R"({"label": "rows", "query": "dur > 0"})"));
    INFO((set.has_value() ? std::string{} : set.error().message));
    CHECK(set.has_value());
}

TEST_CASE("an unparseable `query` fails the load") {
    // `query` becomes the plugin's plan_query. A predicate that does not parse
    // used to be logged and dropped, which cost the whole SET its index prune
    // and let this plugin fold over events its own predicate excluded. Both
    // are silent wrong answers, so the load fails instead.
    auto set = load(config_of(R"({"label": "rows", "query": "cat =="})"));
    REQUIRE(!set.has_value());
    CHECK(has_substr(set.error().message, "plan_query"));
    CHECK(has_substr(set.error().message, "does not parse"));
}

TEST_CASE("no config at all still fails when a key is required") {
    auto set = Plugins::builder().add(CONFIG_KEYS_PLUGIN_PATH).build();
    REQUIRE(!set.has_value());
    CHECK(has_substr(set.error().message, "required key 'label' is missing"));
}
