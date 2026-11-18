// SPDX-License-Identifier: MIT
// describe() reports the names a loaded plugin registered, one list per kind,
// and whether its descriptor carries a fold. Each case loads a real .so through
// Plugins::Builder, so the lists come from the same registration path a plugin
// uses, not from a hand-filled struct.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <doctest/doctest.h>

using namespace dftracer::utils;

#ifndef TOY_FORMAT_PLUGIN_PATH
#error "TOY_FORMAT_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef TOY_SINK_PLUGIN_PATH
#error "TOY_SINK_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef UDF_PLUGIN_PATH
#error "UDF_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef UDAF_PLUGIN_PATH
#error "UDAF_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef PROVIDER_FIXTURE_PLUGIN_PATH
#error "PROVIDER_FIXTURE_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef BUILDER_PLUGIN_PATH
#error "BUILDER_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef INDEX_MINMAX_PLUGIN_PATH
#error "INDEX_MINMAX_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef LIFETIME_PLUGIN_PATH
#error "LIFETIME_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

using Info = plugins::host::Plugins::PluginInfo;

// One loaded plugin and what describe() said about it. The set is destroyed
// with the answer it made, which unregisters every name it added.
struct Described {
    bool ok = false;
    dstd::String error;
    dstd::Vector<Info> info;
};

Described describe_one(dstd::StringView path) {
    Described d;
    auto built =
        plugins::host::Plugins::Builder().add(dstd::String(path)).build();
    if (!built) {
        d.error = built.error().message();
        return d;
    }
    d.info = built->describe();
    d.ok = true;
    return d;
}

bool has(dstd::Span<const dstd::String> list, dstd::StringView name) {
    for (const dstd::String& s : list)
        if (s == name) return true;
    return false;
}

dstd::String joined(dstd::Span<const dstd::String> list) {
    dstd::String out;
    for (const dstd::String& s : list) {
        if (!out.empty()) out += ", ";
        out += s;
    }
    return out.empty() ? dstd::String("(none)") : out;
}

// The one plugin a case loaded, or none when the load failed (the MESSAGE says
// why).
const Info* only(const Described& d) {
    if (!d.ok) {
        MESSAGE("load failed: " << d.error);
        return nullptr;
    }
    if (d.info.size() != 1) {
        MESSAGE("describe() listed " << d.info.size() << " plugins, not 1");
        return nullptr;
    }
    return &d.info[0];
}

}  // namespace

TEST_CASE("describe() lists the file formats a plugin registered") {
    const Described d = describe_one(TOY_FORMAT_PLUGIN_PATH);
    const Info* p = only(d);
    REQUIRE(p != nullptr);
    CHECK(p->path == TOY_FORMAT_PLUGIN_PATH);
    CHECK(p->file_formats.size() == 2);
    CHECK(has(p->file_formats, "toy_format.rows"));
    CHECK(has(p->file_formats, "toy_format.low"));
    CHECK(p->sinks.empty());
    CHECK(p->udfs.empty());
    CHECK(p->providers.empty());
    MESSAGE("file_formats: " << joined(p->file_formats));
}

TEST_CASE("describe() lists the sink a plugin registered") {
    const Described d = describe_one(TOY_SINK_PLUGIN_PATH);
    const Info* p = only(d);
    REQUIRE(p != nullptr);
    REQUIRE(p->sinks.size() == 1);
    CHECK(p->sinks[0] == "toy_sink.out");
    CHECK(p->file_formats.empty());
    MESSAGE("sinks: " << joined(p->sinks));
}

TEST_CASE("describe() lists the udfs a plugin registered") {
    const Described d = describe_one(UDF_PLUGIN_PATH);
    const Info* p = only(d);
    REQUIRE(p != nullptr);
    CHECK(p->udfs.size() == 10);
    CHECK(has(p->udfs, "uadd"));
    CHECK(has(p->udfs, "uplus1"));
    CHECK(has(p->udfs, "ustr"));
    CHECK(p->udafs.empty());
    MESSAGE("udfs: " << joined(p->udfs));
}

TEST_CASE("describe() lists the udafs a plugin registered") {
    const Described d = describe_one(UDAF_PLUGIN_PATH);
    const Info* p = only(d);
    REQUIRE(p != nullptr);
    CHECK(has(p->udafs, "usum"));
    CHECK(has(p->udafs, "udistinct"));
    CHECK(has(p->udafs, "ucount"));
    CHECK(p->udfs.empty());
    MESSAGE("udafs: " << joined(p->udafs));
}

TEST_CASE("describe() lists the providers and nodes a plugin registered") {
    const Described d = describe_one(PROVIDER_FIXTURE_PLUGIN_PATH);
    const Info* p = only(d);
    REQUIRE(p != nullptr);
    REQUIRE(p->providers.size() == 3);
    CHECK(has(p->providers, "provider_fixture.rows"));
    CHECK(has(p->providers, "provider_fixture.failing"));
    CHECK(has(p->providers, "provider_fixture.sdk"));
    REQUIRE(p->nodes.size() == 1);
    CHECK(p->nodes[0] == "provider_fixture.double");
    MESSAGE("providers: " << joined(p->providers)
                          << "; nodes: " << joined(p->nodes));
}

TEST_CASE("describe() lists the ops and states of a plugin with a fold") {
    const Described d = describe_one(BUILDER_PLUGIN_PATH);
    const Info* p = only(d);
    REQUIRE(p != nullptr);
    REQUIRE(p->ops.size() == 1);
    CHECK(p->ops[0].find("builder_plugin.double_rows") != dstd::String::npos);
    REQUIRE(p->states.size() == 1);
    CHECK(p->states[0] == "builder_plugin.total_dur");
    // The built plugin folds its own row count, so its descriptor carries one.
    CHECK(p->has_fold);
    MESSAGE("ops: " << joined(p->ops) << "; states: " << joined(p->states));
}

TEST_CASE("describe() lists the index extensions a plugin registered") {
    const Described d = describe_one(INDEX_MINMAX_PLUGIN_PATH);
    const Info* p = only(d);
    REQUIRE(p != nullptr);
    REQUIRE(p->index_extensions.size() == 1);
    CHECK(p->index_extensions[0] == "index_minmax.range");
    MESSAGE("index_extensions: " << joined(p->index_extensions));
}

TEST_CASE("a plugin whose descriptor has no fold reports has_fold false") {
    // Every kind it registers is a name; nothing it declares runs per batch.
    const Described d = describe_one(LIFETIME_PLUGIN_PATH);
    const Info* p = only(d);
    REQUIRE(p != nullptr);
    CHECK_FALSE(p->has_fold);
    REQUIRE(p->providers.size() == 1);
    CHECK(p->providers[0] == "lifetime_plugin.src");
    REQUIRE(p->nodes.size() == 1);
    CHECK(p->nodes[0] == "lifetime_plugin.pass");
    REQUIRE(p->ops.size() == 1);
    CHECK(p->ops[0].find("lifetime_plugin.first_row") != dstd::String::npos);
    CHECK(p->udfs.empty());
    CHECK(p->udafs.empty());
    CHECK(p->states.empty());
    CHECK(p->file_formats.empty());
    CHECK(p->sinks.empty());
    CHECK(p->index_extensions.empty());
}
