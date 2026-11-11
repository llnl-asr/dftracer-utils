// SPDX-License-Identifier: MIT
// The hot loops of the string-match kernels. CMake compiles this file twice
// when the compiler can pad branches for the Intel JCC erratum: once as
// `plain`, and once, through string_match_jcc.cpp, as `jcc` with the padding
// flag. The flag acts on the assembler, so it needs a second copy of the
// code and cannot be a per-function attribute.
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/string_reader.h>
#include <dftracer/utils/dataframe/kernels/limited_rows.h>
#include <dftracer/utils/dataframe/kernels/string_match.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/duql/pattern_engine.h>
#include <dftracer/utils/duql/substr_simd.h>

#include <cstdint>

#ifndef DFTRACER_UTILS_KERNEL_NS
#define DFTRACER_UTILS_KERNEL_NS plain
#endif

namespace dftracer::utils::dataframe::kernels::DFTRACER_UTILS_KERNEL_NS {

namespace {

using duql::detail::substr_find;

// Bool mask of `p` over `v`; a row whose match reached the work limit is null.
dftu_series* pattern_predicate(const dftu_series* v,
                               const duql::CompiledPattern& p) {
    DFTU_PER_CHUNK(v, pattern_predicate, p);
    // A literal pattern runs inline, with no call per row.
    if (const auto t = duql::literal_test(p); t && !t->icase) {
        const dstd::StringView lit = t->text;
        switch (t->op) {
            case duql::LiteralTest::Op::Equals:
                return string_mask(
                    v, [lit](dstd::StringView s) { return s == lit; });
            case duql::LiteralTest::Op::StartsWith:
                return string_mask(v, [lit](dstd::StringView s) {
                    return s.starts_with(lit);
                });
            case duql::LiteralTest::Op::EndsWith:
                return string_mask(
                    v, [lit](dstd::StringView s) { return s.ends_with(lit); });
            case duql::LiteralTest::Op::Contains:
                return string_mask(v, [lit](dstd::StringView s) {
                    return substr_find(
                               s.data(), static_cast<std::int64_t>(s.size()),
                               lit.data(),
                               static_cast<std::int64_t>(lit.size())) >= 0;
                });
        }
    }
    // Case-sensitive globs run inline too; SEGMENTS search long strings.
    if (!p.icase && p.kind == duql::detail::Kind::Glob)
        return string_mask(v, [toks = p.glob](dstd::StringView s) {
            return duql::detail::glob_match_t<false, true>(toks, s);
        });
    if (!p.icase && p.kind == duql::detail::Kind::Segments)
        return string_mask(v, [&p, toks = p.glob](dstd::StringView s) {
            return s.size() < duql::detail::SIMD_MIN_HAY
                       ? duql::detail::glob_match_t<false, false>(toks, s)
                       : duql::detail::segments_match(p, s);
        });
    LimitedRows limited;
    dftu_series* out = string_mask(v, [&](dstd::StringView s) {
        const auto r = duql::match(p, s);
        if (r == duql::MatchResult::Limit) limited.add(s);
        return r == duql::MatchResult::Yes;
    });
    return limited.apply(out, v);
}

dftu_series* mask_eq(const dftu_series* v, const char* rhs,
                     std::int32_t rhs_size) {
    dstd::StringView r(rhs, static_cast<std::size_t>(rhs_size));
    return string_mask(v, [r](dstd::StringView s) { return s == r; });
}

dftu_series* mask_contains(const dftu_series* v, const char* needle,
                           std::int32_t needle_size) {
    dstd::StringView n(needle, static_cast<std::size_t>(needle_size));
    return string_mask(v, [n](dstd::StringView s) {
        return substr_find(s.data(), static_cast<std::int64_t>(s.size()),
                           n.data(), static_cast<std::int64_t>(n.size())) >= 0;
    });
}

dftu_series* mask_starts_with(const dftu_series* v, const char* prefix,
                              std::int32_t prefix_size) {
    dstd::StringView p(prefix, static_cast<std::size_t>(prefix_size));
    return string_mask(v, [p](dstd::StringView s) { return s.starts_with(p); });
}

dftu_series* mask_ends_with(const dftu_series* v, const char* suffix,
                            std::int32_t suffix_size) {
    dstd::StringView p(suffix, static_cast<std::size_t>(suffix_size));
    return string_mask(v, [p](dstd::StringView s) { return s.ends_with(p); });
}

}  // namespace

const StringMatch& table() {
    static constexpr StringMatch TABLE{pattern_predicate, mask_eq,
                                       mask_contains, mask_starts_with,
                                       mask_ends_with};
    return TABLE;
}

}  // namespace dftracer::utils::dataframe::kernels::DFTRACER_UTILS_KERNEL_NS
