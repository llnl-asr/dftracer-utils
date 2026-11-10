// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/cpu_features.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/detail/ast.h>
#include <dftracer/utils/duql/literal_set.h>
#include <dftracer/utils/duql/pattern_engine.h>
#include <dftracer/utils/index/plan/condition.h>
#include <dftracer/utils/index/plan/prefilter.h>
#include <dftracer/utils/index/plan/prefilter_kernel.h>

#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <variant>

#ifndef DFTRACER_UTILS_HAS_JCC_VARIANT
#define DFTRACER_UTILS_HAS_JCC_VARIANT 0
#endif

namespace dftracer::utils::index::plan {

namespace {

using Clauses = dstd::Vector<dstd::Vector<dstd::String>>;

// Past this many clauses an `or` distributes into, it adds none.
constexpr std::size_t MAX_CLAUSES = 16;
constexpr std::size_t MIN_NEEDLE = 3;
// Letters from rarest to most common in English text, which key names follow;
// the rarest letter of a key is the one memchr stops on least.
constexpr dstd::StringView LETTERS_BY_RARITY = "zqxjkvbpygfwmucldrhsnioate";

std::size_t letter_rank(char c) {
    const char lower =
        (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    const auto i = LETTERS_BY_RARITY.find(lower);
    return i == dstd::StringView::npos ? 0 : i;
}

// A reader repairs a bad byte in a string to '?', so a '?' of the value need
// not be in the text.
bool safe(dstd::StringView v) {
    for (unsigned char c : v)
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\' || c == '/' ||
            c == '?')
            return false;
    return true;
}

dstd::Optional<dstd::String> needle(const duql::LiteralNode& lit) {
    dstd::String out;
    if (const auto* s = std::get_if<dstd::String>(&lit.value)) {
        if (!safe(*s)) return dstd::nullopt;
        out = "\"" + *s + "\"";
    } else if (const auto* i = std::get_if<std::int64_t>(&lit.value)) {
        out = dstd::to_string(*i);
    } else if (const auto* u = std::get_if<std::uint64_t>(&lit.value)) {
        out = dstd::to_string(*u);
    } else {
        return dstd::nullopt;
    }
    if (out.size() < MIN_NEEDLE) return dstd::nullopt;
    return out;
}

// The clauses a line matching `node` must satisfy; empty when nothing can be
// required of its bytes.
Clauses clauses_of(const duql::detail::QueryNode& node) {
    return std::visit(
        [](auto&& n) -> Clauses {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql::detail::CompareNode>) {
                if (n.op != duql::CompareOp::Eq) return {};
                auto s = needle(n.value);
                if (!s) return {};
                return {{std::move(*s)}};
            } else if constexpr (std::is_same_v<T, duql::detail::InNode>) {
                if (n.values.elements.size() > SEMI_JOIN_CAP) return {};
                dstd::Vector<dstd::String> any;
                for (const auto& e : n.values.elements) {
                    auto s = needle(e);
                    if (!s) return {};
                    any.push_back(std::move(*s));
                }
                if (any.empty()) return {};
                return {std::move(any)};
            } else if constexpr (std::is_same_v<T, duql::detail::MatchNode>) {
                // A case-sensitive pattern's required literals sit in the
                // value's bytes unless JSON escaping hides them.
                if (n.negated || !n.compiled ||
                    (n.op != duql::MatchOp::Like &&
                     n.op != duql::MatchOp::Regex))
                    return {};
                Clauses out;
                for (const auto& lit : duql::required_literals(*n.compiled))
                    if (lit.size() >= MIN_NEEDLE && safe(lit))
                        out.push_back({dstd::String(lit)});
                return out;
            } else if constexpr (std::is_same_v<T, duql::detail::AndNode>) {
                Clauses out = clauses_of(*n.left);
                Clauses right = clauses_of(*n.right);
                out.insert(out.end(), std::make_move_iterator(right.begin()),
                           std::make_move_iterator(right.end()));
                return out;
            } else if constexpr (std::is_same_v<T, duql::detail::OrNode>) {
                // (a1 and a2) or (b1 and b2) holds each ai-or-bj clause.
                const Clauses l = clauses_of(*n.left);
                const Clauses r = clauses_of(*n.right);
                if (l.empty() || r.empty() || l.size() * r.size() > MAX_CLAUSES)
                    return {};
                Clauses out;
                for (const auto& a : l)
                    for (const auto& b : r) {
                        auto& c = out.emplace_back(a);
                        c.insert(c.end(), b.begin(), b.end());
                    }
                return out;
            } else {
                return {};
            }
        },
        node.data);
}

// A segment a key can end in: not an array index, no byte a JSON key would
// escape.
bool plain_key(dstd::StringView k) {
    bool digits_only = true;
    for (unsigned char c : k) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return false;
        digits_only = digits_only && c >= '0' && c <= '9';
    }
    return !k.empty() && !digits_only;
}

// The ranges a line matching `node` must satisfy: numeric order leaves
// joined by `and`.
void ranges_of(const duql::detail::QueryNode& node,
               dstd::Vector<Prefilter::Range>& out) {
    if (const auto* a = std::get_if<duql::detail::AndNode>(&node.data)) {
        ranges_of(*a->left, out);
        ranges_of(*a->right, out);
        return;
    }
    const auto* n = std::get_if<duql::detail::CompareNode>(&node.data);
    // An any() key is followed by an array, not the number a range reads.
    if (!n || n->field.any || n->op == duql::CompareOp::Ne) return;
    const dstd::String path(n->field.path);
    const dstd::StringView key =
        dstd::StringView(path).substr(path.rfind('.') + 1);
    if (!plain_key(key)) return;
    Prefilter::Bound bound{n->op};
    if (const auto* i = std::get_if<std::int64_t>(&n->value.value)) {
        bound.ibound = *i;
    } else if (const auto* u = std::get_if<std::uint64_t>(&n->value.value)) {
        if (*u > static_cast<std::uint64_t>(INT64_MAX)) return;
        bound.ibound = static_cast<std::int64_t>(*u);
    } else if (const auto* d = std::get_if<double>(&n->value.value)) {
        bound.integral = false;
        bound.dbound = *d;
    } else {
        return;
    }
    // A number equals the bound when it is at both ends of [bound, bound].
    dstd::Vector<Prefilter::Bound> bounds{bound};
    if (n->op == duql::CompareOp::Eq) {
        bounds[0].op = duql::CompareOp::Ge;
        bounds.push_back(bounds[0]);
        bounds[1].op = duql::CompareOp::Le;
    }
    for (auto& r : out)
        if (r.path == path) {
            r.bounds.insert(r.bounds.end(), bounds.begin(), bounds.end());
            return;
        }
    std::size_t pivot = 0;
    for (std::size_t i = 1; i < key.size(); ++i)
        if (letter_rank(key[i]) < letter_rank(key[pivot])) pivot = i;
    out.push_back({path, dstd::String(key) + "\"", std::move(bounds), pivot});
}

// JSON lines are mostly structure, digits and lowercase keys; a needle's
// byte outside those is rare, so memchr on it stops seldom.
int rarity(unsigned char c) {
    if (c == '"' || c == ':' || c == ',' || c == '{' || c == '}' || c == ' ')
        return 0;
    if (c >= '0' && c <= '9') return 1;
    if (c >= 'a' && c <= 'z') return 2;
    return 3;
}

std::size_t pivot_of(const dstd::String& n) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < n.size(); ++i)
        if (rarity(static_cast<unsigned char>(n[i])) >
            rarity(static_cast<unsigned char>(n[best])))
            best = i;
    return best;
}

}  // namespace

Prefilter::Prefilter(const duql::Query& q) : kernel_(&prefilter_kernel()) {
    parts_.clauses = clauses_of(duql::detail::QueryAccess::root(q));
    ranges_of(duql::detail::QueryAccess::root(q), parts_.ranges);
    parts_.pivots.reserve(parts_.clauses.size());
    for (const auto& clause : parts_.clauses) {
        auto& p = parts_.pivots.emplace_back();
        for (const auto& n : clause) p.push_back(pivot_of(n));
    }
    std::size_t widest = 0;
    for (const auto& clause : parts_.clauses)
        widest = std::max(widest, clause.size());
    if (!duql::LiteralSet::AVAILABLE || widest < HS_MIN_ALTERNATIVES) return;
    dstd::HashMap<dstd::String, unsigned> seen;
    dstd::Vector<dstd::StringView> literals;
    parts_.ids.reserve(parts_.clauses.size());
    for (const auto& clause : parts_.clauses) {
        auto& ids = parts_.ids.emplace_back();
        for (const auto& n : clause) {
            auto [it, fresh] =
                seen.try_emplace(n, static_cast<unsigned>(literals.size()));
            if (fresh) literals.push_back(n);
            ids.push_back(it->second);
        }
    }
    parts_.literals = duql::LiteralSet::make(literals);
}

namespace {

const PrefilterKernel* jcc_kernel() {
#if DFTRACER_UTILS_HAS_JCC_VARIANT
    return &jcc::kernel();
#else
    return nullptr;
#endif
}

}  // namespace

const PrefilterKernel& resolve_prefilter_kernel(common::JccKernels request,
                                                bool cpu_affected) {
    if (!common::prefers_jcc_kernels(request, cpu_affected))
        return plain::kernel();
    if (const PrefilterKernel* jcc = jcc_kernel()) return *jcc;
    if (request == common::JccKernels::Jcc)
        DFTRACER_UTILS_LOG_WARN(
            "{}=jcc, but this build has no jcc prefilter kernel; using plain",
            common::constants::jcc_kernel_settings::KERNELS_ENV);
    return plain::kernel();
}

const PrefilterKernel& prefilter_kernel() {
    static const PrefilterKernel& chosen = resolve_prefilter_kernel(
        common::jcc_kernels_request(), common::cpu_has_jcc_erratum());
    return chosen;
}

const PrefilterKernel* prefilter_kernel_variant(common::JccKernels kind) {
    switch (kind) {
        case common::JccKernels::Plain:
            return &plain::kernel();
        case common::JccKernels::Jcc:
            return jcc_kernel();
        case common::JccKernels::Auto:
            break;
    }
    return nullptr;
}

Prefilter::Prefilter() : kernel_(&prefilter_kernel()) {}

bool Prefilter::may_match_per_needle(dstd::StringView line) const {
    return kernel_->may_match_per_needle(parts_, line);
}

bool Prefilter::may_match(dstd::StringView line) const {
    return kernel_->may_match(parts_, line);
}

bool Prefilter::Gate::may_match(dstd::StringView line) {
    return p_->kernel_->gate_may_match(p_->parts_, state_, line);
}

}  // namespace dftracer::utils::index::plan
