// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/errors.h>
#include <dftracer/utils/dataframe/internal/frame_index.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/shared_string.h>
#include <dftracer/utils/dstd/vector.h>

#include <utility>

namespace dftracer::utils::dataframe {

namespace {

using IndexNames = dstd::Vector<dstd::SharedString>;

void set_names(DataFrame& out, IndexNames names) {
    if (names.empty()) {
        out.row_index.reset();
        return;
    }
    out.row_index = dstd::make_unique<IndexNames>(std::move(names));
}

}  // namespace

dstd::Optional<IndexRule> index_rule_of(dstd::StringView op) {
#define DFTU_FRAME_INDEX_RULE(name, rule) \
    if (op == #name) return IndexRule::rule;
#include <dftracer/utils/dataframe/frame_op_index_rules.def>
    return dstd::nullopt;
}

void apply_index_drop(DataFrame& out) { out.row_index.reset(); }

void apply_index_names(dstd::Span<const dstd::String> names, DataFrame& out) {
    IndexNames kept;
    kept.reserve(names.size());
    for (const dstd::String& n : names)
        if (out.column_index(n) >= 0) kept.emplace_back(dstd::StringView(n));
    set_names(out, std::move(kept));
}

void apply_index_keep(const DataFrame& in, DataFrame& out) {
    if (!in.row_index) {
        out.row_index.reset();
        return;
    }
    IndexNames names;
    names.reserve(in.row_index->size());
    for (const dstd::SharedString& n : *in.row_index)
        if (out.column_index(n) >= 0) names.push_back(n);
    set_names(out, std::move(names));
}

void apply_index_keys(const DataFrame& in, dstd::Span<const dstd::String> keys,
                      DataFrame& out) {
    if (!in.row_index) {
        out.row_index.reset();
        return;
    }
    IndexNames names;
    names.reserve(keys.size());
    for (const dstd::String& k : keys)
        if (out.column_index(k) >= 0) names.emplace_back(dstd::StringView(k));
    set_names(out, std::move(names));
}

void apply_index_renamed(const DataFrame& in, DataFrame& out) {
    if (!in.row_index) {
        out.row_index.reset();
        return;
    }
    IndexNames names;
    names.reserve(in.row_index->size());
    for (const dstd::SharedString& n : *in.row_index) {
        const std::int64_t i = in.column_index(n);
        if (i >= 0 && static_cast<std::size_t>(i) < out.names.size())
            names.push_back(out.names[static_cast<std::size_t>(i)]);
    }
    set_names(out, std::move(names));
}

void apply_index_joined(const DataFrame& left, const DataFrame& right,
                        dstd::Span<const dstd::String> left_on,
                        dstd::Span<const dstd::String> right_on,
                        DataFrame& out) {
    if (!index_join_keeps(left.index_names(), right.index_names(), left_on,
                          right_on)) {
        out.row_index.reset();
        return;
    }
    apply_index_keys(left, left_on, out);
}

void apply_index_concat(dstd::Span<const DataFrame* const> parts,
                        DataFrame& out) {
    if (parts.empty() || !parts.front()->row_index) {
        out.row_index.reset();
        return;
    }
    const dstd::Span<const dstd::SharedString> shared =
        parts.front()->index_names();
    for (const DataFrame* p : parts) {
        if (!index_same(p->index_names(), shared)) {
            out.row_index.reset();
            return;
        }
    }
    for (const dstd::SharedString& n : shared) {
        if (out.column_index(n) < 0) {
            out.row_index.reset();
            return;
        }
    }
    set_names(out, IndexNames(shared.begin(), shared.end()));
}

DataFrame DataFrame::share() const {
    DataFrame out;
    out.names = names;
    out.columns.reserve(columns.size());
    for (const Series& c : columns) out.columns.push_back(c.share());
    if (row_index) out.row_index = dstd::make_unique<IndexNames>(*row_index);
    return out;
}

DataFrame DataFrame::without_index() const {
    DataFrame out = share();
    out.row_index.reset();
    return out;
}

dstd::Result<DataFrame> DataFrame::with_index(
    dstd::Span<const dstd::StringView> index_cols) const {
    IndexNames kept;
    kept.reserve(index_cols.size());
    for (std::size_t i = 0; i < index_cols.size(); ++i) {
        const dstd::StringView name = index_cols[i];
        const std::int64_t at = column_index(name);
        if (at < 0) return not_found<"with_index: no column named {}">(name);
        for (std::size_t j = 0; j < i; ++j)
            if (index_cols[j] == name)
                return invalid_argument<"with_index: column {} repeats">(name);
        const TypeId type = columns[static_cast<std::size_t>(at)].type();
        if (type == TypeId::List || type == TypeId::Struct)
            return invalid_argument<
                "with_index: column {} has type {}, which has no hash and "
                "equality">(name, type_name(type));
        kept.emplace_back(name);
    }
    DataFrame out = share();
    set_names(out, std::move(kept));
    return out;
}

}  // namespace dftracer::utils::dataframe
