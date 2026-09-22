#include <dftracer/utils/dataframe/agg/detail.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::dataframe {

std::int64_t agg_num_groups(const AggState& st) { return st.ngroups(); }

const std::vector<AggSpec>& agg_specs(const AggState& st) { return st.specs; }

std::vector<std::string> agg_group_key(const AggState& st, std::int64_t g) {
    std::vector<std::string> out;
    out.reserve(st.nkeys);
    for (std::size_t k = 0; k < st.nkeys; ++k) {
        if (st.nkey_cols[k][static_cast<std::size_t>(g)]) {
            out.emplace_back();
            continue;
        }
        if (st.key_is_bytes[k]) {
            out.push_back(st.skey_cols[k][static_cast<std::size_t>(g)]);
            continue;
        }
        const std::int64_t bits = st.ikey_cols[k][static_cast<std::size_t>(g)];
        const FieldStatDomain kd =
            k < st.key_domain.size() ? st.key_domain[k] : FieldStatDomain::I64;
        if (kd == FieldStatDomain::U64)
            out.push_back(std::to_string(std::bit_cast<std::uint64_t>(bits)));
        else if (kd == FieldStatDomain::F64)
            out.push_back(std::to_string(std::bit_cast<double>(bits)));
        else
            out.push_back(std::to_string(bits));
    }
    return out;
}

std::size_t agg_approx_bytes(const AggState& st) {
    std::size_t total = 0;
    for (std::size_t k = 0; k < st.nkeys; ++k) {
        if (st.key_is_bytes[k])
            for (const std::string& v : st.skey_cols[k])
                total += v.size() + sizeof(std::string);
        else
            total += st.ikey_cols[k].size() * sizeof(std::int64_t);
        total += st.nkey_cols[k].size();
    }
    total += st.counts.size() * sizeof(std::uint64_t);
    total += st.fstats.size() * sizeof(FieldStat);
    total += st.light.size() * sizeof(AggState::LightStat);
    if (st.has_fl) {
        total +=
            (st.fl_first.size() + st.fl_last.size()) * sizeof(std::uint64_t);
        total += (st.fl_first_idx.size() + st.fl_last_idx.size()) *
                 sizeof(std::int64_t);
        for (const std::string& v : st.fl_first_s) total += v.size();
        for (const std::string& v : st.fl_last_s) total += v.size();
    }
    if (st.has_sketch)
        for (const DDSketch& sk : st.sketches)
            total += sk.bins().size() * 24 + 64;
    if (st.has_arg) {
        total += st.arg_by.size() * sizeof(double) + st.arg_has.size();
        for (const std::string& v : st.arg_repr) total += v.size();
    }
    if (st.has_bitor) total += st.bitor_acc.size() * sizeof(std::uint64_t);
    if (st.has_prod) total += st.prod_acc.size() * sizeof(double);
    if (st.has_kmv)
        for (const KmvMap& m : st.kmv)
            for (const auto& [h, v] : m)
                total += sizeof(std::uint64_t) + v.size() + 48;
    if (st.has_lst)
        for (const ListItems& items : st.lst)
            for (const auto& [by, repr] : items)
                total += sizeof(double) + repr.size() + sizeof(std::string);
    if (st.has_ss)
        for (const SpaceSavingMap& m : st.ss_counters)
            for (const auto& [v, c] : m)
                total += v.size() + sizeof(std::uint64_t) + 48;
    if (st.has_co)
        total += (st.co_n.size() + st.co_sx.size() + st.co_sy.size() +
                  st.co_sxx.size() + st.co_syy.size() + st.co_sxy.size()) *
                 sizeof(double);
    if (st.has_set)
        for (const AggState::StringSet& gset : st.sets)
            for (const std::string& v : gset) total += v.size() + 32;
    if (st.has_occ) {
        total += (st.occ_total.size() + st.occ_ts.size() + st.occ_te.size()) *
                 sizeof(std::uint64_t);
        for (const AggState::OccDeltas& d : st.occ_deltas)
            total += d.size() * (sizeof(std::uint64_t) + sizeof(std::int64_t));
    }
    if (st.has_dyn) {
        for (const std::map<std::string, FieldStat>& gmap : st.dyn_fs)
            for (const auto& [name, fs] : gmap)
                total += name.size() + sizeof(FieldStat) + 48;
        if (st.dyn_has_sketch)
            for (const std::map<std::string, DDSketch>& gsk : st.dyn_sketch)
                for (const auto& [name, sk] : gsk)
                    total += name.size() + sk.bins().size() * 24 + 64;
    }
    return total;
}

DataType agg_output_type(AggOp op, TypeId value_type) {
    auto widened = [&] {
        switch (col_domain(value_type)) {
            case FieldStatDomain::U64:
                return scalar(TypeId::Uint64);
            case FieldStatDomain::F64:
                return scalar(TypeId::Float64);
            default:
                return scalar(TypeId::Int64);
        }
    };
    switch (op) {
        case AggOp::Count:
        case AggOp::CountValid:
        case AggOp::Distinct:
            return scalar(TypeId::Int64);
        case AggOp::Sum:
        case AggOp::Min:
        case AggOp::Max:
            return widened();
        case AggOp::First:
        case AggOp::Last:
            return value_type == TypeId::String ? scalar(TypeId::String)
                                                : widened();
        case AggOp::Mean:
        case AggOp::Var:
        case AggOp::Std:
        case AggOp::Skew:
        case AggOp::Kurt:
        case AggOp::Pct:
        case AggOp::SumSq:
        case AggOp::Busy:
        case AggOp::Concurrency:
        case AggOp::Utilization:
        case AggOp::Active:
        case AggOp::Corr:
        case AggOp::CovarPop:
        case AggOp::CovarSamp:
        case AggOp::RegrSlope:
        case AggOp::RegrIntercept:
        case AggOp::RegrR2:
        case AggOp::Prod:
            return scalar(TypeId::Float64);
        case AggOp::ArgMax:
        case AggOp::ArgMin:
        case AggOp::SetUnion:
            return scalar(TypeId::String);
        case AggOp::BitOr:
            return scalar(TypeId::Uint64);
        case AggOp::Hist:
            return list_of(
                struct_of({Field{"lo", scalar(TypeId::Float64), true},
                           Field{"hi", scalar(TypeId::Float64), true},
                           Field{"count", scalar(TypeId::Uint64), true}}));
        case AggOp::ListSorted:
        case AggOp::TopK:
        case AggOp::BottomK:
        case AggOp::Sample:
            return list_of(scalar(TypeId::String));
        case AggOp::ApproxTopK:
            return list_of(
                struct_of({Field{"value", scalar(TypeId::String), true},
                           Field{"count", scalar(TypeId::Uint64), true}}));
    }
    return scalar(TypeId::Unknown);
}

int agg_key_cmp(const AggState& a, std::int64_t ga, const AggState& b,
                std::int64_t gb) {
    for (std::size_t k = 0; k < a.nkeys; ++k) {
        // A null key sorts last.
        const bool xn = a.nkey_cols[k][static_cast<std::size_t>(ga)] != 0;
        const bool yn = b.nkey_cols[k][static_cast<std::size_t>(gb)] != 0;
        if (xn || yn) {
            if (xn && yn) continue;
            return xn ? 1 : -1;
        }
        if (a.key_is_bytes[k]) {
            const std::string& x = a.skey_cols[k][static_cast<std::size_t>(ga)];
            const std::string& y = b.skey_cols[k][static_cast<std::size_t>(gb)];
            if (x != y) return x < y ? -1 : 1;
        } else {
            const std::int64_t x = a.ikey_cols[k][static_cast<std::size_t>(ga)];
            const std::int64_t y = b.ikey_cols[k][static_cast<std::size_t>(gb)];
            if (x != y) return x < y ? -1 : 1;
        }
    }
    return 0;
}

}  // namespace dftracer::utils::dataframe
