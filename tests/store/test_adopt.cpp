// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/store.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstddef>
#include <optional>
#include <utility>

using namespace dftracer::utils;

namespace {

namespace store = dftracer::utils::store;

using store::Namespace;
using store::NamespaceSpec;
using store::ScopeKey;
using store::Store;

dstd::ByteView view_of(const dstd::String& bytes) {
    return {bytes.data(), bytes.size()};
}

dstd::StringView sv(const dstd::String& text) {
    return dstd::StringView(text.data(), text.size());
}

NamespaceSpec spec_of(const dstd::String& name) {
    NamespaceSpec spec;
    spec.name = dstd::String(name.data(), name.size());
    return spec;
}

dstd::Expected<void, dstd::Error> run(
    coro::CoroTask<dstd::Expected<void, dstd::Error>> task) {
    return dftracer::utils::default_runtime().submit(std::move(task)).get();
}

Store open_store(const dstd::String& dir) {
    auto opened = Store::open(sv(dir));
    REQUIRE(opened.has_value());
    return std::move(*opened);
}

void write_scope(Store& store, const NamespaceSpec& spec,
                 const dstd::String& path, const dstd::String& value) {
    auto ns = store.ns(spec);
    REQUIRE(ns.has_value());
    auto batch = store.batch(
        *ns, ScopeKey::file(dstd::StringView(path.data(), path.size())));
    batch.put(view_of("k"), view_of(value));
    REQUIRE(run(store.commit(std::move(batch))).has_value());
    REQUIRE(run(store.close()).has_value());
}

dstd::Optional<dstd::String> read_scope(Store& store, const NamespaceSpec& spec,
                                        const dstd::String& path) {
    auto ns = store.ns(spec);
    REQUIRE(ns.has_value());
    auto snapshot = store.snapshot();
    REQUIRE(snapshot.has_value());
    auto found = snapshot->get(
        *ns, ScopeKey::file(dstd::StringView(path.data(), path.size())),
        view_of("k"));
    REQUIRE(found.has_value());
    if (!found->has_value()) return dstd::nullopt;
    const auto bytes = (*found)->bytes();
    return dstd::String(reinterpret_cast<const char*>(bytes.data()),
                        bytes.size());
}

}  // namespace

TEST_SUITE("store adopt") {
    TEST_CASE("adopt moves the data files and an open target sees them") {
        dftu_utils_test::ScopedTestDir root("store_adopt");
        const dstd::String from = root.str() + "/from";
        const dstd::String to = root.str() + "/to";
        const NamespaceSpec spec = spec_of("dftu.test.adopt");

        Store target = open_store(to);
        write_scope(target, spec, "/traces/own.pfw", "own");
        // The first snapshot arms the interval before the next look at the
        // folder.
        CHECK(read_scope(target, spec, "/traces/moved.pfw") == dstd::nullopt);

        {
            Store source = open_store(from);
            write_scope(source, spec, "/traces/moved.pfw", "moved");
        }
        auto moved = Store::adopt(sv(from), sv(to));
        REQUIRE(moved.has_value());
        CHECK(*moved == 1);

        CHECK(read_scope(target, spec, "/traces/moved.pfw") ==
              dstd::Optional<dstd::String>("moved"));
        CHECK(read_scope(target, spec, "/traces/own.pfw") ==
              dstd::Optional<dstd::String>("own"));

        auto again = Store::adopt(sv(from), sv(to));
        REQUIRE(again.has_value());
        CHECK(*again == 0);
    }

    TEST_CASE("adopt of a folder that cannot be listed is an Io error") {
        dftu_utils_test::ScopedTestDir root("store_adopt_missing");
        const dstd::String from = root.str() + "/absent";
        const dstd::String to = root.str() + "/to";
        auto moved = Store::adopt(sv(from), sv(to));
        REQUIRE_FALSE(moved.has_value());
        CHECK(moved.error().is(dstd::Condition::Io));
    }
}
