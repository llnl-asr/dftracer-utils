#include <dftracer/utils/dataframe/agg/detail.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::dataframe {

namespace {

template <class T>
void put(std::string& s, T v) {
    s.append(reinterpret_cast<const char*>(&v), sizeof(T));
}
// Field by field: a raw copy would write the padding after `domain`.
void put(std::string& s, const FieldStat& f) {
    put(s, f.n);
    put(s, f.sum);
    put(s, f.min);
    put(s, f.max);
    put(s, static_cast<std::uint8_t>(f.domain));
    put(s, f.esum);
    put(s, f.emin);
    put(s, f.emax);
    put(s, f.sumsq);
    put(s, f.m3);
    put(s, f.m4);
}
void put_bytes(std::string& s, std::string_view b) {
    put(s, static_cast<std::uint64_t>(b.size()));
    s.append(b);
}
// A bounded reader: a blob from another process is untrusted bytes, so every
// read checks the end and every count is checked against the bytes left
// (each counted element costs at least one byte to read) before a container
// is sized by it. A short or corrupt blob throws std::invalid_argument.
struct Reader {
    const char* p;
    const char* end;
    std::size_t left() const { return static_cast<std::size_t>(end - p); }
    [[noreturn]] static void truncated() {
        throw std::invalid_argument(
            "agg_deserialize: the blob is truncated or corrupt");
    }
    void count(std::uint64_t n) const {
        if (n > left()) truncated();
    }
    template <class T>
    T get() {
        if (left() < sizeof(T)) truncated();
        T v;
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    FieldStat get_stat() {
        FieldStat f;
        f.n = get<std::uint64_t>();
        f.sum = get<double>();
        f.min = get<double>();
        f.max = get<double>();
        const std::uint8_t d = get<std::uint8_t>();
        if (d > static_cast<std::uint8_t>(FieldStatDomain::F64)) truncated();
        f.domain = static_cast<FieldStatDomain>(d);
        f.esum = get<std::int64_t>();
        f.emin = get<std::int64_t>();
        f.emax = get<std::int64_t>();
        f.sumsq = get<double>();
        f.m3 = get<double>();
        f.m4 = get<double>();
        return f;
    }
    std::string get_bytes() {
        std::uint64_t n = get<std::uint64_t>();
        count(n);
        std::string r(p, n);
        p += n;
        return r;
    }
    void skip(std::uint64_t n) {
        count(n);
        p += n;
    }
};

}  // namespace

std::string agg_serialize(const AggState& st_in) {
    const AggState& st = settled(st_in);
    std::string s;
    put(s, static_cast<std::uint32_t>(st.specs.size()));
    put(s, static_cast<std::uint32_t>(st.nkeys));
    for (char b : st.key_is_bytes) put(s, static_cast<std::uint8_t>(b));
    for (std::size_t k = 0; k < st.nkeys; ++k)
        put(s, static_cast<std::uint8_t>(k < st.key_domain.size()
                                             ? st.key_domain[k]
                                             : FieldStatDomain::I64));
    for (std::size_t k = 0; k < st.nkeys; ++k)
        put(s, static_cast<std::uint8_t>(
                   k < st.key_type.size() ? st.key_type[k] : TypeId::Int64));
    for (std::size_t k = 0; k < st.nkeys; ++k)
        put(s, static_cast<std::int32_t>(k < st.key_time_unit.size()
                                             ? st.key_time_unit[k]
                                             : TimeUnit::Micro));
    for (std::size_t k = 0; k < st.nkeys; ++k)
        put_bytes(
            s, k < st.key_timezone.size() ? st.key_timezone[k] : std::string());
    for (std::size_t k = 0; k < st.nkeys; ++k)
        put(s, k < st.key_byte_width.size() ? st.key_byte_width[k]
                                            : std::int32_t{0});
    for (std::size_t k = 0; k < st.nkeys; ++k)
        put(s, k < st.key_decimal_precision.size() ? st.key_decimal_precision[k]
                                                   : std::int32_t{0});
    for (std::size_t k = 0; k < st.nkeys; ++k)
        put(s, k < st.key_decimal_scale.size() ? st.key_decimal_scale[k]
                                               : std::int32_t{0});
    for (const AggSpec& sp : st.specs) {
        put(s, static_cast<std::int32_t>(sp.op));
        put(s, sp.value_col);
        put_bytes(s, sp.out);
        put(s, sp.param);
        put(s, sp.by_col);
    }
    put(s, static_cast<std::uint32_t>(st.nf));
    for (FieldStatDomain d : st.field_domain)
        put(s, static_cast<std::uint8_t>(d));
    put(s, static_cast<std::uint8_t>(st.has_fl ? 1 : 0));
    if (st.has_fl)
        for (std::size_t fj = 0; fj < st.nf; ++fj)
            put(s, static_cast<std::uint8_t>(
                       fj < st.field_is_str.size() ? st.field_is_str[fj] : 0));
    const std::int64_t ng = st.ngroups();
    put(s, ng);
    for (std::size_t k = 0; k < st.nkeys; ++k) {
        if (st.key_is_bytes[k])
            for (const std::string& v : st.skey_cols[k]) put_bytes(s, v);
        else
            for (std::int64_t v : st.ikey_cols[k]) put(s, v);
        for (std::uint8_t v : st.nkey_cols[k]) put(s, v);
    }
    for (std::uint64_t c : st.counts) put(s, c);
    for (const FieldStat& f : st.fstats) put(s, f);
    if (st.has_fl) {
        for (std::uint64_t b : st.fl_first) put(s, b);
        for (std::uint64_t b : st.fl_last) put(s, b);
        for (std::int64_t x : st.fl_first_idx) put(s, x);
        for (std::int64_t x : st.fl_last_idx) put(s, x);
        for (const std::string& v : st.fl_first_s) put_bytes(s, v);
        for (const std::string& v : st.fl_last_s) put_bytes(s, v);
    }
    if (st.has_sketch) {
        std::vector<std::uint8_t> blob;
        for (const DDSketch& sk : st.sketches) {
            sk.serialize_into(blob);
            put(s, static_cast<std::uint32_t>(blob.size()));
            s.append(reinterpret_cast<const char*>(blob.data()), blob.size());
        }
    }
    put(s, static_cast<std::uint8_t>(st.has_arg ? 1 : 0));
    if (st.has_arg) {
        for (double b : st.arg_by) put(s, b);
        for (char h : st.arg_has) put(s, static_cast<std::uint8_t>(h));
        for (const std::string& r : st.arg_repr) put_bytes(s, r);
    }
    put(s, static_cast<std::uint8_t>(st.has_set ? 1 : 0));
    if (st.has_set) {
        for (const AggState::StringSet& gset : st.sets) {
            put(s, static_cast<std::uint32_t>(gset.size()));
            std::vector<std::string_view> sorted(gset.begin(), gset.end());
            std::sort(sorted.begin(), sorted.end());
            for (const std::string_view v : sorted) put_bytes(s, v);
        }
    }
    put(s, static_cast<std::uint8_t>(st.has_occ ? 1 : 0));
    if (st.has_occ) {
        const std::size_t sz = static_cast<std::size_t>(ng) * st.n_occ;
        for (std::size_t i = 0; i < sz; ++i) put(s, st.occ_total[i]);
        for (std::size_t i = 0; i < sz; ++i) put(s, st.occ_ts[i]);
        for (std::size_t i = 0; i < sz; ++i) put(s, st.occ_te[i]);
        for (std::size_t i = 0; i < sz; ++i) {
            put(s, static_cast<std::uint64_t>(st.occ_deltas[i].size()));
            for (const auto& [t, dlt] : st.occ_deltas[i]) {
                put(s, t);
                put(s, dlt);
            }
        }
    }
    put(s, static_cast<std::uint8_t>(st.has_dyn ? 1 : 0));
    if (st.has_dyn) {
        put(s, static_cast<std::uint32_t>(st.dyn_specs.size()));
        for (const AggDynSpec& d : st.dyn_specs) {
            put(s, static_cast<std::int32_t>(d.op));
            put(s, d.param);
            put_bytes(s, d.out_prefix);
        }
        put(s, static_cast<std::uint8_t>(st.dyn_has_sketch ? 1 : 0));
        put(s, static_cast<std::uint32_t>(st.dyn_domain.size()));
        for (const auto& [name, dom] : st.dyn_domain) {
            put_bytes(s, name);
            put(s, static_cast<std::uint8_t>(dom));
        }
        for (std::int64_t g = 0; g < ng; ++g) {
            const std::map<std::string, FieldStat>& gmap =
                st.dyn_fs[static_cast<std::size_t>(g)];
            put(s, static_cast<std::uint64_t>(gmap.size()));
            for (const auto& [name, fs] : gmap) {
                put_bytes(s, name);
                put(s, fs);
            }
        }
        if (st.dyn_has_sketch) {
            std::vector<std::uint8_t> blob;
            for (std::int64_t g = 0; g < ng; ++g) {
                const std::map<std::string, DDSketch>& gsk =
                    st.dyn_sketch[static_cast<std::size_t>(g)];
                put(s, static_cast<std::uint64_t>(gsk.size()));
                for (const auto& [name, sk] : gsk) {
                    put_bytes(s, name);
                    sk.serialize_into(blob);
                    put(s, static_cast<std::uint32_t>(blob.size()));
                    s.append(reinterpret_cast<const char*>(blob.data()),
                             blob.size());
                }
            }
        }
    }
    put(s, static_cast<std::uint8_t>(st.has_bitor ? 1 : 0));
    if (st.has_bitor)
        for (std::uint64_t b : st.bitor_acc) put(s, b);
    put(s, static_cast<std::uint8_t>(st.has_prod ? 1 : 0));
    if (st.has_prod)
        for (double p : st.prod_acc) put(s, p);
    put(s, static_cast<std::uint8_t>(st.has_kmv ? 1 : 0));
    if (st.has_kmv) {
        for (const KmvMap& m : st.kmv) {
            put(s, static_cast<std::uint32_t>(m.size()));
            for (const auto& [h, v] : m) {
                put(s, h);
                put_bytes(s, v);
            }
        }
    }
    put(s, static_cast<std::uint8_t>(st.has_lst ? 1 : 0));
    if (st.has_lst) {
        for (const ListItems& items : st.lst) {
            put(s, static_cast<std::uint32_t>(items.size()));
            for (const auto& [by, repr] : items) {
                put(s, by);
                put_bytes(s, repr);
            }
        }
    }
    put(s, static_cast<std::uint8_t>(st.has_ss ? 1 : 0));
    if (st.has_ss) {
        for (const SpaceSavingMap& m : st.ss_counters) {
            put(s, static_cast<std::uint32_t>(m.size()));
            for (const auto& [v, c] : m) {
                put_bytes(s, v);
                put(s, c);
            }
        }
    }
    put(s, static_cast<std::uint8_t>(st.has_co ? 1 : 0));
    if (st.has_co) {
        const std::size_t sz = static_cast<std::size_t>(ng) * st.n_co;
        for (std::size_t i = 0; i < sz; ++i) put(s, st.co_n[i]);
        for (std::size_t i = 0; i < sz; ++i) put(s, st.co_sx[i]);
        for (std::size_t i = 0; i < sz; ++i) put(s, st.co_sy[i]);
        for (std::size_t i = 0; i < sz; ++i) put(s, st.co_sxx[i]);
        for (std::size_t i = 0; i < sz; ++i) put(s, st.co_syy[i]);
        for (std::size_t i = 0; i < sz; ++i) put(s, st.co_sxy[i]);
    }
    return s;
}

AggStatePtr agg_deserialize(const std::string& blob) {
    Reader r{blob.data(), blob.data() + blob.size()};
    AggStatePtr st(new AggState());
    const std::uint32_t ns = r.get<std::uint32_t>();
    const std::uint32_t nkeys = r.get<std::uint32_t>();
    st->nkeys = nkeys;
    r.count(nkeys);
    st->key_is_bytes.resize(nkeys);
    for (std::uint32_t k = 0; k < nkeys; ++k)
        st->key_is_bytes[k] = static_cast<char>(r.get<std::uint8_t>());
    r.count(nkeys);
    st->key_domain.resize(nkeys);
    for (std::uint32_t k = 0; k < nkeys; ++k) {
        const std::uint8_t d = r.get<std::uint8_t>();
        if (d > static_cast<std::uint8_t>(FieldStatDomain::F64))
            Reader::truncated();
        st->key_domain[k] = static_cast<FieldStatDomain>(d);
    }
    r.count(nkeys);
    st->key_type.resize(nkeys);
    for (std::uint32_t k = 0; k < nkeys; ++k) {
        const std::uint8_t t = r.get<std::uint8_t>();
        if (t > static_cast<std::uint8_t>(TypeId::Map)) Reader::truncated();
        st->key_type[k] = static_cast<TypeId>(t);
    }
    r.count(nkeys);
    st->key_time_unit.resize(nkeys);
    for (std::uint32_t k = 0; k < nkeys; ++k)
        st->key_time_unit[k] = static_cast<TimeUnit>(r.get<std::int32_t>());
    r.count(nkeys);
    st->key_timezone.resize(nkeys);
    for (std::uint32_t k = 0; k < nkeys; ++k)
        st->key_timezone[k] = r.get_bytes();
    r.count(nkeys);
    st->key_byte_width.resize(nkeys);
    for (std::uint32_t k = 0; k < nkeys; ++k)
        st->key_byte_width[k] = r.get<std::int32_t>();
    r.count(nkeys);
    st->key_decimal_precision.resize(nkeys);
    for (std::uint32_t k = 0; k < nkeys; ++k)
        st->key_decimal_precision[k] = r.get<std::int32_t>();
    r.count(nkeys);
    st->key_decimal_scale.resize(nkeys);
    for (std::uint32_t k = 0; k < nkeys; ++k)
        st->key_decimal_scale[k] = r.get<std::int32_t>();
    r.count(ns);
    st->specs.resize(ns);
    for (std::uint32_t i = 0; i < ns; ++i) {
        const std::int32_t op = r.get<std::int32_t>();
        if (op < 0 || op > static_cast<std::int32_t>(AggOp::Prod))
            Reader::truncated();
        st->specs[i].op = static_cast<AggOp>(op);
        st->specs[i].value_col = r.get<std::int32_t>();
        st->specs[i].out = r.get_bytes();
        st->specs[i].param = r.get<double>();
        st->specs[i].by_col = r.get<std::int32_t>();
    }
    st->init_layout();
    const std::uint32_t nf = r.get<std::uint32_t>();
    if (nf != st->nf) Reader::truncated();
    r.count(nf);
    st->field_domain.resize(nf);
    for (std::uint32_t i = 0; i < nf; ++i)
        st->field_domain[i] =
            static_cast<FieldStatDomain>(r.get<std::uint8_t>());
    if (st->has_fl != (r.get<std::uint8_t>() != 0)) Reader::truncated();
    if (st->has_fl) {
        r.count(nf);
        st->field_is_str.resize(nf);
        for (std::uint32_t i = 0; i < nf; ++i)
            st->field_is_str[i] = static_cast<char>(r.get<std::uint8_t>());
    }
    const std::int64_t ng = r.get<std::int64_t>();
    if (ng < 0) Reader::truncated();
    r.count(nkeys);
    st->ikey_cols.resize(nkeys);
    r.count(nkeys);
    st->skey_cols.resize(nkeys);
    r.count(nkeys);
    st->nkey_cols.resize(nkeys);
    for (std::uint32_t k = 0; k < nkeys; ++k) {
        if (st->key_is_bytes[k]) {
            r.count(static_cast<std::size_t>(ng));
            st->skey_cols[k].resize(static_cast<std::size_t>(ng));
            for (std::int64_t g = 0; g < ng; ++g)
                st->skey_cols[k][static_cast<std::size_t>(g)] = r.get_bytes();
        } else {
            r.count(static_cast<std::size_t>(ng));
            st->ikey_cols[k].resize(static_cast<std::size_t>(ng));
            for (std::int64_t g = 0; g < ng; ++g)
                st->ikey_cols[k][static_cast<std::size_t>(g)] =
                    r.get<std::int64_t>();
        }
        r.count(static_cast<std::size_t>(ng));
        st->nkey_cols[k].resize(static_cast<std::size_t>(ng));
        for (std::int64_t g = 0; g < ng; ++g)
            st->nkey_cols[k][static_cast<std::size_t>(g)] =
                r.get<std::uint8_t>();
    }
    st->rebuild_key_buckets(ng);
    r.count(static_cast<std::size_t>(ng));
    st->counts.resize(static_cast<std::size_t>(ng));
    for (std::int64_t g = 0; g < ng; ++g)
        st->counts[static_cast<std::size_t>(g)] = r.get<std::uint64_t>();
    r.count(static_cast<std::size_t>(ng) * nf);
    st->fstats.resize(static_cast<std::size_t>(ng) * nf);
    for (std::size_t i = 0; i < st->fstats.size(); ++i)
        st->fstats[i] = r.get_stat();
    if (st->has_fl) {
        const std::size_t sz = static_cast<std::size_t>(ng) * nf;
        r.count(sz);
        st->fl_first.resize(sz);
        r.count(sz);
        st->fl_last.resize(sz);
        r.count(sz);
        st->fl_first_idx.resize(sz);
        r.count(sz);
        st->fl_last_idx.resize(sz);
        r.count(sz);
        st->fl_first_s.resize(sz);
        r.count(sz);
        st->fl_last_s.resize(sz);
        for (std::size_t i = 0; i < sz; ++i)
            st->fl_first[i] = r.get<std::uint64_t>();
        for (std::size_t i = 0; i < sz; ++i)
            st->fl_last[i] = r.get<std::uint64_t>();
        for (std::size_t i = 0; i < sz; ++i)
            st->fl_first_idx[i] = r.get<std::int64_t>();
        for (std::size_t i = 0; i < sz; ++i)
            st->fl_last_idx[i] = r.get<std::int64_t>();
        for (std::size_t i = 0; i < sz; ++i) st->fl_first_s[i] = r.get_bytes();
        for (std::size_t i = 0; i < sz; ++i) st->fl_last_s[i] = r.get_bytes();
    }
    if (st->has_sketch) {
        const std::size_t sz = static_cast<std::size_t>(ng) * st->n_sketch;
        r.count(sz);
        st->sketches.reserve(sz);
        for (std::size_t i = 0; i < sz; ++i) {
            const std::uint32_t blen = r.get<std::uint32_t>();
            r.count(blen);
            st->sketches.push_back(DDSketch::deserialize(
                reinterpret_cast<const std::uint8_t*>(r.p), blen));
            r.p += blen;
        }
    }
    if (st->has_arg != (r.get<std::uint8_t>() != 0)) Reader::truncated();
    if (st->has_arg) {
        const std::size_t sz = static_cast<std::size_t>(ng) * st->n_arg;
        const std::size_t rz = static_cast<std::size_t>(ng) * st->n_arg_repr;
        r.count(sz);
        st->arg_by.resize(sz);
        r.count(sz);
        st->arg_has.resize(sz);
        r.count(rz);
        st->arg_repr.resize(rz);
        for (std::size_t i = 0; i < sz; ++i) st->arg_by[i] = r.get<double>();
        for (std::size_t i = 0; i < sz; ++i)
            st->arg_has[i] = static_cast<char>(r.get<std::uint8_t>());
        for (std::size_t i = 0; i < rz; ++i) st->arg_repr[i] = r.get_bytes();
    }
    if (st->has_set != (r.get<std::uint8_t>() != 0)) Reader::truncated();
    if (st->has_set) {
        const std::size_t sz = static_cast<std::size_t>(ng) * st->n_set;
        r.count(sz);
        st->sets.resize(sz);
        for (std::size_t i = 0; i < sz; ++i) {
            const std::uint32_t cnt = r.get<std::uint32_t>();
            for (std::uint32_t j = 0; j < cnt; ++j)
                st->sets[i].insert(r.get_bytes());
        }
    }
    if (st->has_occ != (r.get<std::uint8_t>() != 0)) Reader::truncated();
    if (st->has_occ) {
        const std::size_t sz = static_cast<std::size_t>(ng) * st->n_occ;
        r.count(sz);
        st->occ_total.resize(sz);
        r.count(sz);
        st->occ_ts.resize(sz);
        r.count(sz);
        st->occ_te.resize(sz);
        r.count(sz);
        st->occ_deltas.resize(sz);
        for (std::size_t i = 0; i < sz; ++i)
            st->occ_total[i] = r.get<std::uint64_t>();
        for (std::size_t i = 0; i < sz; ++i)
            st->occ_ts[i] = r.get<std::uint64_t>();
        for (std::size_t i = 0; i < sz; ++i)
            st->occ_te[i] = r.get<std::uint64_t>();
        for (std::size_t i = 0; i < sz; ++i) {
            const std::uint64_t cnt = r.get<std::uint64_t>();
            r.count(static_cast<std::size_t>(cnt));
            st->occ_deltas[i].reserve(static_cast<std::size_t>(cnt));
            for (std::uint64_t j = 0; j < cnt; ++j) {
                const std::uint64_t t = r.get<std::uint64_t>();
                const std::int64_t dlt = r.get<std::int64_t>();
                st->occ_deltas[i][t] = dlt;
            }
        }
    }
    // The dyn specs ride the blob (init_layout saw none yet), so the flag is
    // checked against them once they are read.
    st->has_dyn = r.get<std::uint8_t>() != 0;
    if (st->has_dyn) {
        const std::uint32_t nd = r.get<std::uint32_t>();
        if (nd == 0) Reader::truncated();
        r.count(nd);
        st->dyn_specs.resize(nd);
        for (std::uint32_t i = 0; i < nd; ++i) {
            const std::int32_t op = r.get<std::int32_t>();
            if (op < 0 || op > static_cast<std::int32_t>(AggOp::Prod))
                Reader::truncated();
            st->dyn_specs[i].op = static_cast<AggOp>(op);
            st->dyn_specs[i].param = r.get<double>();
            st->dyn_specs[i].out_prefix = r.get_bytes();
        }
        bool sketch = false;
        for (const AggDynSpec& d : st->dyn_specs)
            if (d.op == AggOp::Pct) sketch = true;
        if ((r.get<std::uint8_t>() != 0) != sketch) Reader::truncated();
        st->dyn_has_sketch = sketch;
        const std::uint32_t ndom = r.get<std::uint32_t>();
        for (std::uint32_t i = 0; i < ndom; ++i) {
            std::string name = r.get_bytes();
            st->dyn_domain.emplace(
                std::move(name),
                static_cast<FieldStatDomain>(r.get<std::uint8_t>()));
        }
        r.count(static_cast<std::size_t>(ng));
        st->dyn_fs.resize(static_cast<std::size_t>(ng));
        for (std::int64_t g = 0; g < ng; ++g) {
            const std::uint64_t cnt = r.get<std::uint64_t>();
            for (std::uint64_t j = 0; j < cnt; ++j) {
                std::string name = r.get_bytes();
                st->dyn_fs[static_cast<std::size_t>(g)].emplace(std::move(name),
                                                                r.get_stat());
            }
        }
        if (st->dyn_has_sketch) {
            r.count(static_cast<std::size_t>(ng));
            st->dyn_sketch.resize(static_cast<std::size_t>(ng));
            for (std::int64_t g = 0; g < ng; ++g) {
                const std::uint64_t cnt = r.get<std::uint64_t>();
                for (std::uint64_t j = 0; j < cnt; ++j) {
                    std::string name = r.get_bytes();
                    const std::uint32_t blen = r.get<std::uint32_t>();
                    r.count(blen);
                    st->dyn_sketch[static_cast<std::size_t>(g)].emplace(
                        std::move(name),
                        DDSketch::deserialize(
                            reinterpret_cast<const std::uint8_t*>(r.p), blen));
                    r.p += blen;
                }
            }
        }
    }
    if (st->has_bitor != (r.get<std::uint8_t>() != 0)) Reader::truncated();
    if (st->has_bitor) {
        const std::size_t sz = static_cast<std::size_t>(ng) * st->n_bitor;
        r.count(sz);
        st->bitor_acc.resize(sz);
        for (std::size_t i = 0; i < sz; ++i)
            st->bitor_acc[i] = r.get<std::uint64_t>();
    }
    if (st->has_prod != (r.get<std::uint8_t>() != 0)) Reader::truncated();
    if (st->has_prod) {
        const std::size_t sz = static_cast<std::size_t>(ng) * st->n_prod;
        r.count(sz);
        st->prod_acc.resize(sz);
        for (std::size_t i = 0; i < sz; ++i) st->prod_acc[i] = r.get<double>();
    }
    if (st->has_kmv != (r.get<std::uint8_t>() != 0)) Reader::truncated();
    if (st->has_kmv) {
        const std::size_t sz = static_cast<std::size_t>(ng) * st->n_kmv;
        r.count(sz);
        st->kmv.resize(sz);
        for (std::size_t i = 0; i < sz; ++i) {
            const std::uint32_t cnt = r.get<std::uint32_t>();
            for (std::uint32_t j = 0; j < cnt; ++j) {
                const std::uint64_t h = r.get<std::uint64_t>();
                st->kmv[i].emplace(h, r.get_bytes());
            }
        }
    }
    if (st->has_lst != (r.get<std::uint8_t>() != 0)) Reader::truncated();
    if (st->has_lst) {
        const std::size_t sz = static_cast<std::size_t>(ng) * st->n_lst;
        r.count(sz);
        st->lst.resize(sz);
        for (std::size_t i = 0; i < sz; ++i) {
            const std::uint32_t cnt = r.get<std::uint32_t>();
            r.count(cnt);
            st->lst[i].reserve(cnt);
            for (std::uint32_t j = 0; j < cnt; ++j) {
                const double by = r.get<double>();
                st->lst[i].emplace_back(by, r.get_bytes());
            }
        }
    }
    if (st->has_ss != (r.get<std::uint8_t>() != 0)) Reader::truncated();
    if (st->has_ss) {
        const std::size_t sz = static_cast<std::size_t>(ng) * st->n_ss;
        r.count(sz);
        st->ss_counters.resize(sz);
        for (std::size_t i = 0; i < sz; ++i) {
            const std::uint32_t cnt = r.get<std::uint32_t>();
            for (std::uint32_t j = 0; j < cnt; ++j) {
                std::string v = r.get_bytes();
                st->ss_counters[i].emplace(std::move(v),
                                           r.get<std::uint64_t>());
            }
        }
    }
    if (st->has_co != (r.get<std::uint8_t>() != 0)) Reader::truncated();
    if (st->has_co) {
        const std::size_t sz = static_cast<std::size_t>(ng) * st->n_co;
        r.count(sz);
        st->co_n.resize(sz);
        r.count(sz);
        st->co_sx.resize(sz);
        r.count(sz);
        st->co_sy.resize(sz);
        r.count(sz);
        st->co_sxx.resize(sz);
        r.count(sz);
        st->co_syy.resize(sz);
        r.count(sz);
        st->co_sxy.resize(sz);
        for (std::size_t i = 0; i < sz; ++i) st->co_n[i] = r.get<double>();
        for (std::size_t i = 0; i < sz; ++i) st->co_sx[i] = r.get<double>();
        for (std::size_t i = 0; i < sz; ++i) st->co_sy[i] = r.get<double>();
        for (std::size_t i = 0; i < sz; ++i) st->co_sxx[i] = r.get<double>();
        for (std::size_t i = 0; i < sz; ++i) st->co_syy[i] = r.get<double>();
        for (std::size_t i = 0; i < sz; ++i) st->co_sxy[i] = r.get<double>();
    }
    st->inited = true;
    return st;
}

}  // namespace dftracer::utils::dataframe
