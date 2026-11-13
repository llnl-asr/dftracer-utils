// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/extensions/extension_registry.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/tables.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstdint>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::index;
using namespace dftracer::utils::trace::index::extensions;

namespace {

constexpr dstd::StringView NAME = "test.fake";

struct Knobs {
    std::uint64_t max_payload = 0;
    std::uint64_t oversize_granule = UINT64_MAX;
    std::uint64_t fail_step_granule = UINT64_MAX;
    bool null_query = false;
    bool file_check = true;
    int finish_file_calls = 0;
    std::uint64_t current_granule = 0;
};

class FakeQuery final : public ExtensionQuery {
   public:
    bool may_match(dstd::StringView payload) const override {
        return payload.size() > 0 && payload[0] == 'g';
    }
    bool file_may_match(dstd::StringView) const override { return true; }
};

class FakeBuilder final : public ExtensionBuilder {
   public:
    explicit FakeBuilder(Knobs& k) : k_(k) {}

    dstd::Result<void> step(dstd::Span<const dstd::StringView> lines) override {
        if (k_.current_granule == k_.fail_step_granule)
            return common::make_error<"step refused">(
                common::ErrorCode::Internal);
        lines_ += lines.size();
        return {};
    }
    dstd::Result<void> finish_granule(std::uint64_t granule,
                                      ExtensionOut& out) override {
        if (granule == k_.oversize_granule) {
            out.append("head");
            out.append(dstd::String(2 * 1024 * 1024, 'x'));
        } else
            out.append(dstd::String("g") + dstd::int_text(lines_));
        lines_ = 0;
        return {};
    }
    bool has_finish_file() const override { return true; }
    dstd::Result<void> finish_file(ExtensionOut& out) override {
        ++k_.finish_file_calls;
        out.append("file");
        return {};
    }

   private:
    Knobs& k_;
    std::uint64_t lines_ = 0;
};

class FakeExtension final : public Extension {
   public:
    explicit FakeExtension(Knobs& k) : k_(k) {}

    dstd::StringView name() const override { return NAME; }
    std::uint32_t version() const override { return 3; }
    std::uint64_t params_hash() const override { return 9; }
    std::uint64_t max_payload() const override { return k_.max_payload; }
    bool has_file_check() const override { return k_.file_check; }
    dstd::Result<dstd::UniquePtr<ExtensionBuilder>> make_builder()
        const override {
        return dstd::UniquePtr<ExtensionBuilder>(
            dstd::make_unique<FakeBuilder>(k_));
    }
    dstd::Result<dstd::UniquePtr<ExtensionQuery>> compile(
        const ExtensionLeaf&) const override {
        if (k_.null_query) return dstd::UniquePtr<ExtensionQuery>();
        return dstd::UniquePtr<ExtensionQuery>(dstd::make_unique<FakeQuery>());
    }

   private:
    Knobs& k_;
};

struct Registered {
    Knobs knobs;
    Registered() {
        REQUIRE(register_extension(
                    ExtensionPtr(dstd::make_shared<FakeExtension>(knobs)))
                    .has_value());
    }
    ~Registered() { unregister_extension(NAME); }
};

dstd::Expected<void, dstd::Error> run(
    coro::CoroTask<dstd::Expected<void, dstd::Error>> task) {
    return default_runtime().submit(std::move(task)).get();
}

// Feeds three chunks through the registered extensions into a file build and
// reads back what the build stored.
PluginData build_three_chunks(Registered& reg, dstd::String& dir) {
    auto opened = Index::open(dir + "/.dftindex");
    REQUIRE(opened.has_value());
    SourceStat stat;
    stat.mtime = 1;
    stat.size = 2;
    auto build = std::move(*opened->build("/traces/a.pfw.gz", stat));
    put_file_metadata(build, 4096, 10, 100, false);
    gzip::GzipMemberRecord member;
    member.c_size = 50;
    member.uc_size = 100;
    member.first_line_num = 1;
    member.last_line_num = 10;
    put_gzip_member(build, member);
    build.finish(trace::index::store::IndexExtension::Members, 11);

    ExtensionBuilders builders(
        trace::index::extensions::registered_extensions());
    const dstd::StringView lines[] = {"{}", "{}"};
    for (std::uint64_t g = 0; g < 3; ++g) {
        reg.knobs.current_granule = g;
        builders.step(g, dstd::Span<const dstd::StringView>(lines, 2));
    }
    builders.finish();
    builders.write(build);
    REQUIRE(run(build.commit()).has_value());

    auto snapshot = opened->snapshot();
    REQUIRE(snapshot.has_value());
    const auto file = snapshot->file("/traces/a.pfw.gz");
    REQUIRE(file.has_value());
    auto copies = plugins(*snapshot, *file);
    REQUIRE(copies.has_value());
    if (copies->empty()) return {};
    CHECK((*copies)[0].version == 3);
    CHECK((*copies)[0].params_hash == 9);
    auto data = plugin(*snapshot, *file, NAME);
    REQUIRE(data.has_value());
    REQUIRE(data->has_value());
    return std::move(**data);
}

}  // namespace

TEST_CASE("an extension is found by name until it is unregistered") {
    {
        Registered reg;
        const auto found = find_extension(NAME);
        REQUIRE(found != nullptr);
        CHECK(found->name() == NAME);
        CHECK(trace::index::extensions::registered_extensions().size() == 1);
        CHECK(found->current(trace::index::store::ExtensionState{3, 9, true}));
        CHECK_FALSE(
            found->current(trace::index::store::ExtensionState{4, 9, true}));
        CHECK_FALSE(
            found->current(trace::index::store::ExtensionState{3, 8, true}));
        CHECK_FALSE(
            found->current(trace::index::store::ExtensionState{3, 9, false}));
    }
    CHECK(find_extension(NAME) == nullptr);
    CHECK(trace::index::extensions::registered_extensions().empty());
}

TEST_CASE("a second extension under a taken name is refused") {
    Registered reg;
    Knobs other;
    const auto again = register_extension(
        ExtensionPtr(dstd::make_shared<FakeExtension>(other)));
    CHECK_FALSE(again.has_value());
    CHECK(trace::index::extensions::registered_extensions().size() == 1);
}

TEST_CASE("the builders store one payload per chunk and one for the file") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    Registered reg;
    dstd::String dir = env.dir();
    const auto data = build_three_chunks(reg, dir);
    REQUIRE(data.granules.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(data.granules[i].first == i);
        CHECK(data.granules[i].second == "g2");
    }
    CHECK(data.file.value_or("") == "file");
}

TEST_CASE("a payload over max_payload is stored as nothing") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    Registered reg;
    reg.knobs.oversize_granule = 1;
    dstd::String dir = env.dir();
    const auto data = build_three_chunks(reg, dir);
    REQUIRE(data.granules.size() == 2);
    CHECK(data.granules[0].first == 0);
    CHECK(data.granules[1].first == 2);
}

TEST_CASE("a payload past a custom max_payload is stored as nothing") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    Registered reg;
    reg.knobs.max_payload = 1;
    dstd::String dir = env.dir();
    const auto data = build_three_chunks(reg, dir);
    CHECK(data.granules.empty());
    CHECK_FALSE(data.file.has_value());
}

TEST_CASE("a failing step stores nothing for the chunk and skips the file") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    Registered reg;
    reg.knobs.fail_step_granule = 1;
    dstd::String dir = env.dir();
    const auto data = build_three_chunks(reg, dir);
    REQUIRE(data.granules.size() == 2);
    CHECK(data.granules[0].first == 0);
    CHECK(data.granules[1].first == 2);
    CHECK_FALSE(data.file.has_value());
    CHECK(reg.knobs.finish_file_calls == 0);
}

TEST_CASE("a compile that returns no query means no pruning") {
    Registered reg;
    const auto ext = find_extension(NAME);
    REQUIRE(ext != nullptr);
    const dstd::String value = "x";
    ExtensionLeaf leaf;
    leaf.path = "name";
    leaf.value.kind = ExtensionValue::Kind::String;
    leaf.value.str = &value;

    auto query = ext->compile(leaf);
    REQUIRE(query.has_value());
    REQUIRE(*query != nullptr);
    CHECK((*query)->may_match("g1"));
    CHECK_FALSE((*query)->may_match("zzz"));

    reg.knobs.null_query = true;
    auto none = ext->compile(leaf);
    REQUIRE(none.has_value());
    CHECK(*none == nullptr);
}
