// SPDX-License-Identifier: MIT
// The UDF and UDAF adapters (dataframe/udf_adapter.h, udaf_adapter.h): a UDF
// whose exec gives a Result<Column> and a UDAF whose finalize gives one reach
// the C ABI through the trampolines, and the flag enums carry the ABI bits.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/c_api.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/plugin_frame.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dataframe/udaf_adapter.h>
#include <dftracer/utils/dataframe/udf_adapter.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

dftu_type_info type_info(TypeId id) {
    dftu_type_info t{};
    t.struct_size = sizeof(dftu_type_info);
    t.id = static_cast<dftu_dtype>(id);
    return t;
}

/// The row values of a column handle, for a test to compare.
dstd::Vector<std::int64_t> rows_of(const dftu_series* series) {
    dstd::Vector<std::int64_t> out;
    const auto* data =
        static_cast<const std::int64_t*>(dftu_series_data(series));
    for (std::int64_t i = 0; i < dftu_series_length(series); ++i)
        out.push_back(data[i]);
    return out;
}

/// A UDF that adds one to every row of an Int64 column.
struct PlusOne {
    static dstd::Result<dstd::UniquePtr<PlusOne>> bind(UdfBind& bind) {
        if (bind.type(0) != TypeId::Int64) {
            return dstd::Unexpected<dstd::Error>(dstd::Error(
                dstd::Condition::InvalidArgument, "plus_one takes an Int64"));
        }
        bind.result(TypeId::Int64);
        return dstd::make_unique<PlusOne>();
    }

    dstd::Result<Column> exec(UdfBatch& batch) const {
        const dstd::Span<const std::int64_t> in =
            batch.arg(0).values<std::int64_t>();
        dstd::Vector<std::int64_t> out;
        for (std::uint64_t i = 0; i < batch.rows(); ++i)
            out.push_back(in[static_cast<std::size_t>(i)] + 1);
        return Column::from_int64(std::move(out));
    }
};

/// A UDAF with a plain-bytes state that sums its rows.
struct Sum {
    struct State {
        std::int64_t total = 0;
    };

    static dstd::Result<dstd::UniquePtr<Sum>> bind(UdfBind& bind) {
        bind.result(TypeId::Int64);
        return dstd::make_unique<Sum>();
    }
    dstd::Result<void> update(UdafStateSet<State>& states,
                              const UdfBatch& batch) const {
        const dstd::Span<const std::int64_t> in =
            batch.arg(0).values<std::int64_t>();
        for (std::uint64_t i = 0; i < states.size(); ++i) {
            if (State* s = states[i]) {
                s->total += in[static_cast<std::size_t>(i)];
            }
        }
        return {};
    }
    dstd::Result<void> combine(State& into, const State& from) const {
        into.total += from.total;
        return {};
    }
    dstd::Result<Column> finalize(UdafStateSet<State>& states) const {
        dstd::Vector<std::int64_t> out;
        for (std::uint64_t i = 0; i < states.size(); ++i)
            out.push_back(states[i]->total);
        return Column::from_int64(std::move(out));
    }
};

/// A UDF whose exec fails: the error reaches the host as a status.
struct Fails {
    static dstd::Result<dstd::UniquePtr<Fails>> bind(UdfBind& bind) {
        bind.result(TypeId::Int64);
        return dstd::make_unique<Fails>();
    }
    dstd::Result<Column> exec(UdfBatch&) const {
        return dstd::Unexpected<dstd::Error>(dstd::Error(
            dstd::Condition::InvalidArgument, "the batch was refused"));
    }
};

/// A host context that keeps the status and the message a callback reports, so
/// a test reads the text the host would see.
struct Captured {
    dstd::String message;
    dftu_status status = DFTU_OK;
    dftu_ctx ctx{};

    static dftu_status record(dftu_ctx* c, dftu_status status, const char* msg,
                              std::uint32_t len) {
        auto* self = static_cast<Captured*>(c->h);
        self->message.assign(msg, len);
        self->status = status;
        return status;
    }
    static dftu_status running(dftu_ctx*) { return DFTU_OK; }

    static constexpr dftu_ctx_vt VT{sizeof(dftu_ctx_vt), &record, &running,
                                    nullptr};

    Captured() {
        ctx.struct_size = sizeof(dftu_ctx);
        ctx.vt = &VT;
        ctx.h = this;
    }
};

}  // namespace

TEST_CASE("a UDF adapter runs exec and gives its column to the host") {
    static_assert(UdfType<PlusOne>);
    const dftu_udf_desc desc =
        detail::udf_desc_of<PlusOne>("plus_one", UdfOptions{});
    CHECK(desc.flags == 0u);
    CHECK(desc.min_args == 1);
    CHECK(desc.max_args == 1);

    dftu_type_info arg = type_info(TypeId::Int64);
    dftu_type_info out{};
    void* bound = nullptr;
    REQUIRE(desc.bind(nullptr, nullptr, &arg, 1, nullptr, &out, &bound) ==
            DFTU_OK);
    CHECK(static_cast<TypeId>(out.id) == TypeId::Int64);
    CHECK(bound != nullptr);

    dstd::Vector<std::int64_t> values;
    values.push_back(1);
    values.push_back(2);
    const Column input = Column::from_int64(std::move(values));
    const dftu_series* argv[1] = {input.get()};
    dftu_series* result = nullptr;
    REQUIRE(desc.exec(nullptr, bound, argv, 1, 2, &result) == DFTU_OK);
    REQUIRE(result != nullptr);
    const Column owned(result);
    const dstd::Vector<std::int64_t> got = rows_of(owned.get());
    REQUIRE(got.size() == 2);
    CHECK(got[0] == 2);
    CHECK(got[1] == 3);
    desc.free_bound(bound);
}

TEST_CASE("a UDAF adapter finalizes a Result<Column> per state") {
    static_assert(UdafType<Sum>);
    const dftu_udaf_desc desc =
        detail::udaf_desc_of<Sum>("sum", UdafOptions{UdafFlag::ParallelSafe});
    CHECK(desc.flags == (static_cast<std::uint32_t>(DFTU_AGG_PARALLEL_SAFE) |
                         static_cast<std::uint32_t>(DFTU_AGG_STATE_POD)));
    CHECK(desc.state_size == sizeof(Sum::State));

    dftu_type_info arg = type_info(TypeId::Int64);
    dftu_type_info out{};
    void* bound = nullptr;
    REQUIRE(desc.bind(nullptr, nullptr, &arg, 1, nullptr, &out, &bound) ==
            DFTU_OK);

    Sum::State states[2];
    void* state_ptrs[2] = {&states[0], &states[1]};
    desc.init(nullptr, state_ptrs, 2);

    dstd::Vector<std::int64_t> values;
    values.push_back(3);
    values.push_back(4);
    const Column input = Column::from_int64(std::move(values));
    const dftu_series* argv[1] = {input.get()};
    REQUIRE(desc.update(nullptr, bound, state_ptrs, argv, 1, 2) == DFTU_OK);
    REQUIRE(desc.combine(nullptr, bound, &state_ptrs[0], &state_ptrs[1], 1) ==
            DFTU_OK);

    dftu_series* result = nullptr;
    REQUIRE(desc.finalize(nullptr, bound, &state_ptrs[0], 1, &result) ==
            DFTU_OK);
    REQUIRE(result != nullptr);
    const Column owned(result);
    const dstd::Vector<std::int64_t> got = rows_of(owned.get());
    REQUIRE(got.size() == 1);
    CHECK(got[0] == 7);
    desc.free_bound(bound);
}

TEST_CASE("a failing exec reports its error and its message as a status") {
    const dftu_udf_desc desc =
        detail::udf_desc_of<Fails>("fails", UdfOptions{});
    Captured host;
    dftu_type_info arg = type_info(TypeId::Int64);
    dftu_type_info out{};
    void* bound = nullptr;
    REQUIRE(desc.bind(nullptr, nullptr, &arg, 1, nullptr, &out, &bound) ==
            DFTU_OK);
    dstd::Vector<std::int64_t> values;
    values.push_back(1);
    const Column input = Column::from_int64(std::move(values));
    const dftu_series* argv[1] = {input.get()};
    dftu_series* result = nullptr;
    const dftu_status status = desc.exec(&host.ctx, bound, argv, 1, 1, &result);
    CHECK(status < 0);
    // The condition of the error, and the text the plugin wrote, both arrive.
    CHECK(status ==
          -static_cast<dftu_status>(dstd::Condition::InvalidArgument) - 1);
    CHECK(host.status == status);
    CHECK(host.message == dstd::String("the batch was refused"));
    CHECK(result == nullptr);
    desc.free_bound(bound);
}

TEST_CASE("the flag helpers read a flag set") {
    CHECK(dataframe::any(UdfFlag::Volatile | UdfFlag::NullAware));
    CHECK(dataframe::has_flag(UdfFlag::Volatile | UdfFlag::NullAware,
                              UdfFlag::NullAware));
    CHECK_FALSE(dataframe::has_flag(UdfFlag::Volatile, UdfFlag::NullAware));
    CHECK_FALSE(dataframe::any(UdfFlag::None));
    CHECK(dataframe::has_flag(UdafFlag::ParallelSafe | UdafFlag::NullAware,
                              UdafFlag::ParallelSafe));
    CHECK_FALSE(dataframe::any(UdafFlag::None));
}

TEST_CASE("UdfBind::constant gives a ScalarView of every kind") {
    dftu_type_info args[5] = {
        type_info(TypeId::Int64), type_info(TypeId::Uint64),
        type_info(TypeId::Float64), type_info(TypeId::String),
        type_info(TypeId::Int64)};
    const dftu_scalar signed_one = i64(-7);
    const dftu_scalar unsigned_one = u64(7);
    const dftu_scalar floating = f64(1.5);
    const dstd::String text("abc");
    const dftu_scalar string_one = str(dstd::StringView(text));
    dftu_scalar error_one{};
    error_one.kind = DFTU_SCALAR_TAG_ERR;
    const dftu_scalar* consts[5] = {&signed_one, &unsigned_one, &floating,
                                    &string_one, &error_one};
    dftu_type_info out{};
    const UdfBind bind(dstd::Span<const dftu_type_info>(args),
                       dstd::Span<const dftu_scalar* const>(consts), &out);
    REQUIRE(bind.size() == 5);
    CHECK(bind.type(2) == TypeId::Float64);

    const dstd::Optional<ScalarView> i = bind.constant(0);
    REQUIRE(i.has_value());
    CHECK(i->kind() == ScalarKind::I64);
    CHECK(i->i64() == -7);
    CHECK(i->f64() == -7.0);
    CHECK(i->boolean());
    CHECK_FALSE(i->is_str());
    CHECK(i->str().empty());

    const dstd::Optional<ScalarView> u = bind.constant(1);
    REQUIRE(u.has_value());
    CHECK(u->kind() == ScalarKind::U64);
    CHECK(u->u64() == 7);
    CHECK(u->as<std::int64_t>() == 7);

    const dstd::Optional<ScalarView> d = bind.constant(2);
    REQUIRE(d.has_value());
    CHECK(d->kind() == ScalarKind::F64);
    CHECK(d->f64() == 1.5);
    // A float read as an integer truncates, and is true as a Bool.
    CHECK(d->i64() == 1);
    CHECK(d->boolean());

    const dstd::Optional<ScalarView> s = bind.constant(3);
    REQUIRE(s.has_value());
    CHECK(s->kind() == ScalarKind::Str);
    CHECK(s->is_str());
    CHECK(s->str() == dstd::StringView("abc"));
    // Text has no number, and gives zero rather than a reinterpreted pointer.
    CHECK(s->i64() == 0);
    CHECK(s->f64() == 0.0);

    const dstd::Optional<ScalarView> e = bind.constant(4);
    REQUIRE(e.has_value());
    CHECK(e->kind() == ScalarKind::Err);
    CHECK_FALSE(e->is_str());
    CHECK(e->i64() == 0);

    // No constant at an index of a call that writes none, or past the end.
    CHECK_FALSE(bind.constant(5).has_value());
    const UdfBind literal(dstd::Span<const dftu_type_info>(args),
                          dstd::Span<const dftu_scalar* const>(), &out);
    CHECK_FALSE(literal.constant(0).has_value());
}
