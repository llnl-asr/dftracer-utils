// SPDX-License-Identifier: MIT
// The per-line code of Prefilter. CMake compiles this file twice: once as
// `plain`, and once, through prefilter_kernel_jcc.cpp, as `jcc` with the
// branch-padding flag against the Intel JCC erratum. The flag acts on the
// assembler, so it needs a second copy of the code and cannot be a
// per-function attribute.
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/literal_set.h>
#include <dftracer/utils/index/plan/prefilter.h>
#include <dftracer/utils/index/plan/prefilter_kernel.h>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>

#ifndef DFTRACER_UTILS_KERNEL_NS
#define DFTRACER_UTILS_KERNEL_NS plain
#endif

namespace dftracer::utils::index::plan::DFTRACER_UTILS_KERNEL_NS {

namespace {

bool holds(duql::CompareOp op, int cmp) {
    switch (op) {
        case duql::CompareOp::Gt:
            return cmp > 0;
        case duql::CompareOp::Ge:
            return cmp >= 0;
        case duql::CompareOp::Lt:
            return cmp < 0;
        case duql::CompareOp::Le:
            return cmp <= 0;
        default:
            return true;
    }
}

// Doubles past this may round across the bound; such a value keeps the line.
constexpr double EXACT_DOUBLE = 9007199254740992.0;

// Whether the number from `p` (just after a key's colon) satisfies every bound
// of `r`; true when it cannot be read exactly, since only a certain failure
// may drop the line.
bool number_holds(const char* p, const char* end, const Prefilter::Range& r) {
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    const char* tok = p;
    bool integer = true;
    while (p < end && ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' ||
                       *p == '.' || *p == 'e' || *p == 'E')) {
        if (*p == '.' || *p == 'e' || *p == 'E') integer = false;
        ++p;
    }
    if (p == tok) return false;
    std::int64_t iv = 0;
    double dv = 0;
    if (integer) {
        auto [q, ec] = dstd::from_chars(tok, p, iv);
        if (ec != std::errc() || q != p) return true;
    } else {
        auto [q, ec] = dstd::from_chars_double(tok, p, dv);
        if (ec != std::errc() || q != p || std::fabs(dv) >= EXACT_DOUBLE)
            return true;
    }
    for (const auto& b : r.bounds) {
        int cmp = 0;
        if (integer && b.integral) {
            cmp = iv < b.ibound ? -1 : (iv > b.ibound ? 1 : 0);
        } else {
            const double v = integer ? static_cast<double>(iv) : dv;
            const double x =
                b.integral ? static_cast<double>(b.ibound) : b.dbound;
            if (std::fabs(v) >= EXACT_DOUBLE || std::fabs(x) >= EXACT_DOUBLE)
                continue;
            cmp = v < x ? -1 : (v > x ? 1 : 0);
        }
        if (!holds(b.op, cmp)) return false;
    }
    return true;
}

// A key ending in the segment of `r` sits at `start`, preceded by its opening
// quote or a dot, followed by the closing quote, spaces and a colon.
bool range_may_hold(dstd::StringView line, const Prefilter::Range& r) {
    const dstd::String& seg = r.segment;
    const std::size_t n = seg.size() - 1;
    const char* begin = line.data();
    const char* end = begin + line.size();
    const std::size_t p = r.pivot;
    for (const char* cur = begin + 1 + p; cur + (seg.size() - p) <= end;) {
        const char* hit = static_cast<const char*>(std::memchr(
            cur, seg[p], static_cast<std::size_t>(end - cur) - (n - p)));
        if (!hit) return false;
        const char* start = hit - p;
        cur = hit + 1;
        if ((start[-1] != '"' && start[-1] != '.') ||
            std::memcmp(start, seg.data(), seg.size()) != 0)
            continue;
        const char* q = start + seg.size();
        while (q < end && (*q == ' ' || *q == '\t')) ++q;
        if (q < end && *q == ':' && number_holds(q + 1, end, r)) return true;
    }
    return false;
}

// memchr on the pivot byte, then compare: several times faster than memmem
// or string_view::find, which cost more per line than parsing it.
bool contains(dstd::StringView line, const dstd::String& n, std::size_t p) {
    if (line.size() < n.size()) return false;
    const char* begin = line.data();
    const char* last = begin + (line.size() - n.size()) + p;
    for (const char* cur = begin + p; cur <= last;) {
        const char* hit = static_cast<const char*>(
            std::memchr(cur, n[p], static_cast<std::size_t>(last - cur) + 1));
        if (!hit) return false;
        if (std::memcmp(hit - p, n.data(), n.size()) == 0) return true;
        cur = hit + 1;
    }
    return false;
}

bool may_match_per_needle(const Prefilter::Parts& parts,
                          dstd::StringView line) {
    for (std::size_t c = 0; c < parts.clauses.size(); ++c) {
        bool any = false;
        for (std::size_t k = 0; k < parts.clauses[c].size(); ++k)
            if (contains(line, parts.clauses[c][k], parts.pivots[c][k])) {
                any = true;
                break;
            }
        if (!any) return false;
    }
    for (const auto& r : parts.ranges)
        if (!range_may_hold(line, r)) return false;
    return true;
}

bool may_match(const Prefilter::Parts& parts, dstd::StringView line) {
    if (!parts.literals) return may_match_per_needle(parts, line);
    const std::uint64_t* const bits = parts.literals.scan(line);
    if (!bits) return may_match_per_needle(parts, line);
    for (const auto& ids : parts.ids) {
        bool any = false;
        for (const unsigned id : ids)
            if ((bits[id >> 6] >> (id & 63)) & 1) {
                any = true;
                break;
            }
        if (!any) return false;
    }
    for (const auto& r : parts.ranges)
        if (!range_may_hold(line, r)) return false;
    return true;
}

bool gate_may_match(const Prefilter::Parts& parts, Prefilter::GateState& state,
                    dstd::StringView line) {
    if (!state.on) return true;
    const bool pass = may_match(parts, line);
    state.passed += pass ? 1 : 0;
    if (++state.checked == Prefilter::CHECK_WINDOW &&
        static_cast<double>(state.passed) >
            Prefilter::MAX_PASS_RATE * static_cast<double>(state.checked))
        state.on = false;
    return pass;
}

}  // namespace

const PrefilterKernel& kernel() {
    static constexpr PrefilterKernel TABLE{may_match, may_match_per_needle,
                                           gate_may_match};
    return TABLE;
}

}  // namespace dftracer::utils::index::plan::DFTRACER_UTILS_KERNEL_NS
