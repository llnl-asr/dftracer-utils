// SPDX-License-Identifier: MIT
// The two builds of the string-match kernels (plain and jcc) give the same
// masks as a per-row reference, on flat, chunked and dictionary columns, and
// the choice between them follows the request and the CPU table.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/cpu_features.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/kernels/string_match.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/pattern_engine.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>

#include <cstdint>
#include <cstdlib>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

using common::jcc_erratum_affected;
using common::JccKernels;
using common::parse_jcc_kernels;
using common::prefers_jcc_kernels;
using dataframe::kernels::resolve_string_match;
using dataframe::kernels::string_match_variant;
using dataframe::kernels::StringMatch;

namespace {

using Row = dstd::Optional<dstd::String>;
using Rows = dstd::Vector<Row>;

Rows make_rows() {
    const dstd::String long_a(70, 'a');
    const dstd::String long_b(130, 'q');
    const dstd::String pool[] = {
        "read",
        "write",
        "MPI_Send",
        "",
        "a_write",
        "cudaMemcpyAsync",
        "pxexd",
        "p e d",
        "H5Dwrite",
        "pread64",
        dstd::fmt<"{}ped">(long_a),
        dstd::fmt<"{}read">(long_b),
        dstd::fmt<"read{}">(long_b),
        dstd::fmt<"{}p{}e{}d{}">(long_a, long_a, long_a, long_a)};
    Rows rows;
    for (std::int64_t i = 0; i < 210; ++i) {
        if (i % 7 == 3)
            rows.emplace_back();
        else
            rows.emplace_back(pool[static_cast<std::size_t>(i * 5 + i / 11) %
                                   (sizeof(pool) / sizeof(pool[0]))]);
    }
    return rows;
}

Series flat_of(const Rows& rows, std::size_t from, std::size_t to) {
    dstd::Vector<std::int32_t> off{0};
    dstd::String data;
    dstd::Vector<std::uint8_t> valid((to - from + 7) / 8, 0);
    for (std::size_t i = from; i < to; ++i) {
        if (rows[i]) {
            data += *rows[i];
            valid[(i - from) >> 3] |=
                static_cast<std::uint8_t>(1u << ((i - from) & 7));
        }
        off.push_back(static_cast<std::int32_t>(data.size()));
    }
    return Series{dftu_series_new_string(
        static_cast<dftu_dtype>(TypeId::String), off.data(), data.data(),
        static_cast<std::int64_t>(to - from), valid.data())};
}

Series chunked_of(const Rows& rows) {
    const std::size_t cuts[] = {0, 1, 60, 61, 150, rows.size()};
    dstd::Vector<dstd::SharedPtr<dftu_series>> chunks;
    for (int k = 0; k < 5; ++k) {
        Series p = flat_of(rows, cuts[k], cuts[k + 1]);
        chunks.push_back(dstd::make_shared<dftu_series>(*p.handle()));
    }
    return Series{make_chunked(std::move(chunks))};
}

struct Column {
    const char* name;
    Series series;
};

dstd::Vector<Column> columns(const Rows& rows) {
    dstd::Vector<Column> out;
    out.push_back({"flat", flat_of(rows, 0, rows.size())});
    out.push_back({"chunked", chunked_of(rows)});
    out.push_back(
        {"dictionary", flat_of(rows, 0, rows.size()).dictionary_encode()});
    return out;
}

// The mask equals `expect(row)` on every non-null row and is null on every
// null row, with the null count the input has.
template <class Pred>
void check_mask(dftu_series* got, const Rows& rows, Pred expect) {
    REQUIRE(got != nullptr);
    const Series m = Series{got}.materialize();
    REQUIRE(m.length() == static_cast<std::int64_t>(rows.size()));
    std::int64_t nulls = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto k = static_cast<std::int64_t>(i);
        REQUIRE(m.is_null(k) == !rows[i].has_value());
        if (!rows[i]) {
            ++nulls;
            continue;
        }
        const bool bit = (m.data<std::uint8_t>()[k >> 3] >> (k & 7)) & 1;
        REQUIRE(bit == expect(dstd::StringView(*rows[i])));
    }
    CHECK(m.null_count() == nulls);
}

duql::PatternPtr must(duql::PatternResult r) {
    REQUIRE(r.has_value());
    return std::move(*r);
}

void check_variant(const StringMatch& k, const char* label) {
    const Rows rows = make_rows();
    const dstd::String long_a(70, 'a');
    const struct {
        const char* like;
    } likes[] = {{"read"},   {"MPI_%"},  {"%write"}, {"%Memcpy%"}, {"%p%e%d%"},
                 {"_write"}, {"%a%a_%"}, {""},       {"%"}};
    for (const Column& c : columns(rows)) {
        CAPTURE(label);
        CAPTURE(c.name);
        for (const auto& l : likes) {
            CAPTURE(l.like);
            const auto p = must(duql::compile_like(l.like, false, '\\'));
            check_mask(k.pattern(c.series.handle(), *p), rows,
                       [&](dstd::StringView s) {
                           return duql::match(*p, s) == duql::MatchResult::Yes;
                       });
        }
        for (const char* re : {"^MPI_.*e$", "(read|write)64?", "p.*e.*d"}) {
            CAPTURE(re);
            const auto p = must(duql::compile_regex(re, false));
            check_mask(k.pattern(c.series.handle(), *p), rows,
                       [&](dstd::StringView s) {
                           return duql::match(*p, s) == duql::MatchResult::Yes;
                       });
        }
        for (const dstd::String& lit :
             {dstd::String("read"), dstd::String(""), dstd::String("write"),
              long_a, dstd::fmt<"{}read">(dstd::String(130, 'q'))}) {
            CAPTURE(lit);
            const auto n = static_cast<std::int32_t>(lit.size());
            check_mask(k.eq(c.series.handle(), lit.data(), n), rows,
                       [&](dstd::StringView s) { return s == lit; });
            check_mask(k.contains(c.series.handle(), lit.data(), n), rows,
                       [&](dstd::StringView s) {
                           return s.find(lit) != dstd::StringView::npos;
                       });
            check_mask(k.starts_with(c.series.handle(), lit.data(), n), rows,
                       [&](dstd::StringView s) { return s.starts_with(lit); });
            check_mask(k.ends_with(c.series.handle(), lit.data(), n), rows,
                       [&](dstd::StringView s) { return s.ends_with(lit); });
        }
    }
}

}  // namespace

TEST_SUITE("string match") {
    TEST_CASE("jcc_erratum_affected follows the Intel table") {
        struct Hit {
            std::uint32_t model, stepping;
        };
        const Hit affected[] = {
            {0x4E, 3}, {0x5E, 3},   {0x55, 4},   {0x55, 7},
            {0x8E, 9}, {0x8E, 0xA}, {0x8E, 0xB}, {0x8E, 0xC},
            {0x9E, 9}, {0x9E, 0xA}, {0x9E, 0xB}, {0x9E, 0xD},
            {0xA6, 0}, {0xAE, 0xA}, {0xA5, 0},   {0xA5, 5}};
        for (const Hit& h : affected) {
            CAPTURE(h.model);
            CAPTURE(h.stepping);
            CHECK(jcc_erratum_affected(6, h.model, h.stepping));
        }
        const Hit clear[] = {{0x55, 5},   {0x55, 6}, {0x55, 0xB},
                             {0x9E, 0xC}, {0x8E, 8}, {0x6A, 6}};
        for (const Hit& h : clear) {
            CAPTURE(h.model);
            CAPTURE(h.stepping);
            CHECK_FALSE(jcc_erratum_affected(6, h.model, h.stepping));
        }
        CHECK_FALSE(jcc_erratum_affected(0x17, 0x55, 4));
        CHECK_FALSE(jcc_erratum_affected(0x19, 0x8E, 9));
    }

    TEST_CASE("the plain kernels match the per-row reference") {
        const StringMatch* plain = string_match_variant(JccKernels::Plain);
        REQUIRE(plain != nullptr);
        check_variant(*plain, "plain");
    }

    // A Linux x86-64 build (the wheels) must carry the padded copy; a
    // toolchain without the branch-padding flag would drop it silently.
    TEST_CASE("a Linux x86-64 build carries the jcc kernels") {
#if defined(__linux__) && defined(__x86_64__)
        CHECK(string_match_variant(JccKernels::Jcc) != nullptr);
#else
        MESSAGE("only a Linux x86-64 build must carry the jcc kernels");
#endif
    }

    TEST_CASE("the jcc kernels match the per-row reference when built") {
        const StringMatch* jcc = string_match_variant(JccKernels::Jcc);
        if (!jcc) {
            MESSAGE("jcc variant not built in this configuration");
            return;
        }
        check_variant(*jcc, "jcc");
    }

    TEST_CASE("a request picks its variant") {
        const StringMatch* plain = string_match_variant(JccKernels::Plain);
        const StringMatch* jcc = string_match_variant(JccKernels::Jcc);
        const StringMatch* on_affected = jcc ? jcc : plain;
        CHECK(&resolve_string_match(JccKernels::Plain, true) == plain);
        CHECK(&resolve_string_match(JccKernels::Jcc, false) == on_affected);
        CHECK(&resolve_string_match(JccKernels::Auto, true) == on_affected);
        CHECK(&resolve_string_match(JccKernels::Auto, false) == plain);
    }

    TEST_CASE("the request text names plain, jcc or auto") {
        CHECK(parse_jcc_kernels("plain") == JccKernels::Plain);
        CHECK(parse_jcc_kernels("jcc") == JccKernels::Jcc);
        CHECK(parse_jcc_kernels("auto") == JccKernels::Auto);
        CHECK(parse_jcc_kernels("") == JccKernels::Auto);
        CHECK(parse_jcc_kernels("bogus") == JccKernels::Auto);
        CHECK(prefers_jcc_kernels(JccKernels::Jcc, false));
        CHECK(prefers_jcc_kernels(JccKernels::Auto, true));
        CHECK_FALSE(prefers_jcc_kernels(JccKernels::Auto, false));
        CHECK_FALSE(prefers_jcc_kernels(JccKernels::Plain, true));
    }

    // The choice is cached on first use, so each request runs in a new process.
    TEST_CASE("the request in the environment picks the kernels" *
              doctest::skip()) {
        fresh_process::announce();
        const char* request = std::getenv("DFTRACER_UTILS_JCC_KERNELS");
        REQUIRE(request != nullptr);
        const StringMatch* want = string_match_variant(
            dstd::StringView(request) == "jcc" ? JccKernels::Jcc
                                               : JccKernels::Plain);
        if (!want) want = string_match_variant(JccKernels::Plain);
        CHECK(&dataframe::kernels::string_match() == want);
    }

    TEST_CASE("DFTRACER_UTILS_JCC_KERNELS picks the string kernels") {
        for (const char* request : {"plain", "jcc"}) {
            CAPTURE(request);
            ::setenv("DFTRACER_UTILS_JCC_KERNELS", request, 1);
            const auto result = fresh_process::run(
                "the request in the environment picks the kernels");
            ::unsetenv("DFTRACER_UTILS_JCC_KERNELS");
            CHECK(result.ok());
        }
    }
}
