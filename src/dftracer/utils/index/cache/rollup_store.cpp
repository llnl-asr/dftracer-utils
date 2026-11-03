// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/internal/expr_handle.h>  // expr_canonical
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/cache/cache_store.h>
#include <dftracer/utils/index/cache/rollup_store.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/utilities/serialization/binary_codec.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace dftracer::utils::index::cache {

namespace codec = utilities::serialization;
namespace dataframe = dftracer::utils::dataframe;

namespace {

// Every plan field except group_by - the basis shared by plan_signature and
// rest_signature.
// Part of every rollup key: a rollup persisted under another identity is never
// matched (it is recomputed instead).
constexpr dstd::StringView ROLLUP_FORMAT = "3";

constexpr dstd::StringView DESC_KEY = dstd::StringView("\x00", 1);
constexpr dstd::StringView ROW_TAG = "\x01";
// The size of an entry counts the signature that names it.
constexpr std::uint64_t SIG_BYTES = 8;

// Everything that decides a group key's values: a transform coarsens them, so
// dirname(file_path) and file_path are different keys.
dstd::String key_identity(const trace::views::GroupKey& gk) {
    dstd::String id{dstd::StringView(gk.arg)};
    if (gk.transform == trace::views::GroupKey::Transform::None) return id;
    id.push_back('\x1f');
    id += dstd::to_string(static_cast<int>(gk.transform));
    for (const auto& a : gk.transform_args) {
        id.push_back('\x1f');
        id.append(a);
    }
    return id;
}

void add_rest_fields(dstd::String& sig,
                     const trace::views::detail::ViewPlan& plan) {
    auto add = [&](dstd::StringView s) {
        sig.append(s);
        sig.push_back('\0');
    };
    add(ROLLUP_FORMAT);
    // Dictionaries of the record schema resolve `resolved.*` fields, so a
    // rollup made under one schema must not serve another.
    if (!plan.record_schema)
        throw common::DFTUtilsException(
            common::ErrorCode::InvalidArgument,
            "rollup signature: the plan's record schema "
            "is not resolved");
    add(dstd::to_string(plan.record_schema->params_hash()));
    dstd::Vector<const trace::views::ViewFile*> files;
    files.reserve(plan.files.size());
    for (const auto& f : plan.files) files.push_back(&f);
    std::sort(files.begin(), files.end(),
              [](auto* a, auto* b) { return a->file_path < b->file_path; });
    for (auto* f : files) {
        add(f->file_path);
        dftracer::utils::common::InputFileStat st;
        const bool ok = dftracer::utils::common::stat_input(f->file_path, st);
        add(dstd::to_string(ok ? st.size : 0));
        add(dstd::to_string(static_cast<long long>(ok ? st.mtime : 0)));
    }
    add(plan.query ? plan.query->source() : "");
    for (const trace::views::detail::ComputedColumn& c : plan.computed) {
        add(c.name);
        add(dataframe::expr_canonical(c.expr));
        for (const auto& in : c.inputs) add(in);
    }
    add(dstd::to_string(static_cast<int>(plan.phase)));
    // time_bucket_us is intentionally NOT part of the rest basis: a coarser
    // query can be served by re-bucketing a finer rollup, so buckets must not
    // gate subsumption. It lands in plan_signature (row-key identity) only.
    add(dstd::to_string(plan.time_scale));
    if (plan.time_range) {
        add(dstd::to_string(plan.time_range->first));
        add(dstd::to_string(plan.time_range->second));
    }
    for (const auto& a : plan.agg) {
        add(dstd::to_string(static_cast<int>(a.op)));
        add(a.field);
        add(a.out_name);
        add(a.by);
    }
    // Bucket alignment shifts every bucket boundary, so it partitions the
    // result grid: a min/origin-aligned query must not reuse an absolute
    // rollup. Re-bucketing a coarser query from a finer rollup assumes
    // 0-aligned flooring, so for an aligned plan pin the exact width too (no
    // re-cut).
    add(dstd::to_string(plan.bucket_origin_us));
    add(plan.bucket_origin_min ? "1" : "0");
    if (plan.bucket_origin_us || plan.bucket_origin_min)
        add("bw=" + dstd::to_string(plan.time_bucket_us));
    add(plan.auto_numeric_metrics ? "1" : "0");
    // Whether a per-arg quantile sketch was collected: an MV built without it
    // cannot serve a dyn Pct query, so it must not be reused for one. The dyn
    // FieldStat serves every other reduction, so those need no signature bump.
    bool dyn_sketch = false;
    for (const auto& r : plan.numeric_arg_aggs)
        if (r.op == trace::views::AggOp::Pct) {
            dyn_sketch = true;
            break;
        }
    add(dyn_sketch ? "1" : "0");
    // On-disk rollup format tag: a persisted rollup of another layout lands
    // under a different signature and is recomputed, never misread.
    add("accumfmt2");
    for (const auto& s : plan.select) add(s);
}

}  // namespace

dstd::SharedPtr<CacheStore> open_rollup_db(dstd::StringView path,
                                           CacheStore::Mode mode) {
    return CacheStore::open(path, mode);
}

void persist_rollup(CacheStore& db, std::uint64_t sig, std::uint64_t rest_sig,
                    std::uint64_t time_bucket_us,
                    dstd::Span<const trace::views::GroupKey> group_by,
                    const dataframe::AggState& state) {
    const std::int64_t ng = dataframe::agg_num_groups(state);
    // agg_group_key renders a null key as "", so the finalized key columns
    // tell a null key apart from an empty one.
    const dataframe::DataFrame finalized = dataframe::agg_finalize(
        state, std::vector<std::string>{});  // keep std: agg_finalize signature
    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> entries;
    entries.reserve(static_cast<std::size_t>(ng) + 1);
    std::uint64_t bytes = 0;
    dstd::String row_key;
    for (std::int64_t g = 0; g < ng; ++g) {
        // Row key = the group's composite key, each part tagged null or
        // value, a stable identity so a re-persist of the same group
        // overwrites in place.
        row_key.assign(ROW_TAG);
        const auto parts = dataframe::agg_group_key(state, g);
        for (std::size_t k = 0; k < parts.size(); ++k) {
            if (finalized.columns[k].is_null(g)) {
                row_key.push_back('\x00');
            } else {
                row_key.push_back('\x01');
                row_key.append(parts[k]);
            }
            row_key += trace::views::detail::GROUP_SEP;
        }
        const auto serialized =
            dataframe::agg_serialize(*dataframe::agg_extract_group(state, g));
        dstd::String blob{dstd::StringView(serialized)};
        bytes += SIG_BYTES + row_key.size() + blob.size();
        entries.emplace_back(row_key, std::move(blob));
    }
    // Shape descriptor the planner matches against: rest hash + this rollup's
    // time bucket (for coarsening) + this view's grouping (kind + key identity
    // per key, in order).
    dstd::String desc;
    dstd::append_u64_be(desc, rest_sig);
    dstd::append_u64_be(desc, time_bucket_us);
    dstd::append_u32_be(desc, static_cast<std::uint32_t>(group_by.size()));
    for (const auto& gk : group_by) {
        dstd::append_u32_be(desc, static_cast<std::uint32_t>(gk.kind));
        codec::put_str(desc, key_identity(gk));
    }
    bytes += 2 * (1 + SIG_BYTES) + desc.size() + USAGE_BYTES;
    entries.emplace_back(DESC_KEY, std::move(desc));
    db.put(sig, entries, bytes);
}

bool rollup_exists(const CacheStore& db, std::uint64_t sig) {
    return db.get(sig, DESC_KEY).has_value();
}

dataframe::AggStatePtr read_rollup(const CacheStore& db, std::uint64_t sig) {
    dataframe::AggStatePtr merged;
    db.scan(sig, ROW_TAG, [&](dstd::StringView, dstd::StringView blob) {
        auto one = dataframe::agg_deserialize(
            std::string(blob));  // keep std: agg_deserialize takes std::string
        if (!merged)
            merged = std::move(one);
        else
            dataframe::agg_merge(*merged, *one);
    });
    return merged;
}

std::uint64_t rest_signature(const trace::views::detail::ViewPlan& plan) {
    dstd::String sig;
    add_rest_fields(sig, plan);
    return dftracer::utils::dstd::hash::fnv1a_hash(sig);
}

std::uint64_t plan_signature(const trace::views::detail::ViewPlan& plan) {
    dstd::String sig;
    add_rest_fields(sig, plan);
    sig.append(dstd::to_string(plan.time_bucket_us));
    sig.push_back('\0');
    for (const auto& g : plan.group_by) {
        sig.append(dstd::to_string(static_cast<int>(g.kind)));
        sig.push_back('\0');
        sig.append(key_identity(g));
        sig.push_back('\0');
    }
    return dftracer::utils::dstd::hash::fnv1a_hash(sig);
}

dstd::Optional<dataframe::DataFrame> find_subsuming_rollup(
    CacheStore& db, const trace::views::detail::ViewPlan& plan) {
    const std::uint64_t q_rest = rest_signature(plan);

    // Pick the tightest subsuming rollup: fewest group dims means fewest rows
    // to re-aggregate, and an exact match (same dims as the query) is optimal.
    std::uint64_t best_sig = 0;
    std::uint64_t best_bucket = 0;
    dstd::Vector<trace::views::GroupKey> best_gb;
    bool have_best = false;

    db.scan_all(
        DESC_KEY, ROW_TAG,
        [&](std::uint64_t sig, dstd::StringView k, dstd::StringView v) {
            if (k != DESC_KEY) return true;
            if (v.size() < 20) return true;
            codec::BinaryReader br(v);
            if (br.be64() != q_rest) return true;
            const std::uint64_t r_bucket = br.be64();

            // A query with a time bucket is served only by a rollup whose
            // bucket evenly divides it. A query without a bucket matches any
            // rollup. A finer query than the rollup is unserviceable.
            if (plan.time_bucket_us > 0 &&
                (r_bucket == 0 || plan.time_bucket_us % r_bucket != 0))
                return true;

            dstd::Vector<trace::views::GroupKey> r_gb;
            const std::uint32_t n = br.be32();
            r_gb.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                trace::views::GroupKey gk;
                gk.kind = static_cast<trace::views::GroupKey::Kind>(br.be32());
                gk.arg = dstd::String(br.str());
                r_gb.push_back(std::move(gk));
            }
            if (have_best && r_gb.size() >= best_gb.size()) return true;

            bool subset = true;
            for (const auto& qg : plan.group_by) {
                bool found = false;
                for (const auto& rg : r_gb)
                    if (rg.kind == qg.kind &&
                        dstd::StringView(rg.arg) == key_identity(qg)) {
                        found = true;
                        break;
                    }
                if (!found) {
                    subset = false;
                    break;
                }
            }
            if (!subset) return true;

            best_sig = sig;
            best_bucket = r_bucket;
            best_gb = std::move(r_gb);
            have_best = true;
            if (best_gb.size() == plan.group_by.size() &&
                best_bucket == plan.time_bucket_us)
                return false;  // exact: optimal
            return true;
        });

    if (!have_best) return dstd::nullopt;
    auto fine = read_rollup(db, best_sig);
    if (!fine) return dstd::nullopt;
    db.touch(best_sig);

    // Map the query's grouping onto the stored state's key columns. The state
    // key layout is [time_bucket?, best_gb...]; keep the query's keys in query
    // order (dropping the bucket when the query has none, or re-flooring it to
    // the query's coarser grain otherwise).
    const std::size_t src_off = best_bucket > 0 ? 1 : 0;
    std::vector<std::int32_t> keep;  // keep std: agg_regroup signature
    keep.reserve((plan.time_bucket_us > 0 ? 1 : 0) + plan.group_by.size());
    std::int64_t bucket_recut = 0;
    if (plan.time_bucket_us > 0) {
        keep.push_back(0);  // src bucket key
        if (plan.time_bucket_us != best_bucket)
            bucket_recut = static_cast<std::int64_t>(plan.time_bucket_us);
    }
    for (const auto& qg : plan.group_by) {
        std::size_t at = best_gb.size();
        for (std::size_t i = 0; i < best_gb.size(); ++i)
            if (best_gb[i].kind == qg.kind &&
                dstd::StringView(best_gb[i].arg) == key_identity(qg)) {
                at = i;
                break;
            }
        if (at == best_gb.size()) return dstd::nullopt;  // not subsumed
        keep.push_back(static_cast<std::int32_t>(src_off + at));
    }

    auto coarse = dataframe::agg_regroup(*fine, keep, bucket_recut);
    return trace::views::detail::finalize_engine_result(*coarse, plan);
}

dstd::String cache_dir(const trace::views::detail::ViewPlan& plan) {
    // A Rank group key resolves pid -> rank from PR metadata harvested during
    // the scan; a rollup read-back never scans, so a rank plan cannot use a
    // rollup.
    for (const auto& gk : plan.group_by)
        if (gk.kind == trace::views::GroupKey::Kind::Rank) return {};
    // A lookup's rows are not part of the plan's identity.
    if (plan.build_step) return {};
    if (plan.files.empty()) return {};
    const dstd::StringView p = plan.files.front().index_path;
    if (p.empty()) return {};
    for (const auto& f : plan.files)
        if (f.index_path != p) return {};
    // The index root holds every durable artifact, so the caches go inside it.
    return store::internal::normalize_index_root(p);
}

dstd::String rollup_cache_path(const trace::views::detail::ViewPlan& plan) {
    for (const auto& gk : plan.group_by)
        if (gk.kind == trace::views::GroupKey::Kind::Rank) return {};
    // A caller-set root anchors a cross-file rollup (dask sets it for a
    // multi-file query, whose per-file index paths differ).
    if (plan.build_step) return {};
    if (!plan.rollup_root.empty())
        return dstd::String(dstd::StringView(plan.rollup_root));
    const dstd::String dir = cache_dir(plan);
    return dir.empty() ? dir : index::rollups_dir(dir);
}

}  // namespace dftracer::utils::index::cache
