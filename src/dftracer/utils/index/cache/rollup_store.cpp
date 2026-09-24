#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/hash/fnv1a.h>
#include <dftracer/utils/core/common/str_format.h>
#include <dftracer/utils/core/env.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/internal/expr_handle.h>  // expr_canonical
#include <dftracer/utils/index/cache/rollup_store.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/column_families.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/utilities/common/serialization/binary_codec.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace dftracer::utils::index::cache {

namespace rdb = index::store;
namespace codec = utilities::common::serialization;
namespace dataframe = dftracer::utils::dataframe;

namespace {

void put_be64(std::string& out, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<char>((v >> shift) & 0xFF));
}

std::uint64_t get_be64(std::string_view b) {
    std::uint64_t v = 0;
    for (unsigned char c : b) v = (v << 8) | c;
    return v;
}

// Every plan field except group_by - the basis shared by plan_signature and
// rest_signature.
// Part of every rollup key: a rollup persisted under another identity is never
// matched (it is recomputed instead).
constexpr std::string_view ROLLUP_FORMAT = "1";

constexpr std::uint64_t DEFAULT_CACHE_MAX_BYTES = std::uint64_t{2} << 30;
constexpr std::size_t USAGE_BYTES = 16;

std::string usage_key(std::uint64_t sig) {
    std::string key;
    key.push_back('\x02');
    put_be64(key, sig);
    return key;
}

std::string usage_value(std::uint64_t bytes, std::uint64_t last_used) {
    std::string v;
    put_be64(v, bytes);
    put_be64(v, last_used);
    return v;
}

std::uint64_t now_micros() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void check(const ::rocksdb::Status& st, const char* what) {
    if (!st.ok())
        throw DFTUtilsException(ErrorCode::IO,
                                std::string(what) + ": " + st.ToString());
}

// Removes the least recently used rollups until the store holds at most
// cache_max_bytes(); `keep` goes only when it alone is over the budget.
void evict(rdb::RocksDatabase& db, std::uint64_t keep) {
    struct Entry {
        std::uint64_t sig, bytes, last_used;
    };
    std::vector<Entry> entries;
    std::uint64_t total = 0;
    const char tag = '\x02';
    auto it = db.new_iterator(rdb::cf::ROLLUP);
    for (it->Seek(std::string_view(&tag, 1)); it->Valid(); it->Next()) {
        const std::string_view k(it->key().data(), it->key().size());
        if (k.empty() || k[0] != tag) break;
        const std::string_view v(it->value().data(), it->value().size());
        if (k.size() != 9 || v.size() != USAGE_BYTES) continue;
        entries.push_back({get_be64(k.substr(1)), get_be64(v.substr(0, 8)),
                           get_be64(v.substr(8))});
        total += entries.back().bytes;
    }
    check(it->status(), "rollup usage scan");
    const std::uint64_t budget = cache_max_bytes();
    if (total <= budget) return;
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) {
                  return a.last_used < b.last_used;
              });
    auto batch = db.begin_batch();
    auto drop = [&](const Entry& e) {
        const std::string rows = rollup_row_key(e.sig, "");
        std::string rows_end = rollup_row_key(e.sig + 1, "");
        if (e.sig == UINT64_MAX) rows_end = std::string(1, '\x02');
        db.delete_range(batch, rdb::cf::ROLLUP, rows, rows_end);
        db.del(batch, rdb::cf::ROLLUP, rollup_desc_key(e.sig));
        db.del(batch, rdb::cf::ROLLUP, usage_key(e.sig));
        total -= e.bytes;
    };
    for (const auto& e : entries) {
        if (total <= budget) break;
        if (e.sig != keep) drop(e);
    }
    if (total > budget)
        for (const auto& e : entries)
            if (e.sig == keep) drop(e);
    check(db.commit_batch(batch), "rollup eviction");
}

// Everything that decides a group key's values: a transform coarsens them, so
// dirname(file_path) and file_path are different keys.
std::string key_identity(const trace::views::GroupKey& gk) {
    std::string id = gk.arg;
    if (gk.transform == trace::views::GroupKey::Transform::None) return id;
    id.push_back('\x1f');
    id += std::to_string(static_cast<int>(gk.transform));
    for (const std::string& a : gk.transform_args) {
        id.push_back('\x1f');
        id += a;
    }
    return id;
}

void add_rest_fields(std::string& sig,
                     const trace::views::detail::ViewPlan& plan) {
    auto add = [&](std::string_view s) {
        sig.append(s);
        sig.push_back('\0');
    };
    add(ROLLUP_FORMAT);
    // Dictionaries of the record schema resolve `resolved.*` fields, so a
    // rollup made under one schema must not serve another.
    if (!plan.record_schema)
        throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                "rollup signature: the plan's record schema "
                                "is not resolved");
    add(std::to_string(plan.record_schema->params_hash()));
    std::vector<const trace::views::ViewFile*> files;
    files.reserve(plan.files.size());
    for (const auto& f : plan.files) files.push_back(&f);
    std::sort(files.begin(), files.end(),
              [](auto* a, auto* b) { return a->file_path < b->file_path; });
    for (auto* f : files) {
        add(f->file_path);
        std::error_code ec;
        add(std::to_string(fs::file_size(f->file_path, ec)));
        auto wt = fs::last_write_time(f->file_path, ec);
        add(std::to_string(
            static_cast<long long>(ec ? 0 : wt.time_since_epoch().count())));
    }
    add(plan.query ? plan.query->source() : "");
    for (const trace::views::detail::ComputedColumn& c : plan.computed) {
        add(c.name);
        add(dataframe::expr_canonical(c.expr));
        for (const std::string& in : c.inputs) add(in);
    }
    add(std::to_string(static_cast<int>(plan.phase)));
    // time_bucket_us is intentionally NOT part of the rest basis: a coarser
    // query can be served by re-bucketing a finer rollup, so buckets must not
    // gate subsumption. It lands in plan_signature (row-key identity) only.
    add(std::to_string(plan.time_scale));
    if (plan.time_range) {
        add(std::to_string(plan.time_range->first));
        add(std::to_string(plan.time_range->second));
    }
    for (const auto& a : plan.agg) {
        add(std::to_string(static_cast<int>(a.op)));
        add(a.field);
        add(a.out_name);
        add(a.by);
    }
    // Bucket alignment shifts every bucket boundary, so it partitions the
    // result grid: a min/origin-aligned query must not reuse an absolute
    // rollup. Re-bucketing a coarser query from a finer rollup assumes
    // 0-aligned flooring, so for an aligned plan pin the exact width too (no
    // re-cut).
    add(std::to_string(plan.bucket_origin_us));
    add(plan.bucket_origin_min ? "1" : "0");
    if (plan.bucket_origin_us || plan.bucket_origin_min)
        add("bw=" + std::to_string(plan.time_bucket_us));
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
    add("accumfmt1");
    for (const auto& s : plan.select) add(s);
}

}  // namespace

std::string rollup_row_key(std::uint64_t sig, std::string_view group_key) {
    std::string key;
    key.reserve(9 + group_key.size());
    key.push_back('\x01');
    put_be64(key, sig);
    key.append(group_key);
    return key;
}

std::string rollup_desc_key(std::uint64_t sig) {
    std::string key;
    key.reserve(9);
    key.push_back('\x00');
    put_be64(key, sig);
    return key;
}

std::shared_ptr<rdb::RocksDatabase> open_rollup_db(
    const std::string& path, rdb::RocksDatabase::OpenMode mode) {
    if (mode == rdb::RocksDatabase::OpenMode::ReadWrite)
        fs::create_directories(path);
    return rdb::RocksDBManager::instance().get_or_open(path, mode);
}

std::uint64_t cache_max_bytes() {
    const auto text =
        dftracer::utils::Env::get<std::string_view>("DFTRACER_CACHE_MAX_BYTES");
    if (!text) return DEFAULT_CACHE_MAX_BYTES;
    return dftracer::utils::parse_bytes(*text).value_or(
        DEFAULT_CACHE_MAX_BYTES);
}

void persist_rollup(rdb::RocksDatabase& db, std::uint64_t sig,
                    std::uint64_t rest_sig, std::uint64_t time_bucket_us,
                    const std::vector<trace::views::GroupKey>& group_by,
                    const dataframe::AggState& state) {
    const std::int64_t ng = dataframe::agg_num_groups(state);
    auto batch = db.begin_batch();
    std::uint64_t bytes = 0;
    std::string row_key;
    for (std::int64_t g = 0; g < ng; ++g) {
        // Row key = the group's composite key (unit-separated), a stable
        // identity so a re-persist of the same group overwrites in place.
        row_key.clear();
        for (const auto& part : dataframe::agg_group_key(state, g)) {
            row_key += part;
            row_key += trace::views::detail::GROUP_SEP;
        }
        const std::string key = rollup_row_key(sig, row_key);
        const std::string blob =
            dataframe::agg_serialize(*dataframe::agg_extract_group(state, g));
        bytes += key.size() + blob.size();
        db.put(batch, rdb::cf::ROLLUP, key, blob);
    }
    // Shape descriptor the planner matches against: rest hash + this rollup's
    // time bucket (for coarsening) + this view's grouping (kind + key identity
    // per key, in order).
    std::string desc;
    codec::put_be64(desc, rest_sig);
    codec::put_be64(desc, time_bucket_us);
    codec::put_be32(desc, static_cast<std::uint32_t>(group_by.size()));
    for (const auto& gk : group_by) {
        codec::put_be32(desc, static_cast<std::uint32_t>(gk.kind));
        codec::put_str(desc, key_identity(gk));
    }
    bytes += 9 + desc.size() + 9 + USAGE_BYTES;
    db.put(batch, rdb::cf::ROLLUP, rollup_desc_key(sig), desc);
    db.put(batch, rdb::cf::ROLLUP, usage_key(sig),
           usage_value(bytes, now_micros()));
    check(db.commit_batch(batch), "rollup persist");
    evict(db, sig);
}

bool rollup_exists(const rdb::RocksDatabase& db, std::uint64_t sig) {
    std::string val;
    return db.get(rollup_desc_key(sig), &val, rdb::cf::ROLLUP).ok();
}

dataframe::AggStatePtr read_rollup(const rdb::RocksDatabase& db,
                                   std::uint64_t sig) {
    const std::string prefix = rollup_row_key(sig, "");  // 0x01 | sig
    auto it = db.new_iterator(rdb::cf::ROLLUP);
    dataframe::AggStatePtr merged;
    for (it->Seek(prefix); it->Valid(); it->Next()) {
        const std::string_view k(it->key().data(), it->key().size());
        if (k.size() < prefix.size() || k.substr(0, prefix.size()) != prefix)
            break;
        std::string blob(it->value().data(), it->value().size());
        auto one = dataframe::agg_deserialize(blob);
        if (!merged)
            merged = std::move(one);
        else
            dataframe::agg_merge(*merged, *one);
    }
    return merged;
}

std::uint64_t rest_signature(const trace::views::detail::ViewPlan& plan) {
    std::string sig;
    add_rest_fields(sig, plan);
    return dftracer::utils::hash::fnv1a_hash(sig);
}

std::uint64_t plan_signature(const trace::views::detail::ViewPlan& plan) {
    std::string sig;
    add_rest_fields(sig, plan);
    sig.append(std::to_string(plan.time_bucket_us));
    sig.push_back('\0');
    for (const auto& g : plan.group_by) {
        sig.append(std::to_string(static_cast<int>(g.kind)));
        sig.push_back('\0');
        sig.append(key_identity(g));
        sig.push_back('\0');
    }
    return dftracer::utils::hash::fnv1a_hash(sig);
}

std::optional<dataframe::DataFrame> find_subsuming_rollup(
    rdb::RocksDatabase& db, const trace::views::detail::ViewPlan& plan) {
    const std::uint64_t q_rest = rest_signature(plan);
    const char desc_tag = '\x00';

    // Pick the tightest subsuming rollup: fewest group dims means fewest rows
    // to re-aggregate, and an exact match (same dims as the query) is optimal.
    std::uint64_t best_sig = 0;
    std::uint64_t best_bucket = 0;
    std::vector<trace::views::GroupKey> best_gb;
    bool have_best = false;

    auto it = db.new_iterator(rdb::cf::ROLLUP);
    for (it->Seek(std::string_view(&desc_tag, 1)); it->Valid(); it->Next()) {
        const std::string_view k(it->key().data(), it->key().size());
        if (k.empty() || k[0] != '\x00') break;
        if (k.size() != 9) continue;

        const std::string_view v(it->value().data(), it->value().size());
        if (v.size() < 20) continue;
        codec::BinaryReader br(v);
        if (br.be64() != q_rest) continue;
        const std::uint64_t r_bucket = br.be64();

        // Bucket compatibility: a query with a time bucket can only be served
        // by a rollup whose bucket evenly divides it (coarsen finer ->
        // coarser). A query without a bucket collapses any rollup's buckets, so
        // it always matches. The reverse (finer query, coarser rollup) is
        // unserviceable.
        if (plan.time_bucket_us > 0 &&
            (r_bucket == 0 || plan.time_bucket_us % r_bucket != 0))
            continue;

        std::vector<trace::views::GroupKey> r_gb;
        const std::uint32_t n = br.be32();
        r_gb.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            trace::views::GroupKey gk;
            gk.kind = static_cast<trace::views::GroupKey::Kind>(br.be32());
            gk.arg = std::string(br.str());
            r_gb.push_back(std::move(gk));
        }
        if (have_best && r_gb.size() >= best_gb.size()) continue;

        bool subset = true;
        for (const auto& qg : plan.group_by) {
            bool found = false;
            for (const auto& rg : r_gb)
                if (rg.kind == qg.kind && rg.arg == key_identity(qg)) {
                    found = true;
                    break;
                }
            if (!found) {
                subset = false;
                break;
            }
        }
        if (!subset) continue;

        best_sig = get_be64(k.substr(1, 8));
        best_bucket = r_bucket;
        best_gb = std::move(r_gb);
        have_best = true;
        if (best_gb.size() == plan.group_by.size() &&
            best_bucket == plan.time_bucket_us)
            break;  // exact: optimal
    }

    if (!have_best) return std::nullopt;
    auto fine = read_rollup(db, best_sig);
    if (!fine) return std::nullopt;
    if (!db.is_read_only()) {
        std::string usage;
        if (db.get(usage_key(best_sig), &usage, rdb::cf::ROLLUP).ok() &&
            usage.size() == USAGE_BYTES)
            check(db.put(usage_key(best_sig),
                         usage_value(
                             get_be64(std::string_view(usage).substr(0, 8)),
                             now_micros()),
                         rdb::cf::ROLLUP),
                  "rollup usage");
    }

    // Map the query's grouping onto the stored state's key columns. The state
    // key layout is [time_bucket?, best_gb...]; keep the query's keys in query
    // order (dropping the bucket when the query has none, or re-flooring it to
    // the query's coarser grain otherwise).
    const std::size_t src_off = best_bucket > 0 ? 1 : 0;
    std::vector<std::int32_t> keep;
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
                best_gb[i].arg == key_identity(qg)) {
                at = i;
                break;
            }
        if (at == best_gb.size()) return std::nullopt;  // not subsumed
        keep.push_back(static_cast<std::int32_t>(src_off + at));
    }

    auto coarse = dataframe::agg_regroup(*fine, keep, bucket_recut);
    return trace::views::detail::finalize_engine_result(*coarse, plan);
}

std::string cache_dir(const trace::views::detail::ViewPlan& plan) {
    // A Rank group key resolves pid -> rank from PR metadata harvested during
    // the scan; a rollup read-back never scans, so a rank plan cannot use a
    // rollup.
    for (const auto& gk : plan.group_by)
        if (gk.kind == trace::views::GroupKey::Kind::Rank) return {};
    if (plan.files.empty()) return {};
    const std::string& p = plan.files.front().index_path;
    if (p.empty()) return {};
    for (const auto& f : plan.files)
        if (f.index_path != p) return {};
    // Beside the index, not in it: code treats an existing index folder as an
    // existing index, and a first build removes a folder with no database.
    return (fs::path(p).parent_path() / ".dftindex-cache").string();
}

std::string rollup_cache_path(const trace::views::detail::ViewPlan& plan) {
    for (const auto& gk : plan.group_by)
        if (gk.kind == trace::views::GroupKey::Kind::Rank) return {};
    // A caller-set root anchors a cross-file rollup (dask sets it for a
    // multi-file query, whose per-file index paths differ).
    if (!plan.rollup_root.empty()) return plan.rollup_root;
    const std::string dir = cache_dir(plan);
    return dir.empty() ? dir : (fs::path(dir) / "rollups").string();
}

}  // namespace dftracer::utils::index::cache
