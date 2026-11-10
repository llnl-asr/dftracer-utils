// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/overlap.h>
#include <dftracer/utils/duql/pipeline.h>
#include <dftracer/utils/duql/vectorize.h>
#include <dftracer/utils/index/cache/lookup_store.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_duql_internal.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace dftracer::utils::trace::views::duql_detail {

dstd::Result<bool> Applier::cached(std::size_t i) const {
    if (ctx_.sides->stored[i]) return true;
    const auto& key = ctx_.sides->keys[i];
    if (!key) return false;
    const auto db = index::cache::open_lookup_db(
        key->path, index::cache::CacheStore::Mode::ReadOnly);
    if (!db) return false;
    return index::cache::lookup_exists(*db, key->sig);
}

dstd::Result<dataframe::LazyFrame> Applier::side_plan(std::size_t i,
                                                      bool& memory) const {
    memory =
        std::find(collected_.begin(), collected_.end(), i) != collected_.end();
    if (!memory) {
        const auto held = cached(i);
        if (!held.has_value()) return held.error();
        memory = *held;
    }
    if (!memory)
        return dataframe::detail::tap(
            ctx_.sides->views[i]->lazy(),
            side_tap(main_ ? ctx_.sides : nullptr, ctx_.sides, i));
    dataframe::Schema schema;
    for (const auto& c : ctx_.sides->columns[i])
        schema.fields.push_back(
            dataframe::Field{dstd::String(c.name), c.type, true});
    return side_source(main_ ? ctx_.sides : nullptr, ctx_.sides, i,
                       std::move(schema));
}

Status Applier::stage(const duql::PipelineLookup& l) {
    leave_scan("lookup");
    const dstd::Vector<dstd::String> before = names();
    const auto& side_cols = ctx_.sides->columns[l.side];
    auto side_index =
        [&](dstd::StringView name) -> dstd::Optional<std::size_t> {
        for (std::size_t i = 0; i < side_cols.size(); ++i)
            if (side_cols[i].name == name) return i;
        if (schema_.args_fallback)
            for (std::size_t i = 0; i < side_cols.size(); ++i)
                if (side_cols[i].name == "args." + dstd::String(name)) return i;
        return dstd::nullopt;
    };
    dstd::Vector<dstd::String> left;
    dstd::Vector<dstd::String> right;
    dstd::String keys_text;
    for (std::size_t k = 0; k < l.keys.size(); ++k) {
        const auto& [item, column] = l.keys[k];
        const auto r = side_index(column);
        if (!r)
            return refuse<"'lookup {}': '{}' has no column '{}'">(
                l.name, l.name, column);
        right.push_back(side_cols[*r].name);
        auto from = source(item, dstd::fmt<"__duql_lk_{}">(k));
        if (!from) return from.error();
        left.push_back(std::move(*from));
        if (!keys_text.empty()) keys_text += ", ";
        keys_text += dstd::fmt<"{} == {}">(item.text, column);
    }
    if (const auto* a = std::get_if<duql::PipelineAsof>(&l.mode))
        return asof_lookup(l, *a, left, right, keys_text, side_index);
    if (const auto* o = std::get_if<duql::PipelineOverlap>(&l.mode))
        return overlap_lookup(l, *o, left, right, keys_text, side_index);
    if (l.kind == duql::syntax::LookupKind::Anti) {
        bool memory = false;
        auto planned = side_plan(l.side, memory);
        if (!planned) return planned.error();
        dataframe::LazyFrame side = std::move(*planned);
        lines.push_back(dstd::fmt<"lookup {} on {} anti{}">(
            l.name, keys_text,
            memory ? " (side from memory)"
                   : " (side joined, sharing the scan)"));
        v_ = v_.with_lazy(v_.lazy().join(std::move(side), std_strs(left),
                                         std_strs(right),
                                         dataframe::JoinHow::Anti));
        if (names() != before) return keep(before);
        return {};
    }
    const bool inner = l.kind == duql::syntax::LookupKind::Inner;
    const auto* into = std::get_if<duql::PipelineNest>(&l.mode);
    const bool nest = into != nullptr;
    dstd::Vector<duql::VectorColumn> next = cols_;
    dstd::Vector<dstd::String> added;
    auto shown = [&](const dstd::String& n) {
        return index(n) &&
               std::find(extras_.begin(), extras_.end(), n) == extras_.end();
    };
    if (nest) {
        if (index(into->name))
            return refuse<
                "'lookup {} into {}' names a column the rows have; pick "
                "another name">(l.name, into->name);
        dstd::Vector<dataframe::Field> fields;
        for (const auto& c : side_cols)
            fields.push_back(dataframe::Field{std_str(c.name), c.type, true});
        next.push_back(
            {dstd::String(into->name),
             dataframe::list_of(dataframe::struct_of(std::move(fields)))});
        added.push_back(dstd::String(into->name));
    } else {
        for (const auto& c : side_cols) {
            if (std::find(right.begin(), right.end(), c.name) != right.end())
                continue;
            const auto at = index(c.name);
            // A record field the rows lack stays null there, and the
            // join fills it; any other clash is the query's error.
            if (at && shown(c.name) && !cols_[*at].record)
                return refuse<
                    "'lookup {}' adds '{}', which the rows already have; "
                    "rename one with 'rename' or read the matches with "
                    "'into'">(l.name, c.name);
            if (at) {
                next[*at] = {c.name, c.type};
            } else {
                next.push_back({c.name, c.type});
                added.push_back(c.name);
            }
        }
    }
    bool memory = false;
    auto planned = side_plan(l.side, memory);
    if (!planned) return planned.error();
    dataframe::LazyFrame side = std::move(*planned);
    dstd::Vector<dstd::String> right_on = right;
    if (!nest) {
        // The key columns name the row set in the join's errors.
        // dstd::Vector<dstd::String>
        dstd::Vector<dstd::String> renamed = side.schema();
        for (auto& n : renamed)
            if (std::find(right.begin(), right.end(), dstd::StringView(n)) !=
                right.end()) {
                dstd::String prefixed(l.name);
                prefixed += '.';
                prefixed += n;
                n = std::move(prefixed);
            }
        side = side.rename(std::move(renamed));
        for (auto& k : right_on) k = dstd::fmt<"{}.{}">(l.name, k);
        if (inner) {
            side = side.with_column(dstd::String(HIT_COLUMN),
                                    dataframe::expr_lit_bool(true));
            next.push_back(
                {HIT_COLUMN, dataframe::scalar(dataframe::TypeId::Bool)});
        }
    }
    lines.push_back(dstd::fmt<"lookup {} on {}{}{}{}">(
        l.name, keys_text, inner ? " inner" : "",
        nest ? dstd::fmt<" into {}">(into->name) : dstd::String(),
        memory ? " (side from memory)" : " (side joined, sharing the scan)"));
    v_ = v_.with_lazy(v_.lazy().join(
        std::move(side), std_strs(left), std_strs(right_on),
        nest ? dataframe::JoinHow::Nest : dataframe::JoinHow::Lookup,
        nest ? std_str(into->name) : dstd::String()));
    cols_ = std::move(next);
    if (inner) {
        if (nest)
            v_ = v_.with_lazy(v_.lazy().filter(
                dataframe::expr_list_size(col(into->name)) > std::int64_t{0}));
        else
            v_ = v_.with_lazy(v_.lazy().filter(
                dataframe::expr_not(dataframe::expr_is_null(col(HIT_COLUMN)))));
    }
    dstd::Vector<dstd::String> keep_names = before;
    for (const auto& n : added) keep_names.push_back(n);
    if (names() != keep_names) return keep(std::move(keep_names));
    return {};
}

dstd::Result<dataframe::DataType> Applier::asof_type(
    const dstd::String& what, const dstd::String& side,
    const dataframe::DataType& rows, const dataframe::DataType& other) {
    if (rows.id == dataframe::TypeId::Unknown &&
        other.id == dataframe::TypeId::Unknown)
        return refuse<
            "'lookup {} ... asof': {} has no known type; convert it with "
            "int(), float() or string()">(side, what);
    if (rows == other) return rows;
    if (rows.id == dataframe::TypeId::Unknown) return other;
    if (other.id == dataframe::TypeId::Unknown) return rows;
    if (is_number(rows.id) && is_number(other.id))
        return dataframe::scalar(is_integer(rows.id) && is_integer(other.id)
                                     ? dataframe::TypeId::Int64
                                     : dataframe::TypeId::Float64);
    return refuse<"'lookup {} ... asof': {} is {} in the rows and {} in '{}'">(
        side, what, dataframe::type_name(rows.id),
        dataframe::type_name(other.id), side);
}

dstd::Result<Applier::SideValues> Applier::side_values(
    const duql::PipelineLookup& l, const char* clause,
    const dstd::Vector<std::size_t>& keys, std::size_t consumed,
    std::size_t consumed2) const {
    const auto& side_cols = ctx_.sides->columns[l.side];
    SideValues out;
    out.names.resize(side_cols.size());
    auto shown = [&](const dstd::String& n) {
        return index(n) &&
               std::find(extras_.begin(), extras_.end(), n) == extras_.end();
    };
    for (std::size_t c = 0; c < side_cols.size(); ++c) {
        if (c == consumed || c == consumed2 ||
            std::find(keys.begin(), keys.end(), c) != keys.end())
            continue;
        const dstd::String& own_name = side_cols[c].name;
        const auto at = index(own_name);
        if (at && shown(own_name) && !cols_[*at].record)
            return refuse<
                "'lookup {} ... {}' adds '{}', which the rows already have; "
                "rename one with 'rename'">(l.name, clause, own_name);
        out.names[c] = own_name;
        if (at) {
            const dataframe::DataType& own = cols_[*at].type;
            const dataframe::DataType& theirs = side_cols[c].type;
            if (own.id != dataframe::TypeId::Unknown &&
                theirs.id != dataframe::TypeId::Unknown && own != theirs)
                return refuse<
                    "'lookup {} ... {}': '{}' is {} in the rows and "
                    "{} in '{}'">(l.name, clause, own_name,
                                  dataframe::type_name(own.id),
                                  dataframe::type_name(theirs.id), l.name);
            out.names[c] = "__duql_av_" + own_name;
            out.fills.emplace_back(own_name, out.names[c]);
        } else {
            out.added.push_back(own_name);
        }
        out.columns.push_back({out.names[c], side_cols[c].type});
    }
    return out;
}

Status Applier::asof_lookup(
    const duql::PipelineLookup& l, const duql::PipelineAsof& a,
    const dstd::Vector<dstd::String>& left,
    const dstd::Vector<dstd::String>& right, const dstd::String& keys_text,
    const dstd::Function<dstd::Optional<std::size_t>(dstd::StringView)>&
        side_index) {
    const auto& side_cols = ctx_.sides->columns[l.side];
    const auto t = side_index(a.column);
    if (!t)
        return refuse<"'lookup {} ... asof': '{}' has no column '{}'">(
            l.name, l.name, a.column);
    const dstd::Vector<dstd::String> before = names();
    dstd::Vector<std::size_t> side_key;
    for (const auto& r : right) side_key.push_back(*side_index(r));
    if (std::find(side_key.begin(), side_key.end(), *t) != side_key.end())
        return refuse<"'lookup {} ... asof': '{}' is both a key and the time">(
            l.name, a.column);

    dstd::Vector<dstd::String> equi;
    dstd::Vector<dstd::String> side_names;
    for (const auto& c : side_cols) side_names.push_back(c.name);
    bool memory = false;
    auto planned = side_plan(l.side, memory);
    if (!planned) return planned.error();
    dataframe::LazyFrame side = std::move(*planned);
    for (std::size_t k = 0; k < left.size(); ++k) {
        const dataframe::DataType lt = cols_[*index(left[k])].type;
        const dataframe::DataType st = side_cols[side_key[k]].type;
        const auto shared = asof_type(dstd::fmt<"key '{}'">(l.keys[k].second),
                                      dstd::String(l.name), lt, st);
        if (!shared) return shared.error();
        const dataframe::DataType to = *shared;
        const dstd::String from = dstd::fmt<"__duql_lk_{}">(k);
        if (auto done =
                with_column(from, dataframe::expr_cast(to.id, col(left[k])),
                            dstd::String(l.keys[k].first.text));
            !done)
            return done;
        side = side.with_column(
            std_str(side_names[side_key[k]]),
            dataframe::expr_cast(
                to.id,
                dataframe::expr_col(static_cast<std::int32_t>(side_key[k]))));
        equi.push_back(from);
    }
    auto time_column = source(a.time, "__duql_at");
    if (!time_column) return time_column.error();
    const dstd::String time_src = std::move(*time_column);
    const auto time_shared =
        asof_type(dstd::fmt<"time '{}'">(a.column), dstd::String(l.name),
                  cols_[*index(time_src)].type, side_cols[*t].type);
    if (!time_shared) return time_shared.error();
    const dataframe::DataType time_type = *time_shared;
    if (!is_number(time_type.id))
        return refuse<"'lookup {} ... asof': time '{}' is {}, not a number">(
            l.name, a.time.text, dataframe::type_name(time_type.id));
    const dstd::String time = "__duql_at";
    if (auto done =
            with_column(time, dataframe::expr_cast(time_type.id, col(time_src)),
                        dstd::String(a.time.text));
        !done)
        return done;
    side = side.with_column(
        std_str(side_names[*t]),
        dataframe::expr_cast(
            time_type.id, dataframe::expr_col(static_cast<std::int32_t>(*t))));

    auto gathered = side_values(l, "asof", side_key, *t);
    if (!gathered) return gathered.error();
    SideValues values = std::move(*gathered);
    // A null key has no key: the op would pair null with null.
    for (const auto k : side_key)
        side = side.filter(dataframe::expr_is_null(
            dataframe::expr_col(static_cast<std::int32_t>(k)), false));
    dstd::Vector<dstd::String> renamed = side_names;
    renamed[*t] = time;
    for (std::size_t k = 0; k < side_key.size(); ++k)
        renamed[side_key[k]] = equi[k];
    for (std::size_t c = 0; c < renamed.size(); ++c)
        if (!values.names[c].empty()) renamed[c] = values.names[c];
    side = side.rename(std_strs(renamed));

    const dstd::String pos = "__duql_pos";
    row_index(pos);
    dstd::Vector<dstd::String> out = names();
    for (const auto& n : values.names)
        if (!n.empty()) out.push_back(n);
    dstd::Vector<const char*> by;
    for (const auto& e : equi) by.push_back(e.c_str());
    dataframe::OpArgs args;
    args.str(2, time.c_str())
        .strlist(3, by.data(), static_cast<std::int32_t>(by.size()))
        .i32(4, static_cast<std::int32_t>(a.direction))
        .f64(5, a.tolerance.value_or(-1));
    static constexpr const char* DIRECTIONS[] = {"backward", "forward",
                                                 "nearest"};
    static_assert(static_cast<int>(duql::syntax::AsofDirection::Forward) ==
                  DFTU_ASOF_FORWARD);
    static_assert(static_cast<int>(duql::syntax::AsofDirection::Nearest) ==
                  DFTU_ASOF_NEAREST);
    lines.push_back(dstd::fmt<"lookup {} on {} asof {} {}{} (op "
                              "dftu.frame.asof)">(
        l.name, keys_text, a.time.text,
        DIRECTIONS[static_cast<std::size_t>(a.direction)],
        a.tolerance ? dstd::fmt<" within {}">(number_text(*a.tolerance))
                    : dstd::String()));
    v_ = v_.with_lazy(v_.lazy().frame_op("dftu.frame.asof", args,
                                         {std::move(side)}, std_strs(out)));
    for (auto& c : values.columns) cols_.push_back(std::move(c));
    for (const auto& [name, tmp] : values.fills)
        if (auto done = with_column(
                name, dataframe::expr_coalesce({col(name), col(tmp)}),
                dstd::fmt<"{} ?? {}">(name, tmp));
            !done)
            return done;
    lines.push_back(dstd::fmt<"sort_by {}">(pos));
    v_ = v_.with_lazy(v_.lazy().sort_by(std_str(pos)));
    dstd::Vector<dstd::String> keep_names = before;
    for (const auto& n : values.added) keep_names.push_back(n);
    return keep(std::move(keep_names));
}

Status Applier::overlap_lookup(
    const duql::PipelineLookup& l, const duql::PipelineOverlap& o,
    const dstd::Vector<dstd::String>& left,
    const dstd::Vector<dstd::String>& right, const dstd::String& keys_text,
    const dstd::Function<dstd::Optional<std::size_t>(dstd::StringView)>&
        side_index) {
    const auto& side_cols = ctx_.sides->columns[l.side];
    const auto t = side_index(o.time_column);
    const auto d = side_index(o.duration_column);
    if (!t || !d)
        return refuse<
            "'lookup {} ... overlap': '{}' needs the columns '{}' and '{}', "
            "the record schema's time and duration">(
            l.name, l.name, o.time_column, o.duration_column);
    const dstd::Vector<dstd::String> before = names();
    dstd::Vector<std::size_t> side_key;
    for (const auto& r : right) side_key.push_back(*side_index(r));
    auto time_column = source(o.time, "__duql_ot");
    if (!time_column) return time_column.error();
    const dstd::String time = std::move(*time_column);
    auto duration_column = source(o.duration, "__duql_od");
    if (!duration_column) return duration_column.error();
    const dstd::String duration = std::move(*duration_column);

    const bool nest = !o.into.empty();
    SideValues values;
    dstd::Vector<duql::VectorColumn> next = cols_;
    if (nest) {
        if (index(o.into))
            return refuse<
                "'lookup {} overlap into {}' names a column the rows have; "
                "pick another name">(l.name, o.into);
        dstd::Vector<dataframe::Field> fields;
        for (const auto& c : side_cols)
            fields.push_back(dataframe::Field{std_str(c.name), c.type, true});
        next.push_back(
            {dstd::String(o.into),
             dataframe::list_of(dataframe::struct_of(std::move(fields)))});
        values.added.push_back(dstd::String(o.into));
    } else {
        auto gathered = side_values(l, "overlap", side_key, *t, *d);
        if (!gathered) return gathered.error();
        values = std::move(*gathered);
        for (const auto& c : values.columns) next.push_back(c);
    }

    dataframe::Schema schema;
    for (const auto& c : next)
        schema.fields.push_back(
            dataframe::Field{dstd::String(c.name), c.type, true});
    const dstd::Vector<dstd::String> side_names = [&] {
        dstd::Vector<dstd::String> out;
        for (const auto& c : side_cols) out.push_back(c.name);
        return out;
    }();
    const std::size_t side_id = l.side;
    const dstd::String into = dstd::String(o.into);
    const double scale = o.scale;
    const dstd::Vector<std::size_t> consumed = [&] {
        dstd::Vector<std::size_t> out = side_key;
        out.push_back(*t);
        out.push_back(*d);
        return out;
    }();
    const dstd::Vector<dstd::String> value_names = values.names;
    dstd::SharedPtr<Sides> strong = main_ ? ctx_.sides : nullptr;
    dstd::WeakPtr<Sides> weak = ctx_.sides;
    finish(
        [=](dataframe::DataFrame rows) -> dstd::Result<dataframe::DataFrame> {
            dstd::SharedPtr<Sides> sides = strong;
            if (!sides) {
                auto owned = owner(weak);
                if (!owned) return owned.error();
                sides = std::move(*owned);
            }
            const auto held = sides->frame(side_id);
            if (!held) return held.error();
            dataframe::DataFrame side;
            side.names = (*held)->names;
            for (const auto& c : (*held)->columns)
                side.columns.push_back(c.share());
            side = duql::flat_frame(std::move(side));
            rows = duql::flat_frame(std::move(rows));
            auto find = [](const dataframe::DataFrame& f,
                           const dstd::String& n) -> const dataframe::Series* {
                for (std::size_t i = 0; i < f.names.size(); ++i)
                    if (dstd::StringView(f.names[i]) == n) return &f.columns[i];
                return nullptr;
            };
            auto take =
                [&](const dataframe::DataFrame& f,
                    const dstd::String& n) -> dstd::Result<dataframe::Series> {
                const dataframe::Series* s = find(f, n);
                if (!s)
                    return common::make_error<"overlap: no column {}">(
                        common::ErrorCode::InvalidArgument, n);
                return s->share();
            };
            duql::OverlapColumns mine;
            duql::OverlapColumns theirs;
            for (const auto& n : left) {
                auto s = take(rows, n);
                if (!s) return s.error();
                mine.keys.push_back(std::move(*s));
            }
            auto start = take(rows, time);
            if (!start) return start.error();
            mine.start = std::move(*start);
            auto length = take(rows, duration);
            if (!length) return length.error();
            mine.duration = std::move(*length);
            mine.scale = scale;
            for (const auto k : side_key) {
                auto s = take(side, side_names[k]);
                if (!s) return s.error();
                theirs.keys.push_back(std::move(*s));
            }
            auto their_start = take(side, side_names[*t]);
            if (!their_start) return their_start.error();
            theirs.start = std::move(*their_start);
            auto their_length = take(side, side_names[*d]);
            if (!their_length) return their_length.error();
            theirs.duration = std::move(*their_length);
            theirs.scale = scale;
            auto matched = duql::overlap_matches(mine, theirs);
            if (!matched) return matched.error();
            const duql::OverlapMatches& m = *matched;

            dataframe::DataFrame out;
            out.names = rows.names;
            if (!into.empty()) {
                for (const auto& c : rows.columns)
                    out.columns.push_back(c.share());
                dstd::Vector<std::int32_t> offsets;
                for (const auto x : m.offsets)
                    offsets.push_back(static_cast<std::int32_t>(x));
                dstd::Vector<dataframe::Series> parts;
                for (const auto& c : side.columns)
                    parts.push_back(c.take(m.rows));
                out.names.emplace_back(into);
                out.columns.push_back(dataframe::Series::list(
                    offsets,
                    dataframe::Series::structs(side.names, std::move(parts))));
                return out;
            }
            dstd::Vector<std::int64_t> from_rows;
            dstd::Vector<std::int64_t> from_side;
            for (std::size_t i = 0; i + 1 < m.offsets.size(); ++i) {
                if (m.offsets[i] == m.offsets[i + 1]) {
                    from_rows.push_back(static_cast<std::int64_t>(i));
                    from_side.push_back(-1);
                }
                for (auto k = m.offsets[i]; k < m.offsets[i + 1]; ++k) {
                    from_rows.push_back(static_cast<std::int64_t>(i));
                    from_side.push_back(m.rows[static_cast<std::size_t>(k)]);
                }
            }
            for (const auto& c : rows.columns)
                out.columns.push_back(c.take(from_rows));
            for (std::size_t c = 0; c < value_names.size(); ++c) {
                if (value_names[c].empty()) continue;
                out.names.emplace_back(value_names[c]);
                out.columns.push_back(side.columns[c].take(from_side));
            }
            return out;
        },
        dstd::fmt<"lookup {} on {} overlap{} (sweep over {})">(
            l.name, keys_text,
            nest ? dstd::fmt<" into {}">(o.into) : dstd::String(), l.name),
        std::move(schema));
    cols_ = std::move(next);
    for (const auto& [name, tmp] : values.fills)
        if (auto done = with_column(
                name, dataframe::expr_coalesce({col(name), col(tmp)}),
                dstd::fmt<"{} ?? {}">(name, tmp));
            !done)
            return done;
    dstd::Vector<dstd::String> keep_names = before;
    for (const auto& n : values.added) keep_names.push_back(n);
    return keep(std::move(keep_names));
}

dstd::Result<dataframe::DataType> Applier::unify(const dstd::String& name,
                                                 const dataframe::DataType& a,
                                                 const dataframe::DataType& b) {
    if (a.id == dataframe::TypeId::Unknown) return b;
    if (b.id == dataframe::TypeId::Unknown) return a;
    if (a == b) return a;
    if (is_number(a.id) && is_number(b.id))
        return dataframe::scalar(is_integer(a.id) && is_integer(b.id)
                                     ? dataframe::TypeId::Int64
                                     : dataframe::TypeId::Float64);
    return refuse<
        "'union' gives column '{}' as {} and {}; convert one with int(), "
        "float() or string()">(name, dataframe::type_name(a.id),
                               dataframe::type_name(b.id));
}

View Applier::align(View v, const dstd::Vector<duql::VectorColumn>& cols,
                    const dstd::Vector<duql::VectorColumn>& want) {
    auto at = [&](const dstd::String& n) -> dstd::Optional<std::size_t> {
        for (std::size_t i = 0; i < cols.size(); ++i)
            if (cols[i].name == n) return i;
        return dstd::nullopt;
    };
    dstd::Vector<dstd::String> order;
    for (const auto& w : want) {
        order.push_back(w.name);
        const auto i = at(w.name);
        const dataframe::TypeId to = w.type.id == dataframe::TypeId::Unknown
                                         ? dataframe::TypeId::String
                                         : w.type.id;
        if (!i) {
            v = v.with_column(std_str(w.name), dataframe::expr_lit_null(to));
            continue;
        }
        if (cols[*i].type == w.type) continue;
        const dataframe::Expr c =
            dataframe::expr_col(static_cast<std::int32_t>(*i));
        v = v.with_column(
            std_str(w.name),
            to == dataframe::TypeId::String
                ? dataframe::expr_convert(dataframe::ConvertOp::String, c)
                : dataframe::expr_cast(to, c));
    }
    return v.with_lazy(v.lazy().select(std_strs(order)));
}

Status Applier::stage(const duql::PipelineUnion& u) {
    leave_scan("union");
    const std::size_t first_line = lines.size();
    auto built = build(ctx_, *u.other, true, lines);
    if (!built) return built.error();
    Built other = std::move(*built);
    for (std::size_t i = first_line; i < lines.size(); ++i)
        lines[i] = "  " + lines[i];
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(first_line),
                 dstd::fmt<"union from {}:">(
                     input_text(u.other->input, ctx_.sides->program)));
    dstd::Vector<duql::VectorColumn> want;
    for (const auto& c : cols_) want.push_back({c.name, c.type});
    for (const auto& c : other.cols) {
        auto it = std::find_if(want.begin(), want.end(),
                               [&](const auto& w) { return w.name == c.name; });
        if (it == want.end())
            want.push_back({c.name, c.type});
        else {
            auto shared = unify(c.name, it->type, c.type);
            if (!shared) return shared.error();
            it->type = std::move(*shared);
        }
    }
    for (auto& w : want)
        if (w.type.id == dataframe::TypeId::Unknown)
            w.type = dataframe::scalar(dataframe::TypeId::String);
    View mine = align(v_, cols_, want);
    View theirs = align(other.view, other.cols, want);
    v_ = v_.with_lazy(mine.lazy().concat(theirs.lazy()));
    cols_ = std::move(want);
    return {};
}

}  // namespace dftracer::utils::trace::views::duql_detail
