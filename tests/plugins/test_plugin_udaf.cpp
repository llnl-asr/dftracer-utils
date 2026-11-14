// SPDX-License-Identifier: MIT
// User-defined aggregates a plugin registers from its factory, and the state
// table that holds their states: states grow without moving, update skips null
// rows unless the UDAF is null aware, tables merge, states survive a
// serialize round trip, every state that init made is destroyed, a failing
// callback reports its message, and a state table keeps the plugin library
// loaded. In-process UDAFs go through the same adapter as plugin ones.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/udaf.h>
#include <dftracer/utils/dataframe/udaf_registry.h>
#include <dftracer/utils/dataframe/udaf_states.h>
#include <dftracer/utils/dataframe/udf.h>
#include <dftracer/utils/dataframe/udf_registry.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dlfcn.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>

using namespace dftracer::utils;
using dataframe::Series;
using dataframe::TypeId;
using dataframe::UdafStates;

#ifndef UDAF_PLUGIN_PATH
#error "UDAF_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef UDAF_BAD_PLUGIN_PATH
#error "UDAF_BAD_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

using Ints = dstd::Vector<std::int64_t>;

bool has(dstd::StringView text, dstd::StringView word) {
    return text.find(word) != dstd::StringView::npos;
}

dstd::Optional<plugins::host::Plugins> load(const char* path) {
    auto built = plugins::host::Plugins::builder().add(path).build();
    INFO((built.has_value() ? dstd::String{}
                            : dstd::String(built.error().message())));
    REQUIRE(built.has_value());
    return std::move(*built);
}

bool library_mapped() {
    void* h = dlopen(UDAF_PLUGIN_PATH, RTLD_NOW | RTLD_NOLOAD);
    if (!h) return false;
    dlclose(h);
    return true;
}

std::int64_t counter(const char* symbol) {
    void* h = dlopen(UDAF_PLUGIN_PATH, RTLD_NOW | RTLD_NOLOAD);
    REQUIRE(h != nullptr);
    auto* fn = reinterpret_cast<std::int64_t (*)()>(dlsym(h, symbol));
    REQUIRE(fn != nullptr);
    const std::int64_t value = fn();
    dlclose(h);
    return value;
}

// Row i is null when `null_every` divides i + 1; zero means no nulls.
Series ints(const Ints& values, std::int64_t null_every = 0) {
    dstd::Vector<std::uint8_t> valid((values.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < values.size(); ++i)
        if (null_every == 0 ||
            (static_cast<std::int64_t>(i) + 1) % null_every != 0)
            valid[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    return Series::flat_i64(values.data(),
                            static_cast<std::int64_t>(values.size()),
                            null_every == 0 ? nullptr : valid.data());
}

dstd::Result<UdafStates> table_of(dstd::StringView name, TypeId arg) {
    auto record = dataframe::find_udaf(name);
    if (!record)
        return common::make_error<"no udaf">(common::ErrorCode::NotFound);
    const dataframe::DataType type = dataframe::scalar(arg);
    auto bound = dataframe::bind_udaf(
        std::move(record), dstd::Span<const dataframe::DataType>(&type, 1), {});
    if (!bound) return dstd::Unexpected(std::move(bound.error()));
    return UdafStates(std::move(*bound));
}

Ints column(const Series& s) {
    Ints out;
    for (std::int64_t i = 0; i < s.length(); ++i)
        out.push_back(s.data<std::int64_t>()[i]);
    return out;
}

dstd::Result<void> feed(UdafStates& t, const Ints& gids, const Series& col) {
    const Series* args[] = {&col};
    return t.update(gids, args);
}

// In-process aggregates: a POD sum, and a count of distinct values whose state
// owns heap memory and counts its own destruction.
struct SumAgg {
    static constexpr const char* NAME = "cxx_sum";
    struct State {
        std::int64_t total = 0;
    };

    static dstd::Result<dstd::UniquePtr<SumAgg>> bind(
        dataframe::UdfBind& bind) {
        if (bind.type(0) != DFTU_TYPE_INT64)
            return common::make_error<"cxx_sum takes an Int64">(
                common::ErrorCode::InvalidArgument);
        bind.result(DFTU_TYPE_INT64);
        return dstd::make_unique<SumAgg>();
    }
    dstd::Result<void> update(dataframe::UdafStateSet<State>& states,
                              const dataframe::UdfBatch& batch) const {
        const auto* in =
            static_cast<const std::int64_t*>(dftu_series_data(batch.arg(0)));
        for (std::uint64_t i = 0; i < states.size(); ++i)
            if (State* s = states[i]) s->total += in[i];
        return {};
    }
    dstd::Result<void> combine(State& into, const State& from) const {
        into.total += from.total;
        return {};
    }
    dstd::Result<dftu_series*> finalize(
        dataframe::UdafStateSet<State>& states) const {
        Ints out;
        for (std::uint64_t i = 0; i < states.size(); ++i)
            out.push_back(states[i]->total);
        return dftu_series_new_flat(DFTU_TYPE_INT64, out.data(),
                                    static_cast<std::int64_t>(out.size()),
                                    nullptr);
    }
};

template <bool Clash>
struct SumNamed : SumAgg {
    static constexpr const char* NAME = Clash ? "sum" : "cxx_sum";
    static dstd::Result<dstd::UniquePtr<SumNamed>> bind(
        dataframe::UdfBind& bind) {
        auto made = SumAgg::bind(bind);
        if (!made) return dstd::Unexpected(std::move(made.error()));
        return dstd::make_unique<SumNamed>();
    }
};

struct BothUdaf : SumAgg {
    static constexpr const char* NAME = "cxx_both";
    static dstd::Result<dstd::UniquePtr<BothUdaf>> bind(
        dataframe::UdfBind& bind) {
        auto made = SumAgg::bind(bind);
        if (!made) return dstd::Unexpected(std::move(made.error()));
        return dstd::make_unique<BothUdaf>();
    }
};

struct BothUdf {
    static constexpr const char* NAME = "cxx_both";
    static dstd::Result<dstd::UniquePtr<BothUdf>> bind(
        dataframe::UdfBind& bind) {
        bind.result(DFTU_TYPE_INT64);
        return dstd::make_unique<BothUdf>();
    }
    dstd::Result<dftu_series*> exec(dataframe::UdfBatch& batch) const {
        Ints out(batch.rows(), 0);
        return dftu_series_new_flat(DFTU_TYPE_INT64, out.data(),
                                    static_cast<std::int64_t>(out.size()),
                                    nullptr);
    }
};

int g_list_alive = 0;

struct ListAgg {
    static constexpr const char* NAME = "cxx_distinct";
    struct State {
        State() { ++g_list_alive; }
        State(const State&) = delete;
        ~State() { --g_list_alive; }
        Ints seen;
        void add(std::int64_t x) {
            for (const std::int64_t y : seen)
                if (y == x) return;
            seen.push_back(x);
        }
    };

    static dstd::Result<dstd::UniquePtr<ListAgg>> bind(
        dataframe::UdfBind& bind) {
        bind.result(DFTU_TYPE_INT64);
        return dstd::make_unique<ListAgg>();
    }
    dstd::Result<void> update(dataframe::UdafStateSet<State>& states,
                              const dataframe::UdfBatch& batch) const {
        const auto* in =
            static_cast<const std::int64_t*>(dftu_series_data(batch.arg(0)));
        for (std::uint64_t i = 0; i < states.size(); ++i)
            if (State* s = states[i]) s->add(in[i]);
        return {};
    }
    dstd::Result<void> combine(State& into, const State& from) const {
        for (const std::int64_t x : from.seen) into.add(x);
        return {};
    }
    dstd::Result<dftu_series*> finalize(
        dataframe::UdafStateSet<State>& states) const {
        Ints out;
        for (std::uint64_t i = 0; i < states.size(); ++i)
            out.push_back(static_cast<std::int64_t>(states[i]->seen.size()));
        return dftu_series_new_flat(DFTU_TYPE_INT64, out.data(),
                                    static_cast<std::int64_t>(out.size()),
                                    nullptr);
    }
    void serialize(const State& s, dstd::String& out) const {
        out.append(reinterpret_cast<const char*>(s.seen.data()),
                   s.seen.size() * sizeof(std::int64_t));
    }
    dstd::Result<void> deserialize(State& s, dstd::StringView in) const {
        if (in.size() % sizeof(std::int64_t) != 0)
            return common::make_error<"bad bytes">(
                common::ErrorCode::InvalidArgument);
        for (std::size_t i = 0; i < in.size(); i += sizeof(std::int64_t)) {
            std::int64_t x;
            std::memcpy(&x, in.data() + i, sizeof(x));
            s.add(x);
        }
        return {};
    }
};

}  // namespace

TEST_CASE("a plugin UDAF is registered while the plugin is loaded") {
    {
        auto set = load(UDAF_PLUGIN_PATH);
        for (const char* name :
             {"usum", "udistinct", "ucountall", "ufail", "ubadrows", "ualign32",
              "ualign16", "ufailcombine"})
            CHECK(dataframe::find_udaf(name) != nullptr);
        CHECK(dataframe::udaf_names().size() >= 8);
        auto handle = dataframe::udaf("usum");
        REQUIRE(handle.has_value());
        CHECK_FALSE(handle->is_owning());
        CHECK(library_mapped());
    }
    CHECK(dataframe::find_udaf("usum") == nullptr);
    CHECK_FALSE(dataframe::udaf("usum").has_value());
    CHECK_FALSE(library_mapped());
}

TEST_CASE("a refused UDAF fails the load with a message") {
    auto bad =
        plugins::host::Plugins::builder().add(UDAF_BAD_PLUGIN_PATH).build();
    REQUIRE_FALSE(bad.has_value());
    CHECK(has(bad.error().message(), "ubad"));
    CHECK(has(bad.error().message(), "serialize"));
    CHECK(dataframe::find_udaf("ubad") == nullptr);

    auto first = load(UDAF_PLUGIN_PATH);
    auto again =
        plugins::host::Plugins::builder().add(UDAF_PLUGIN_PATH).build();
    REQUIRE_FALSE(again.has_value());
    CHECK(has(again.error().message(), "usum"));
    CHECK(has(again.error().message(), "already"));
    CHECK(dataframe::find_udaf("usum") != nullptr);
}

TEST_CASE("a descriptor that does not fit is refused with the reason") {
    auto set = load(UDAF_PLUGIN_PATH);
    const dataframe::UdafRecord& good = *dataframe::find_udaf("usum");
    dftu_udaf_desc d = good.desc();

    d.name = "sum";
    auto r = dataframe::add_udaf(d, nullptr, nullptr);
    REQUIRE_FALSE(r.has_value());
    CHECK(has(r.error().message(), "built-in"));

    d.name = "bad name";
    CHECK_FALSE(dataframe::add_udaf(d, nullptr, nullptr).has_value());

    d.name = "zero_size";
    d.state_size = 0;
    r = dataframe::add_udaf(d, nullptr, nullptr);
    REQUIRE_FALSE(r.has_value());
    CHECK(has(r.error().message(), "state_size"));

    d = good.desc();
    d.name = "odd_align";
    d.state_align = 3;
    r = dataframe::add_udaf(d, nullptr, nullptr);
    REQUIRE_FALSE(r.has_value());
    CHECK(has(r.error().message(), "state_align"));

    d = good.desc();
    d.name = "no_update";
    d.update = nullptr;
    r = dataframe::add_udaf(d, nullptr, nullptr);
    REQUIRE_FALSE(r.has_value());
    CHECK(has(r.error().message(), "update"));

    d = good.desc();
    d.name = "retract_alone";
    d.flags |= DFTU_AGG_HAS_RETRACT;
    r = dataframe::add_udaf(d, nullptr, nullptr);
    REQUIRE_FALSE(r.has_value());
    CHECK(has(r.error().message(), "retract"));

    d = good.desc();
    d.name = "unknown_flag";
    d.flags |= 1u << 20;
    CHECK_FALSE(dataframe::add_udaf(d, nullptr, nullptr).has_value());

    CHECK(dataframe::find_udaf("zero_size") == nullptr);
    CHECK(dataframe::find_udaf("retract_alone") == nullptr);
}

TEST_CASE("update adds rows to their groups and skips null rows") {
    auto set = load(UDAF_PLUGIN_PATH);
    auto t = table_of("usum", TypeId::Int64);
    REQUIRE(t.has_value());
    CHECK(t->result_type().id == TypeId::Int64);
    REQUIRE(t->grow(3).has_value());
    CHECK(t->group_count() == 3);

    // Every third row is null: 30 and 60 are skipped.
    const Series col = ints({10, 20, 30, 40, 50, 60}, 3);
    REQUIRE(feed(*t, {0, 1, 2, 0, 1, 2}, col).has_value());
    auto out = t->finalize();
    REQUIRE(out.has_value());
    CHECK(column(*out) == Ints{50, 70, 0});

    // A negative id skips the row.
    REQUIRE(
        feed(*t, {-1, 0, -1, 0, -1, 0}, ints({5, 6, 7, 8, 9, 10})).has_value());
    out = t->finalize();
    REQUIRE(out.has_value());
    CHECK(column(*out) == Ints{50 + 6 + 8 + 10, 70, 0});

    auto beyond = feed(*t, {3}, ints({1}));
    REQUIRE_FALSE(beyond.has_value());
    CHECK(has(beyond.error().message(), "grow"));
}

TEST_CASE("a null aware UDAF sees every row") {
    auto set = load(UDAF_PLUGIN_PATH);
    auto all = table_of("ucountall", TypeId::Int64);
    auto sum = table_of("usum", TypeId::Int64);
    REQUIRE(all.has_value());
    REQUIRE(sum.has_value());
    REQUIRE(all->grow(1).has_value());
    REQUIRE(sum->grow(1).has_value());
    const Series col = ints({1, 2, 3, 4, 5, 6, 7, 8}, 2);
    REQUIRE(feed(*all, Ints(8, 0), col).has_value());
    REQUIRE(feed(*sum, Ints(8, 0), col).has_value());
    CHECK(column(*all->finalize()) == Ints{8});
    CHECK(column(*sum->finalize()) == Ints{1 + 3 + 5 + 7});
}

TEST_CASE("states keep their value while the table grows past one block") {
    auto set = load(UDAF_PLUGIN_PATH);
    auto t = table_of("usum", TypeId::Int64);
    REQUIRE(t.has_value());
    const std::int64_t first = 5000;
    REQUIRE(t->grow(10).has_value());
    REQUIRE(feed(*t, {0, 9}, ints({7, 9})).has_value());
    const std::size_t small = t->approx_bytes();

    REQUIRE(t->grow(first).has_value());
    CHECK(t->approx_bytes() > small);
    REQUIRE(t->grow(100).has_value());
    CHECK(t->group_count() == first);
    Ints gids;
    Ints values;
    for (std::int64_t g = 0; g < first; ++g) {
        gids.push_back(g);
        values.push_back(g);
    }
    REQUIRE(feed(*t, gids, ints(values)).has_value());
    auto out = t->finalize();
    REQUIRE(out.has_value());
    REQUIRE(out->length() == first);
    CHECK(out->data<std::int64_t>()[0] == 7);
    CHECK(out->data<std::int64_t>()[9] == 9 + 9);
    CHECK(out->data<std::int64_t>()[4999] == 4999);
}

TEST_CASE("a table of one group does not hold a whole block") {
    auto set = load(UDAF_PLUGIN_PATH);
    auto t = table_of("usum", TypeId::Int64);
    REQUIRE(t.has_value());
    REQUIRE(t->grow(1).has_value());
    CHECK(t->approx_bytes() < 1024);

    // Growing a group at a time crosses from the small first block into full
    // blocks with every state where it was.
    constexpr std::int64_t GROUPS = 3000;
    for (std::int64_t g = 1; g <= GROUPS; ++g) {
        REQUIRE(t->grow(g).has_value());
        REQUIRE(feed(*t, {g - 1}, ints({g})).has_value());
    }
    auto out = t->finalize();
    REQUIRE(out.has_value());
    REQUIRE(out->length() == GROUPS);
    for (std::int64_t g = 0; g < GROUPS; ++g)
        CHECK(out->data<std::int64_t>()[g] == g + 1);
}

TEST_CASE("combine_from merges by a group mapping and empties the source") {
    auto set = load(UDAF_PLUGIN_PATH);
    auto a = table_of("udistinct", TypeId::Int64);
    auto b = table_of("udistinct", TypeId::Int64);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(a->grow(3).has_value());
    REQUIRE(b->grow(2).has_value());
    REQUIRE(feed(*a, {0, 0, 1, 2}, ints({1, 2, 3, 4})).has_value());
    REQUIRE(feed(*b, {0, 0, 1}, ints({2, 5, 4})).has_value());

    const std::int64_t before = counter("udaf_plugin_destroys");
    // b0 = {2, 5} into a2 = {4}; b1 = {4} into a0 = {1, 2}.
    REQUIRE(a->combine_from(*b, Ints{2, 0}).has_value());
    CHECK(b->group_count() == 0);
    CHECK(counter("udaf_plugin_destroys") - before == 2);
    CHECK(column(*a->finalize()) == Ints{3, 1, 3});

    CHECK_FALSE(a->combine_from(*b, Ints{0}).has_value());
    auto c = table_of("usum", TypeId::Int64);
    REQUIRE(c.has_value());
    CHECK_FALSE(a->combine_from(*c, Ints{}).has_value());
    CHECK_FALSE(a->combine_from(*a, Ints{0, 1, 2}).has_value());
}

TEST_CASE("serialize and deserialize move the states to a fresh table") {
    auto set = load(UDAF_PLUGIN_PATH);
    for (const char* name : {"usum", "udistinct"}) {
        INFO(name);
        auto from = table_of(name, TypeId::Int64);
        auto to = table_of(name, TypeId::Int64);
        REQUIRE(from.has_value());
        REQUIRE(to.has_value());
        REQUIRE(from->grow(3).has_value());
        REQUIRE(to->grow(3).has_value());
        REQUIRE(
            feed(*from, {0, 0, 2, 2, 2}, ints({4, 4, 7, 8, 7})).has_value());

        for (std::int64_t g = 0; g < 3; ++g) {
            dstd::String bytes;
            REQUIRE(from->serialize(g, bytes).has_value());
            REQUIRE(to->deserialize(g, bytes).has_value());
        }
        CHECK(column(*to->finalize()) == column(*from->finalize()));
    }
    auto pod = table_of("usum", TypeId::Int64);
    REQUIRE(pod.has_value());
    REQUIRE(pod->grow(1).has_value());
    dstd::String bytes = "abc";
    CHECK_FALSE(pod->deserialize(0, bytes).has_value());
    CHECK_FALSE(pod->serialize(1, bytes).has_value());
    CHECK(bytes == "abc");
}

TEST_CASE("every state init made is destroyed and the bound is freed") {
    auto set = load(UDAF_PLUGIN_PATH);
    const std::int64_t inits = counter("udaf_plugin_inits");
    const std::int64_t destroys = counter("udaf_plugin_destroys");
    const std::int64_t binds = counter("udaf_plugin_binds");
    const std::int64_t frees = counter("udaf_plugin_frees");
    {
        auto a = table_of("udistinct", TypeId::Int64);
        auto b = table_of("udistinct", TypeId::Int64);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        REQUIRE(a->grow(40).has_value());
        REQUIRE(b->grow(1500).has_value());
        REQUIRE(feed(*a, {3, 3}, ints({1, 2})).has_value());
        CHECK(counter("udaf_plugin_inits") - inits == 1540);
        // The table that moves keeps the states, and the moved-from one has
        // none.
        UdafStates moved(std::move(*a));
        CHECK(a->group_count() == 0);
        CHECK(moved.group_count() == 40);
    }
    CHECK(counter("udaf_plugin_destroys") - destroys == 1540);
    CHECK(counter("udaf_plugin_binds") - binds == 2);
    CHECK(counter("udaf_plugin_frees") - frees == 2);
}

TEST_CASE("a failing callback reports its message") {
    auto set = load(UDAF_PLUGIN_PATH);
    auto fail = table_of("ufail", TypeId::Int64);
    REQUIRE(fail.has_value());
    REQUIRE(fail->grow(1).has_value());
    auto updated = feed(*fail, {0}, ints({1}));
    REQUIRE_FALSE(updated.has_value());
    CHECK(has(updated.error().message(), "ufail"));
    CHECK(has(updated.error().message(), "refused on purpose"));
    CHECK(has(updated.error().message(), "update"));

    auto rows = table_of("ubadrows", TypeId::Int64);
    REQUIRE(rows.has_value());
    REQUIRE(rows->grow(2).has_value());
    auto out = rows->finalize();
    REQUIRE_FALSE(out.has_value());
    CHECK(has(out.error().message(), "ubadrows"));
    CHECK(has(out.error().message(), "rows"));

    auto refused = table_of("udistinct", TypeId::Float64);
    REQUIRE_FALSE(refused.has_value());
    CHECK(has(refused.error().message(), "argument type"));
    CHECK(has(refused.error().message(), "udistinct"));

    auto record = dataframe::find_udaf("usum");
    const dataframe::DataType two[] = {dataframe::scalar(TypeId::Int64),
                                       dataframe::scalar(TypeId::Int64)};
    auto arity = dataframe::bind_udaf(std::move(record), two, {});
    REQUIRE_FALSE(arity.has_value());
    CHECK(has(arity.error().message(), "arguments"));
}

TEST_CASE("a state table keeps the plugin library loaded") {
    dstd::Optional<UdafStates> t;
    {
        auto set = load(UDAF_PLUGIN_PATH);
        auto made = table_of("usum", TypeId::Int64);
        REQUIRE(made.has_value());
        t.emplace(std::move(*made));
        REQUIRE(t->grow(2).has_value());
    }
    CHECK(dataframe::find_udaf("usum") == nullptr);
    CHECK(library_mapped());
    REQUIRE(feed(*t, {1}, ints({3})).has_value());
    CHECK(column(*t->finalize()) == Ints{0, 3});
    t.reset();
    CHECK_FALSE(library_mapped());
}

TEST_CASE("make_udaf<T> registers an in-process UDAF for the handle's life") {
    dstd::Optional<UdafStates> t;
    {
        auto reg = dataframe::make_udaf<SumAgg>();
        REQUIRE(reg.has_value());
        CHECK(reg->is_owning());
        CHECK(reg->name() == "cxx_sum");
        auto looked = dataframe::udaf("cxx_sum");
        REQUIRE(looked.has_value());
        CHECK_FALSE(looked->is_owning());

        auto again = dataframe::make_udaf<SumAgg>();
        REQUIRE_FALSE(again.has_value());
        CHECK(has(again.error().message(), "already"));
        auto builtin = dataframe::make_udaf<SumNamed<true>>();
        REQUIRE_FALSE(builtin.has_value());
        CHECK(has(builtin.error().message(), "built-in"));

        auto made = table_of("cxx_sum", TypeId::Int64);
        REQUIRE(made.has_value());
        t.emplace(std::move(*made));
        REQUIRE(t->grow(2).has_value());
        REQUIRE(feed(*t, {0, 1, 0}, ints({1, 2, 3}, 3)).has_value());
        dataframe::Udaf moved(std::move(*reg));
        CHECK(reg->name().empty());
        CHECK(dataframe::find_udaf("cxx_sum") != nullptr);
    }
    CHECK(dataframe::find_udaf("cxx_sum") == nullptr);
    auto gone = dataframe::udaf("cxx_sum");
    REQUIRE_FALSE(gone.has_value());
    CHECK(gone.error().condition() == dstd::Condition::NotFound);
    // The table made before still runs: the record outlives the name.
    CHECK(column(*t->finalize()) == Ints{1, 2});
}

TEST_CASE("an in-process UDAF with a heap state is destroyed and round trips") {
    auto reg = dataframe::make_udaf<ListAgg>();
    REQUIRE(reg.has_value());
    {
        auto a = table_of("cxx_distinct", TypeId::Int64);
        auto b = table_of("cxx_distinct", TypeId::Int64);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        REQUIRE(a->grow(2).has_value());
        REQUIRE(b->grow(2).has_value());
        CHECK(g_list_alive == 4);
        REQUIRE(feed(*a, {0, 0, 1}, ints({1, 1, 2})).has_value());
        REQUIRE(feed(*b, {0, 1, 1}, ints({1, 3, 4})).has_value());

        dstd::String bytes;
        REQUIRE(a->serialize(0, bytes).has_value());
        CHECK(bytes.size() == sizeof(std::int64_t));
        REQUIRE(b->deserialize(0, bytes).has_value());
        CHECK(column(*b->finalize()) == Ints{1, 2});

        REQUIRE(a->combine_from(*b, Ints{1, 0}).has_value());
        CHECK(g_list_alive == 2);
        CHECK(column(*a->finalize()) == Ints{3, 2});
    }
    CHECK(g_list_alive == 0);
}

TEST_CASE("a UDF and a UDAF cannot share a name, in either order") {
    {
        auto udaf = dataframe::make_udaf<BothUdaf>();
        REQUIRE(udaf.has_value());
        auto udf = dataframe::make_udf<BothUdf>();
        REQUIRE_FALSE(udf.has_value());
        CHECK(has(udf.error().message(), "cxx_both"));
        CHECK(has(udf.error().message(), "udaf"));
        CHECK(dataframe::find_udf("cxx_both") == nullptr);
    }
    {
        auto udf = dataframe::make_udf<BothUdf>();
        REQUIRE(udf.has_value());
        auto udaf = dataframe::make_udaf<BothUdaf>();
        REQUIRE_FALSE(udaf.has_value());
        CHECK(has(udaf.error().message(), "cxx_both"));
        CHECK(has(udaf.error().message(), "udf"));
        CHECK(dataframe::find_udaf("cxx_both") == nullptr);
    }
    CHECK(dataframe::find_udf("cxx_both") == nullptr);
}

#ifdef STATS_UDAFS_PLUGIN_PATH
TEST_CASE("the SDK plugin registers UDAFs through PluginBuilder::udaf") {
    auto set = load(STATS_UDAFS_PLUGIN_PATH);
    REQUIRE(dataframe::find_udaf("geomean") != nullptr);
    REQUIRE(dataframe::find_udaf("p50") != nullptr);
    CHECK(dataframe::find_udaf("geomean")->is_pod());
    CHECK_FALSE(dataframe::find_udaf("p50")->is_pod());
    CHECK(dataframe::find_udaf("p50")->is_parallel_safe());

    const Series col = ints({1, 100, 3, 4, 7, 9, 50, 2}, 8);
    auto p50 = table_of("p50", TypeId::Int64);
    auto geo = table_of("geomean", TypeId::Int64);
    REQUIRE(p50.has_value());
    REQUIRE(geo.has_value());
    CHECK(p50->result_type().id == TypeId::Float64);
    REQUIRE(p50->grow(2).has_value());
    REQUIRE(geo->grow(2).has_value());
    // Group 0 gets 1 3 7 50, group 1 gets 100 4 9 and a null row, which is
    // skipped.
    const Ints gids{0, 1, 0, 1, 0, 1, 0, 1};
    REQUIRE(feed(*p50, gids, col).has_value());
    REQUIRE(feed(*geo, gids, ints({2, 8, 8, 2, 4, 4, 1, 1})).has_value());

    auto med = p50->finalize();
    REQUIRE(med.has_value());
    CHECK(med->data<double>()[0] == 7);
    CHECK(med->data<double>()[1] == 9);
    auto mean = geo->finalize();
    REQUIRE(mean.has_value());
    CHECK(mean->data<double>()[0] == doctest::Approx(2.8284271247));
    CHECK(mean->data<double>()[1] == doctest::Approx(2.8284271247));

    dstd::String bytes;
    REQUIRE(p50->serialize(0, bytes).has_value());
    CHECK(bytes.size() == 4 * sizeof(double));
    auto fresh = table_of("p50", TypeId::Int64);
    REQUIRE(fresh.has_value());
    REQUIRE(fresh->grow(1).has_value());
    REQUIRE(fresh->deserialize(0, bytes).has_value());
    CHECK(fresh->finalize()->data<double>()[0] == 7);
}
#endif

TEST_CASE("an op cannot take the name of a registered UDAF") {
    auto reg = dataframe::make_udaf<SumAgg>();
    REQUIRE(reg.has_value());
    const dftu_op_desc op{"cxx_sum", DFTU_OP_SIG(SERIES, SERIES, NONE, NONE),
                          nullptr};
    CHECK(dftu_op_register(&op) != 0);
    CHECK(dftu_op_find("cxx_sum") == nullptr);
}

TEST_CASE("states are aligned although the size is no multiple of the align") {
    auto set = load(UDAF_PLUGIN_PATH);
    const std::int64_t before = counter("udaf_plugin_misaligned");
    for (const char* name : {"ualign32", "ualign16"}) {
        INFO(name);
        auto t = table_of(name, TypeId::Int64);
        REQUIRE(t.has_value());
        REQUIRE(t->grow(1).has_value());
        REQUIRE(t->grow(3000).has_value());
        Ints gids;
        Ints values;
        for (std::int64_t g = 0; g < 3000; ++g) {
            gids.push_back(g);
            values.push_back(g);
        }
        REQUIRE(feed(*t, gids, ints(values)).has_value());
        auto out = t->finalize();
        REQUIRE(out.has_value());
        CHECK(column(*out) == Ints(3000, 1));
    }
    CHECK(counter("udaf_plugin_misaligned") == before);
}

TEST_CASE(
    "a failing combine reports its message and leaves the source intact") {
    auto set = load(UDAF_PLUGIN_PATH);
    auto a = table_of("ufailcombine", TypeId::Int64);
    auto b = table_of("ufailcombine", TypeId::Int64);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(a->grow(2).has_value());
    REQUIRE(b->grow(2).has_value());
    REQUIRE(feed(*b, {0, 1, 1}, ints({1, 2, 3})).has_value());

    auto merged = a->combine_from(*b, Ints{0, 1});
    REQUIRE_FALSE(merged.has_value());
    CHECK(has(merged.error().message(), "ufailcombine"));
    CHECK(has(merged.error().message(), "refused merge"));
    CHECK(has(merged.error().message(), "combine"));
    CHECK(b->group_count() == 2);
    CHECK(column(*b->finalize()) == Ints{1, 2});
    CHECK(column(*a->finalize()) == Ints{0, 0});
}

TEST_CASE("a failing deserialize reports its message and leaks nothing") {
    auto set = load(UDAF_PLUGIN_PATH);
    const std::int64_t inits = counter("udaf_plugin_inits");
    const std::int64_t destroys = counter("udaf_plugin_destroys");
    {
        auto t = table_of("udistinct", TypeId::Int64);
        REQUIRE(t.has_value());
        REQUIRE(t->grow(2).has_value());
        auto bad = t->deserialize(1, dstd::StringView("abc", 3));
        REQUIRE_FALSE(bad.has_value());
        CHECK(has(bad.error().message(), "udistinct"));
        CHECK(has(bad.error().message(), "bad state bytes"));
        CHECK(has(bad.error().message(), "deserialize"));
        REQUIRE(feed(*t, {1}, ints({5})).has_value());
        CHECK(column(*t->finalize()) == Ints{0, 1});
    }
    CHECK(counter("udaf_plugin_inits") - inits == 2);
    CHECK(counter("udaf_plugin_destroys") - destroys == 2);
}
