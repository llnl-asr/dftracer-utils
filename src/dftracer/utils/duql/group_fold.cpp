#include <dftracer/utils/core/common/base64.h>
#include <dftracer/utils/duql/group_fold.h>
#include <dftracer/utils/duql/vectorize.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "../dataframe/batch_ops.h"

namespace dftracer::utils::duql {

namespace {

namespace df = dftracer::utils::dataframe;
using df::TypeId;

constexpr double SKETCH_ACCURACY = 0.01;

bool is_signed(TypeId t) {
    return t == TypeId::Int8 || t == TypeId::Int16 || t == TypeId::Int32 ||
           t == TypeId::Int64 || t == TypeId::Uint8 || t == TypeId::Uint16 ||
           t == TypeId::Uint32;
}

bool is_float(TypeId t) {
    return t == TypeId::Float16 || t == TypeId::Float32 || t == TypeId::Float64;
}

// `s` as the one type per kind the fold reads: Int64 (bools too), Uint64,
// Float64 or String; nullopt for a list or object.
std::optional<df::Series> normal(const df::Series& s) {
    const TypeId t = s.type();
    if (t == TypeId::Int64 || t == TypeId::Uint64 || t == TypeId::Float64 ||
        t == TypeId::String)
        return s.share();
    if (is_signed(t) || t == TypeId::Bool) return s.cast(TypeId::Int64);
    if (is_float(t)) return s.cast(TypeId::Float64);
    if (t == TypeId::LargeString) return s.cast(TypeId::String);
    return std::nullopt;
}

df::Series as_type(const df::Series& s, const df::DataType& type) {
    if (type.id == TypeId::Unknown || type.id == TypeId::List ||
        type.id == TypeId::Struct || s.type() == type.id)
        return s.share();
    return s.cast(type.id);
}

df::Series empty(const df::DataType& type) {
    if (type.id == TypeId::List)
        return df::Series::list(
            {0}, empty(type.fields.empty() ? df::scalar(TypeId::String)
                                           : type.fields.front().type));
    return df::Series::nulls(
        type.id == TypeId::Unknown ? TypeId::String : type.id, 0);
}

df::Series joined(const std::vector<df::Series>& parts,
                  const df::DataType& type) {
    if (parts.empty()) return empty(type);
    if (parts.size() == 1) return parts.front().share();
    std::vector<df::DataFrame> frames(parts.size());
    std::vector<const df::DataFrame*> ptrs;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        frames[i].names.push_back("v");
        frames[i].columns.push_back(parts[i].share());
        ptrs.push_back(&frames[i]);
    }
    return std::move(df::concat(ptrs).columns.front());
}

std::vector<std::uint8_t> validity(const std::vector<bool>& valid) {
    std::vector<std::uint8_t> bits((valid.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < valid.size(); ++i)
        if (valid[i]) bits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    return bits;
}

}  // namespace

// A `by` value in duql's order: bool, then number, then string.
struct GroupFold::Ord {
    std::uint8_t rank = 0;
    bool is_int = false;
    bool is_uint = false;
    std::int64_t i = 0;
    std::uint64_t u = 0;
    double d = 0;
    std::string s;

    static int sign(auto a, auto b) { return a < b ? -1 : (b < a ? 1 : 0); }

    int compare(const Ord& o) const {
        if (rank != o.rank) return sign(rank, o.rank);
        if (rank == 3) return s.compare(o.s) < 0 ? -1 : (s == o.s ? 0 : 1);
        if (rank == 1) return sign(i, o.i);
        if (is_int && o.is_int) return sign(i, o.i);
        if (is_uint && o.is_uint) return sign(u, o.u);
        if (is_int && o.is_uint)
            return i < 0 ? -1 : sign(static_cast<std::uint64_t>(i), o.u);
        if (is_uint && o.is_int)
            return o.i < 0 ? 1 : sign(u, static_cast<std::uint64_t>(o.i));
        return sign(d, o.d);
    }

    // Row `r` of the normal() column `c`, which holds bools as Int64 when
    // `boolean`; false when it has no value.
    bool read(const df::Series& c, std::int64_t r, bool boolean) {
        if (c.is_null(r)) return false;
        is_int = is_uint = false;
        switch (c.type()) {
            case TypeId::Int64:
                rank = boolean ? 1 : 2;
                is_int = true;
                i = c.data<std::int64_t>()[r];
                d = static_cast<double>(i);
                return true;
            case TypeId::Uint64:
                rank = 2;
                is_uint = true;
                u = c.data<std::uint64_t>()[r];
                d = static_cast<double>(u);
                return true;
            case TypeId::Float64:
                rank = 2;
                d = c.data<double>()[r];
                return !std::isnan(d);
            case TypeId::String:
                rank = 3;
                s.assign(c.string_at(r));
                return true;
            default:
                return false;
        }
    }
};

struct GroupFold::Arg {
    Ord best;
    std::int64_t at = -1;
    bool has = false;
};

GroupFold::GroupFold(std::vector<df::DataType> key_types,
                     std::vector<FoldAgg> aggs)
    : key_types_(std::move(key_types)),
      aggs_(std::move(aggs)),
      key_parts_(key_types_.size()),
      distinct_(aggs_.size()),
      parts_(aggs_.size()),
      part_rows_(aggs_.size(), 0),
      members_(aggs_.size()),
      args_(aggs_.size()),
      sketches_(aggs_.size()) {
    std::vector<df::AggSpec> specs;
    for (std::size_t a = 0; a < aggs_.size(); ++a)
        if (aggs_[a].op == FoldOp::ENGINE)
            specs.push_back({aggs_[a].engine, aggs_[a].in,
                             "__duql_fold_" + std::to_string(a), aggs_[a].param,
                             -1});
    if (!specs.empty()) engine_ = df::agg_new(std::move(specs));
}

GroupFold::~GroupFold() = default;

void GroupFold::add(df::DataFrame batch) {
    batch = flat_frame(std::move(batch));
    const std::int64_t n = batch.num_rows();
    if (n == 0) return;
    const std::size_t n_keys = key_types_.size();
    std::vector<std::int64_t> gid(static_cast<std::size_t>(n));
    std::vector<std::int64_t> fresh;
    std::string key;
    for (std::int64_t r = 0; r < n; ++r) {
        key.clear();
        for (std::size_t k = 0; k < n_keys; ++k) {
            const df::Series& c = batch.columns[k];
            if (append_cell_key(key, c, r)) continue;
            if (c.is_null(r)) {
                key += '\0';
            } else if (is_float(c.type())) {
                key += '\1';
            } else {
                throw std::invalid_argument(
                    "group: key '" + batch.names[k] +
                    "' holds a list or object; group by a scalar");
            }
        }
        auto [it, added] =
            groups_.try_emplace(key, static_cast<std::int64_t>(groups_.size()));
        if (added) fresh.push_back(r);
        gid[static_cast<std::size_t>(r)] = it->second;
    }
    for (std::size_t k = 0; k < n_keys && !fresh.empty(); ++k)
        key_parts_[k].push_back(
            as_type(batch.columns[k], key_types_[k]).take(fresh));
    const auto groups = static_cast<std::size_t>(groups_.size());
    if (engine_) {
        const df::Series g = df::Series::flat_i64(gid.data(), n);
        std::vector<const df::Series*> values;
        for (const auto& c : batch.columns) values.push_back(&c);
        df::agg_set_row_base(*engine_, rows_);
        df::agg_accumulate(*engine_, {&g}, values);
    }
    for (std::size_t a = 0; a < aggs_.size(); ++a) {
        const FoldAgg& f = aggs_[a];
        if (f.op == FoldOp::ENGINE) continue;
        const df::Series& in = batch.columns[static_cast<std::size_t>(f.in)];
        switch (f.op) {
            case FoldOp::COUNT_DISTINCT: {
                auto& sets = distinct_[a];
                sets.resize(groups);
                std::string v;
                for (std::int64_t r = 0; r < n; ++r) {
                    v.clear();
                    if (append_cell_key(v, in, r))
                        sets[static_cast<std::size_t>(
                                 gid[static_cast<std::size_t>(r)])]
                            .insert(v);
                }
                break;
            }
            case FoldOp::COLLECT: {
                auto& members = members_[a];
                members.resize(groups);
                std::vector<std::int64_t> rows;
                for (std::int64_t r = 0; r < n; ++r) {
                    if (in.is_null(r)) continue;
                    members[static_cast<std::size_t>(
                                gid[static_cast<std::size_t>(r)])]
                        .push_back(part_rows_[a] +
                                   static_cast<std::int64_t>(rows.size()));
                    rows.push_back(r);
                }
                if (rows.empty()) break;
                parts_[a].push_back(
                    as_type(in, f.type.fields.front().type).take(rows));
                part_rows_[a] += static_cast<std::int64_t>(rows.size());
                break;
            }
            case FoldOp::ARGMAX:
            case FoldOp::ARGMIN:
                arg(a, in, batch.columns[static_cast<std::size_t>(f.by)], gid);
                break;
            case FoldOp::SKETCH:
            case FoldOp::MERGE:
            case FoldOp::MERGE_QUANTILE:
                sketches(a, in, gid);
                break;
            case FoldOp::ENGINE:
                break;
        }
    }
    rows_ += n;
}

void GroupFold::arg(std::size_t a, const df::Series& in, const df::Series& by,
                    const std::vector<std::int64_t>& gid) {
    auto& state = args_[a];
    state.resize(static_cast<std::size_t>(groups_.size()));
    const auto b = normal(by);
    if (!b) return;
    const bool boolean = by.type() == TypeId::Bool;
    const int want = aggs_[a].op == FoldOp::ARGMAX ? 1 : -1;
    std::vector<std::int64_t> touched;
    std::vector<std::int64_t> row_of(state.size(), -1);
    Ord o;
    for (std::int64_t r = 0; r < b->length(); ++r) {
        if (!o.read(*b, r, boolean)) continue;
        const auto g =
            static_cast<std::size_t>(gid[static_cast<std::size_t>(r)]);
        Arg& s = state[g];
        if (s.has && o.compare(s.best) != want) continue;
        s.best = o;
        s.has = true;
        if (row_of[g] < 0) touched.push_back(static_cast<std::int64_t>(g));
        row_of[g] = r;
    }
    if (touched.empty()) return;
    std::vector<std::int64_t> rows;
    for (const std::int64_t g : touched) {
        state[static_cast<std::size_t>(g)].at =
            part_rows_[a] + static_cast<std::int64_t>(rows.size());
        rows.push_back(row_of[static_cast<std::size_t>(g)]);
    }
    parts_[a].push_back(as_type(in, aggs_[a].type).take(rows));
    part_rows_[a] += static_cast<std::int64_t>(rows.size());
}

void GroupFold::sketches(std::size_t a, const df::Series& in,
                         const std::vector<std::int64_t>& gid) {
    auto& state = sketches_[a];
    state.resize(static_cast<std::size_t>(groups_.size()));
    const std::int64_t n = in.length();
    if (aggs_[a].op == FoldOp::SKETCH) {
        const TypeId t = in.type();
        if (!is_signed(t) && !is_float(t) && t != TypeId::Uint64) return;
        const df::Series d = in.cast(TypeId::Float64);
        const double* v = d.data<double>();
        for (std::int64_t r = 0; r < n; ++r) {
            if (d.is_null(r) || std::isnan(v[r])) continue;
            auto& s = state[static_cast<std::size_t>(
                gid[static_cast<std::size_t>(r)])];
            if (!s) s = std::make_unique<Sketch>(SKETCH_ACCURACY);
            s->add(v[r]);
        }
        return;
    }
    if (in.type() != TypeId::String) return;
    for (std::int64_t r = 0; r < n; ++r) {
        if (in.is_null(r)) continue;
        const auto bytes = base64_decode(in.string_at(r));
        if (!bytes) continue;
        Sketch one = Sketch::deserialize(
            reinterpret_cast<const std::uint8_t*>(bytes->data()),
            bytes->size());
        if (one.empty()) continue;
        auto& s =
            state[static_cast<std::size_t>(gid[static_cast<std::size_t>(r)])];
        // Sketches of another accuracy do not merge; they count as unknown.
        if (s && std::abs(s->log_gamma() - one.log_gamma()) > 1e-12) continue;
        if (!s)
            s = std::make_unique<Sketch>(std::move(one));
        else
            s->merge(one);
    }
}

df::DataFrame GroupFold::finish(std::vector<std::string> names) {
    const auto groups = static_cast<std::int64_t>(groups_.size());
    const auto count = static_cast<std::size_t>(groups);
    df::DataFrame out;
    out.names = std::move(names);
    if (groups == 0) {
        for (const auto& t : key_types_) out.columns.push_back(empty(t));
        for (const auto& a : aggs_) out.columns.push_back(empty(a.type));
        return out;
    }
    for (std::size_t k = 0; k < key_types_.size(); ++k)
        out.columns.push_back(joined(key_parts_[k], key_types_[k]));
    std::optional<df::DataFrame> engine;
    std::vector<std::int64_t> order;
    if (engine_) {
        engine =
            df::agg_finalize(*engine_, std::vector<std::string>{"__duql_gid"});
        order.assign(count, -1);
        const df::Series g = engine->columns.front().cast(TypeId::Int64);
        for (std::int64_t r = 0; r < g.length(); ++r)
            order[static_cast<std::size_t>(g.data<std::int64_t>()[r])] = r;
    }
    std::size_t next_engine = 1;
    for (std::size_t a = 0; a < aggs_.size(); ++a) {
        const FoldAgg& f = aggs_[a];
        switch (f.op) {
            case FoldOp::ENGINE:
                out.columns.push_back(
                    engine->columns[next_engine++].take(order));
                break;
            case FoldOp::COUNT_DISTINCT: {
                std::vector<std::int64_t> v(count, 0);
                for (std::size_t g = 0; g < distinct_[a].size(); ++g)
                    v[g] = static_cast<std::int64_t>(distinct_[a][g].size());
                out.columns.push_back(df::Series::flat_i64(v.data(), groups));
                break;
            }
            case FoldOp::COLLECT: {
                auto& members = members_[a];
                members.resize(count);
                std::vector<std::int32_t> offsets{0};
                std::vector<std::int64_t> rows;
                for (const auto& m : members) {
                    rows.insert(rows.end(), m.begin(), m.end());
                    offsets.push_back(static_cast<std::int32_t>(rows.size()));
                }
                out.columns.push_back(df::Series::list(
                    offsets,
                    joined(parts_[a], f.type.fields.front().type).take(rows)));
                break;
            }
            case FoldOp::ARGMAX:
            case FoldOp::ARGMIN: {
                std::vector<std::int64_t> rows(count, -1);
                for (std::size_t g = 0; g < args_[a].size(); ++g)
                    rows[g] = args_[a][g].at;
                out.columns.push_back(joined(parts_[a], f.type).take(rows));
                break;
            }
            case FoldOp::SKETCH:
            case FoldOp::MERGE:
            case FoldOp::MERGE_QUANTILE: {
                auto& state = sketches_[a];
                state.resize(count);
                std::vector<bool> valid(count, false);
                if (f.op == FoldOp::MERGE_QUANTILE) {
                    std::vector<double> v(count, 0);
                    for (std::size_t g = 0; g < count; ++g) {
                        if (!state[g]) continue;
                        v[g] = std::clamp(state[g]->quantile(f.param),
                                          state[g]->min(), state[g]->max());
                        valid[g] = true;
                    }
                    const auto bits = validity(valid);
                    out.columns.push_back(
                        df::Series::flat_f64(v.data(), groups, bits.data()));
                    break;
                }
                std::vector<std::string> text(count);
                std::vector<std::uint8_t> buf;
                for (std::size_t g = 0; g < count; ++g) {
                    if (!state[g]) continue;
                    buf.clear();
                    state[g]->serialize_into(buf);
                    text[g] = base64_encode(buf.data(), buf.size());
                    valid[g] = true;
                }
                const std::vector<std::string_view> views(text.begin(),
                                                          text.end());
                const auto bits = validity(valid);
                out.columns.push_back(df::Series::strings(views, bits.data()));
                break;
            }
        }
    }
    return out;
}

}  // namespace dftracer::utils::duql
