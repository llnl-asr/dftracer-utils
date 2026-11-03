// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/internal/governor.h>
#include <dftracer/utils/dataframe/lazy/cursors.h>
#include <dftracer/utils/dataframe/lazy/hash_arena.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/hash/xxh3.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/queue.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <memory>

namespace dftracer::utils::dataframe {

using namespace lazy_internal;

namespace {

// Appends cell `i` to a head_by key. Numbers encode by value, so equal numbers
// of different numeric types (1, 1u, 1.0, and 0.0 / -0.0) share one key and
// every NaN is one key; any other cell uses append_cell's exact bytes.
void append_value_key(dstd::String& key, const Series& c, std::int64_t i) {
    const TypeId t = c.type();
    if (!is_numeric(t) || c.is_null(i)) {
        append_cell(key, c, i);
        return;
    }
    auto put = [&](char tag, auto v) {
        key.push_back(tag);
        key.append(reinterpret_cast<const char*>(&v), sizeof(v));
    };
    if (t == TypeId::Float32 || t == TypeId::Float64) {
        const double d = read_f64(c, i);
        constexpr double TWO_63 = 9223372036854775808.0;
        if (std::isnan(d)) {
            key.push_back('n');
        } else if (d != std::trunc(d)) {
            put('f', d);
        } else if (d >= -TWO_63 && d < TWO_63) {
            put('i', static_cast<std::int64_t>(d));
        } else if (d >= 0 && d < 2 * TWO_63) {
            put('u', static_cast<std::uint64_t>(d));
        } else {
            put('f', d);
        }
        return;
    }
    if (t == TypeId::Uint64) {
        const std::uint64_t u = read_u64(c, i);
        if (u > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
            put('u', u);
            return;
        }
        put('i', static_cast<std::int64_t>(u));
        return;
    }
    put('i', static_cast<std::int64_t>(read_u64(c, i)));
}

// Rows whose unique() key bytes share one buffer, and the unit of parallel
// work that builds them.
constexpr std::size_t KEY_CHUNK_ROWS = std::size_t{1} << 13;

// Fan-out width for the partitioned first-seen dedup below.
constexpr std::size_t DEDUP_PARTITIONS = 32;

// Approximate per-entry overhead of one string in an unordered_dense::set
// (node + bucket bookkeeping), added to the key's own byte length when sizing
// the distinct-key state for the unique() spill trigger below.
constexpr std::size_t DEDUP_ENTRY_OVERHEAD = 48;

// Grace-hash-distinct spill fan-out and recursion bound (unique() below): a
// partition that still exceeds budget after fanning out is re-partitioned
// with a depth-salted hash, capped so a single hot key (which always lands in
// the same partition, however deep) cannot recurse forever.
constexpr int UNIQUE_SPILL_FANOUT = 16;

constexpr int UNIQUE_SPILL_MAX_DEPTH = 3;

constexpr std::int64_t UNIQUE_SPILL_MIN_LEAF_ROWS = 64;

constexpr std::size_t SURVIVOR_CHUNK = 512;

// Survivor files read at once while the spool is replayed; more files are
// merged into longer ones first, so the open files do not grow with the
// number of hash leaves.
constexpr std::size_t MAX_OPEN_SURVIVORS = 64;

// The part of "keep the first `keep` rows of each key" that runs past the
// memory budget. The rows still to come are spooled in order; only their
// (row id, key) pairs are hash-partitioned to disk, so a partition holds ids in
// ascending order and the first `keep` it sees per key are the survivors, which
// come out already sorted. An oversized partition is split again with a
// salted hash. The spool is then replayed and only the surviving rows are
// emitted, so no row data is merged and every file is read in sequence.
class FirstRowsSpill {
   public:
    using Already = dstd::Function<std::int64_t(const dstd::String&)>;

    // `gov` is the owning cursor's. `held` is what that cursor keeps in memory
    // beside the spool.
    FirstRowsSpill(std::uint64_t budget, std::int64_t keep,
                   std::int64_t first_id, Already already, Governor& gov,
                   std::uint64_t held)
        : gov_(gov),
          held_(held),
          budget_(budget),
          keep_(keep),
          next_id_(first_id),
          replay_id_(first_id),
          already_(std::move(already)),
          spool_(budget / share::SPOOL),
          parts_(dir_, UNIQUE_SPILL_FANOUT) {}

    void add(Morsel&& m, dstd::Vector<dstd::String> keys) {
        const std::int64_t n = m.rows;
        dstd::Vector<dstd::Vector<std::int64_t>> ids(UNIQUE_SPILL_FANOUT);
        dstd::Vector<dstd::Vector<dstd::String>> ks(UNIQUE_SPILL_FANOUT);
        for (std::int64_t i = 0; i < n; ++i) {
            dstd::String& key = keys[static_cast<std::size_t>(i)];
            if (already_(key) >= keep_) continue;
            const std::size_t p =
                dstd::hash::partition_of(key, UNIQUE_SPILL_FANOUT);
            ids[p].push_back(next_id_ + i);
            ks[p].push_back(std::move(key));
        }
        for (std::size_t p = 0; p < UNIQUE_SPILL_FANOUT; ++p) {
            if (ids[p].empty()) continue;
            const auto count = static_cast<std::int64_t>(ids[p].size());
            dstd::Vector<Series> cols;
            cols.push_back(Series::flat_i64(ids[p].data(), count));
            cols.push_back(Series::strings(ks[p]));
            parts_.write(p, cols, count);
        }
        next_id_ += n;
        spool_.add(std::move(m.columns), n);
        if (has_cap(budget_) && gov_.check(held_ + spool_.memory_bytes(),
                                           spool_.memory_bytes()) != 0) {
            spool_.spill();
            gov_.spilled(held_ + spool_.memory_bytes());
        }
    }

    coro::CoroTask<void> finish() {
        parts_.close();
        for (std::size_t p = 0; p < parts_.size(); ++p)
            co_await settle(parts_.path(p), parts_.bytes(p), parts_.rows(p), 0);
        while (survivor_paths_.size() > MAX_OPEN_SURVIVORS) {
            dstd::Vector<dstd::String> merged;
            for (std::size_t at = 0; at < survivor_paths_.size();
                 at += MAX_OPEN_SURVIVORS) {
                const std::size_t end =
                    std::min(survivor_paths_.size(), at + MAX_OPEN_SURVIVORS);
                if (end - at == 1)
                    merged.push_back(survivor_paths_[at]);
                else
                    merged.push_back(co_await merge_survivors(
                        {survivor_paths_.begin() +
                             static_cast<std::ptrdiff_t>(at),
                         survivor_paths_.begin() +
                             static_cast<std::ptrdiff_t>(end)}));
            }
            survivor_paths_ = std::move(merged);
        }
        for (const dstd::String& path : survivor_paths_)
            runs_.emplace_back(dstd::make_unique<spill::Reader>(path));
        replay_ = spool_.reader();
    }

    coro::CoroTask<dstd::Optional<Morsel>> next(std::int64_t max_rows) {
        if (runs_.empty()) co_return dstd::nullopt;
        for (;;) {
            auto m = co_await replay_->next(max_rows);
            if (!m) break;
            const std::int64_t lo = replay_id_;
            const std::int64_t hi = lo + m->rows;
            replay_id_ = hi;
            dstd::Vector<std::int64_t> keep;
            for (Run& r : runs_) {
                while (!r.done) {
                    if (!r.cur || r.pos >= r.cur->rows) {
                        r.cur = co_await r.reader->next(SURVIVOR_CHUNK);
                        r.pos = 0;
                        if (!r.cur) {
                            r.done = true;
                            break;
                        }
                    }
                    const std::int64_t id =
                        r.cur->columns[0].data<std::int64_t>()[r.pos];
                    if (id >= hi) break;
                    keep.push_back(id - lo);
                    ++r.pos;
                }
            }
            if (keep.empty()) continue;
            std::sort(keep.begin(), keep.end());
            DataFrame mf = frame_of(std::move(m->columns));
            co_return morsel_of(take(mf, keep));
        }
        co_return dstd::nullopt;
    }

   private:
    struct Run {
        explicit Run(dstd::UniquePtr<spill::Reader> r) : reader(std::move(r)) {}
        dstd::UniquePtr<spill::Reader> reader;
        dstd::Optional<Morsel> cur;
        std::int64_t pos = 0;
        bool done = false;
    };

    // The sorted row ids of several survivor files as one sorted file.
    coro::CoroTask<dstd::String> merge_survivors(
        dstd::Vector<dstd::String> paths) {
        dstd::Vector<Run> heads;
        heads.reserve(paths.size());
        for (const dstd::String& path : paths)
            heads.emplace_back(dstd::make_unique<spill::Reader>(path));
        using Entry = dstd::Pair<std::int64_t, std::size_t>;
        dstd::PriorityQueue<Entry, std::greater<Entry>> heap;
        const auto load = [&](std::size_t k) -> coro::CoroTask<void> {
            Run& r = heads[k];
            r.cur = co_await r.reader->next(SURVIVOR_CHUNK);
            r.pos = 0;
            if (r.cur)
                heap.emplace(r.cur->columns[0].data<std::int64_t>()[0], k);
        };
        for (std::size_t k = 0; k < heads.size(); ++k) co_await load(k);
        const dstd::String out_path = dir_.run_path(dir_.next_run());
        spill::Writer out(out_path);
        dstd::Vector<std::int64_t> ids;
        ids.reserve(SURVIVOR_CHUNK);
        const auto flush = [&] {
            if (ids.empty()) return;
            const auto n = static_cast<std::int64_t>(ids.size());
            dstd::Vector<Series> cols;
            cols.push_back(Series::flat_i64(ids.data(), n));
            out.write(cols, n);
            ids.clear();
        };
        while (!heap.empty()) {
            const std::size_t k = heap.top().second;
            heap.pop();
            Run& r = heads[k];
            ids.push_back(r.cur->columns[0].data<std::int64_t>()[r.pos]);
            if (ids.size() == SURVIVOR_CHUNK) flush();
            if (++r.pos < r.cur->rows)
                heap.emplace(r.cur->columns[0].data<std::int64_t>()[r.pos], k);
            else
                co_await load(k);
        }
        flush();
        out.close();
        heads.clear();
        for (const dstd::String& path : paths) dir_.remove_run(path);
        co_return out_path;
    }

    // A partition that fits a fraction of the budget is counted in memory; a
    // larger one is split again (until the depth limit or too few rows).
    coro::CoroTask<void> settle(const dstd::String& path, std::uint64_t bytes,
                                std::int64_t rows, int depth) {
        if (depth < UNIQUE_SPILL_MAX_DEPTH && bytes > budget_ / share::SMALL &&
            rows > UNIQUE_SPILL_MIN_LEAF_ROWS) {
            spill::HashPartitions sub(dir_, UNIQUE_SPILL_FANOUT);
            spill::Reader reader(path);
            for (;;) {
                auto m = co_await reader.next(DEFAULT_MORSEL_ROWS);
                if (!m) break;
                dstd::Vector<dstd::Vector<std::int64_t>> pick(
                    UNIQUE_SPILL_FANOUT);
                for (std::int64_t i = 0; i < m->rows; ++i)
                    pick[dstd::hash::partition_of(m->columns[1].string_at(i),
                                                  UNIQUE_SPILL_FANOUT,
                                                  depth + 1)]
                        .push_back(i);
                DataFrame mf = frame_of(std::move(m->columns));
                for (std::size_t p = 0; p < UNIQUE_SPILL_FANOUT; ++p) {
                    if (pick[p].empty()) continue;
                    DataFrame part = take(mf, pick[p]);
                    sub.write(p, part.columns, part.num_rows());
                }
            }
            sub.close();
            for (std::size_t p = 0; p < sub.size(); ++p)
                co_await settle(sub.path(p), sub.bytes(p), sub.rows(p),
                                depth + 1);
            co_return;
        }
        dstd::StringViewMap<std::int64_t> emitted;
        const dstd::String out_path = dir_.run_path(dir_.next_run());
        spill::Writer out(out_path);
        dstd::Vector<std::int64_t> ids;
        ids.reserve(SURVIVOR_CHUNK);
        bool any = false;
        auto flush = [&] {
            if (ids.empty()) return;
            const auto n = static_cast<std::int64_t>(ids.size());
            dstd::Vector<Series> cols;
            cols.push_back(Series::flat_i64(ids.data(), n));
            out.write(cols, n);
            ids.clear();
            any = true;
        };
        spill::Reader reader(path);
        for (;;) {
            auto m = co_await reader.next(DEFAULT_MORSEL_ROWS);
            if (!m) break;
            const std::int64_t* row_ids = m->columns[0].data<std::int64_t>();
            for (std::int64_t i = 0; i < m->rows; ++i) {
                const dstd::StringView key = m->columns[1].string_at(i);
                auto it = emitted.find(key);
                if (it == emitted.end())
                    it = emitted
                             .emplace(dstd::String(key),
                                      already_(dstd::String(key)))
                             .first;
                if (it->second >= keep_) continue;
                ++it->second;
                ids.push_back(row_ids[i]);
                if (ids.size() == SURVIVOR_CHUNK) flush();
            }
        }
        flush();
        out.close();
        if (any) survivor_paths_.push_back(out_path);
    }

    Governor& gov_;
    std::uint64_t held_;
    std::uint64_t budget_;
    std::int64_t keep_;
    std::int64_t next_id_;
    std::int64_t replay_id_;
    Already already_;
    spill::Spool spool_;
    spill::Dir dir_;
    spill::HashPartitions parts_;
    dstd::Vector<dstd::String> survivor_paths_;
    dstd::Vector<Run> runs_;
    dstd::UniquePtr<Cursor> replay_;
};

// A key set cannot leave memory, so a request from another holder frees
// nothing and only a refused grow sends the rest of the input external.
bool refused(Governor& gov, std::uint64_t budget, std::uint64_t keys) {
    return has_cap(budget) && gov.check(keys, 0) != 0;
}

// Keeps the first n rows of each distinct key tuple, in input order. Streams:
// holds one counter per distinct key. A null key is its own key.
class HeadByCursor : public Cursor {
   public:
    HeadByCursor(dstd::UniquePtr<Cursor> in, dstd::Vector<std::int64_t> key_idx,
                 std::int64_t n, std::uint64_t budget)
        : in_(std::move(in)),
          key_idx_(std::move(key_idx)),
          n_(n),
          budget_(budget) {}

    std::uint64_t resident_bytes() const override { return gov_.reserved(); }

    coro::CoroTask<dstd::Optional<Morsel>> next(
        std::int64_t max_rows) override {
        if (n_ <= 0) co_return dstd::nullopt;
        if (!spilling_) {
            for (;;) {
                auto m = co_await in_->next(max_rows);
                if (!m) break;
                const std::int64_t rows = m->rows;
                dstd::Vector<dstd::String> keys = keys_of(*m);
                dstd::Vector<std::int64_t> keep;
                keep.reserve(static_cast<std::size_t>(rows));
                for (std::int64_t i = 0; i < rows; ++i) {
                    dstd::String& key = keys[static_cast<std::size_t>(i)];
                    const std::size_t key_bytes =
                        key.size() + DEDUP_ENTRY_OVERHEAD;
                    auto [it, fresh] = counts_.try_emplace(std::move(key), 0);
                    if (fresh) fast_bytes_ += key_bytes;
                    if (it->second < n_) {
                        ++it->second;
                        keep.push_back(i);
                    }
                }
                next_row_id_ += rows;
                if (budget_ > 0 &&
                    (fast_bytes_ > budget_ / share::SPILL_STATE ||
                     refused(gov_, budget_, fast_bytes_)))
                    spilling_ = true;
                if (keep.empty()) {
                    if (spilling_) break;
                    continue;
                }
                if (static_cast<std::int64_t>(keep.size()) == rows) co_return m;
                DataFrame mf = frame_of(std::move(m->columns));
                Morsel out = morsel_of(take(mf, keep));
                out.ordering = m->ordering;
                out.ordered_column = m->ordered_column;
                out.ordered_descending = m->ordered_descending;
                co_return out;
            }
            if (!spilling_) co_return dstd::nullopt;
        }
        if (!external_) {
            external_ = dstd::make_unique<FirstRowsSpill>(
                budget_, n_, next_row_id_,
                [this](const dstd::String& key) {
                    const auto it = counts_.find(key);
                    return it == counts_.end() ? std::int64_t{0} : it->second;
                },
                gov_, fast_bytes_);
            for (;;) {
                auto m = co_await in_->next(max_rows);
                if (!m) break;
                dstd::Vector<dstd::String> keys = keys_of(*m);
                external_->add(std::move(*m), std::move(keys));
            }
            co_await external_->finish();
        }
        co_return co_await external_->next(max_rows);
    }

   private:
    dstd::Vector<dstd::String> keys_of(const Morsel& m) const {
        dstd::Vector<dstd::String> keys(static_cast<std::size_t>(m.rows));
        parallel_for(
            m.rows, std::int64_t{1} << 13, [&](std::int64_t b, std::int64_t e) {
                for (std::int64_t i = b; i < e; ++i) {
                    dstd::String& k = keys[static_cast<std::size_t>(i)];
                    for (std::int64_t c : key_idx_)
                        append_value_key(
                            k, m.columns[static_cast<std::size_t>(c)], i);
                }
            });
        return keys;
    }

    dstd::UniquePtr<Cursor> in_;
    dstd::Vector<std::int64_t> key_idx_;
    std::int64_t n_;
    std::uint64_t budget_;
    dstd::HashMap<dstd::String, std::int64_t> counts_;
    std::size_t fast_bytes_ = 0;
    std::int64_t next_row_id_ = 0;
    bool spilling_ = false;
    Governor gov_{"head_by"};
    dstd::UniquePtr<FirstRowsSpill> external_;
};

// Streaming distinct (keep first occurrence, original order). The fast path
// holds only the set of distinct row keys - which is the result itself,
// materialized by collect anyway - and streams input and output morsel by
// morsel. When that set outgrows half of `budget_`, the rest of the input
// goes through FirstRowsSpill with one row kept per key.
class UniqueCursor : public Cursor {
   public:
    UniqueCursor(dstd::UniquePtr<Cursor> in, dstd::Vector<dstd::String> sch,
                 dstd::Vector<std::int64_t> key_idx, std::uint64_t budget)
        : in_(std::move(in)),
          sch_(std::move(sch)),
          key_idx_(std::move(key_idx)),
          budget_(budget),
          seen_(arena_pool()) {
        if (parallel_backend_installed()) {
            seen_p_.reserve(DEDUP_PARTITIONS);
            for (std::size_t g = 0; g < DEDUP_PARTITIONS; ++g)
                seen_p_.push_back(dstd::make_unique<HashArena>(arena_pool()));
        }
    }

    std::uint64_t resident_bytes() const override { return gov_.reserved(); }

    coro::CoroTask<dstd::Optional<Morsel>> next(
        std::int64_t max_rows) override {
        if (!spilling_) {
            for (;;) {
                auto m = co_await in_->next(max_rows);
                if (!m) break;
                const std::int64_t n = m->rows;
                // Build each row's key bytes once, in parallel: one buffer per
                // chunk of rows (not one allocation per row) plus the row's
                // offset, length and hash. The dedup below only compares them.
                const dstd::Vector<Series> kc = key_cols(m->columns);
                const auto nz = static_cast<std::size_t>(n);
                const std::size_t nchunks =
                    (nz + KEY_CHUNK_ROWS - 1) / KEY_CHUNK_ROWS;
                dstd::Vector<dstd::String> chunk_bytes(nchunks);
                dstd::Vector<std::uint64_t> hashes(nz);
                dstd::Vector<std::uint64_t> key_off(nz);
                dstd::Vector<std::uint32_t> key_len(nz);
                parallel_for(
                    static_cast<std::int64_t>(nchunks), 1,
                    [&](std::int64_t cb, std::int64_t ce) {
                        for (std::int64_t c = cb; c < ce; ++c) {
                            dstd::String& buf =
                                chunk_bytes[static_cast<std::size_t>(c)];
                            const std::size_t lo =
                                static_cast<std::size_t>(c) * KEY_CHUNK_ROWS;
                            const std::size_t hi =
                                std::min(nz, lo + KEY_CHUNK_ROWS);
                            for (std::size_t at = lo; at < hi; ++at) {
                                const std::size_t start = buf.size();
                                for (const Series& col : kc)
                                    append_cell(buf, col,
                                                static_cast<std::int64_t>(at));
                                key_off[at] = start;
                                key_len[at] = static_cast<std::uint32_t>(
                                    buf.size() - start);
                                hashes[at] = dstd::hash::xxh3_64(
                                    buf.data() + start, buf.size() - start);
                            }
                        }
                    });
                auto key_ptr = [&](std::size_t at) {
                    return chunk_bytes[at / KEY_CHUNK_ROWS].data() +
                           key_off[at];
                };
                dstd::Vector<std::uint8_t> keep_mask(nz, 0);
                // The arenas reserve before they grow. The bound is what they
                // would hold if every key of the morsel were new.
                bool refuse = false;
                if (seen_p_.empty()) {
                    if (has_cap(budget_)) {
                        std::size_t key_total = 0;
                        std::size_t key_max = 0;
                        for (std::size_t at = 0; at < nz; ++at) {
                            key_total += key_len[at];
                            key_max =
                                std::max<std::size_t>(key_max, key_len[at]);
                        }
                        refuse =
                            refused(gov_, budget_,
                                    seen_.bytes_after(nz, key_total, key_max));
                    }
                    for (std::size_t at = 0; at < nz; ++at)
                        if (seen_.insert(hashes[at], key_ptr(at), key_len[at]))
                            keep_mask[at] = 1;
                } else {
                    const std::size_t p = seen_p_.size();
                    dstd::Vector<dstd::Vector<std::size_t>> buckets(p);
                    for (std::size_t at = 0; at < nz; ++at)
                        buckets[hashes[at] % p].push_back(at);
                    if (has_cap(budget_)) {
                        std::uint64_t bound = 0;
                        for (std::size_t g = 0; g < p; ++g) {
                            std::size_t key_total = 0;
                            std::size_t key_max = 0;
                            for (std::size_t at : buckets[g]) {
                                key_total += key_len[at];
                                key_max =
                                    std::max<std::size_t>(key_max, key_len[at]);
                            }
                            bound += seen_p_[g]->bytes_after(
                                buckets[g].size(), key_total, key_max);
                        }
                        refuse = refused(gov_, budget_, bound);
                    }
                    parallel_for(
                        static_cast<std::int64_t>(p), 1,
                        [&](std::int64_t pb, std::int64_t pe) {
                            for (std::int64_t g = pb; g < pe; ++g)
                                for (std::size_t at :
                                     buckets[static_cast<std::size_t>(g)])
                                    if (seen_p_[static_cast<std::size_t>(g)]
                                            ->insert(hashes[at], key_ptr(at),
                                                     key_len[at]))
                                        keep_mask[at] = 1;
                        });
                }
                dstd::Vector<std::int64_t> keep;
                keep.reserve(static_cast<std::size_t>(n));
                for (std::int64_t i = 0; i < n; ++i) {
                    if (!keep_mask[static_cast<std::size_t>(i)]) continue;
                    keep.push_back(i);
                    fast_bytes_ += key_len[static_cast<std::size_t>(i)] +
                                   DEDUP_ENTRY_OVERHEAD;
                }
                next_row_id_ += n;
                if (budget_ > 0 &&
                    (fast_bytes_ > budget_ / share::SPILL_STATE || refuse))
                    spilling_ = true;
                if (!keep.empty()) {
                    DataFrame mf = frame_of(std::move(m->columns), sch_);
                    co_return morsel_of(take(mf, keep));
                }
                if (spilling_) break;
            }
            if (!spilling_) co_return dstd::nullopt;
        }
        if (!external_) {
            external_ = dstd::make_unique<FirstRowsSpill>(
                budget_, 1, next_row_id_,
                [this](const dstd::String& key) {
                    return already_seen(key) ? std::int64_t{1}
                                             : std::int64_t{0};
                },
                gov_, arena_bytes());
            for (;;) {
                auto m = co_await in_->next(max_rows);
                if (!m) break;
                dstd::Vector<dstd::String> keys(
                    static_cast<std::size_t>(m->rows));
                const dstd::Vector<Series> kc = key_cols(m->columns);
                parallel_for(m->rows, std::int64_t{1} << 13,
                             [&](std::int64_t b, std::int64_t e) {
                                 for (std::int64_t i = b; i < e; ++i)
                                     keys[static_cast<std::size_t>(i)] =
                                         row_key(kc, i);
                             });
                external_->add(std::move(*m), std::move(keys));
            }
            co_await external_->finish();
        }
        co_return co_await external_->next(max_rows);
    }

   private:
    // The columns the row key hashes: every data column, or the subset the
    // op names (positions against the plan schema).
    dstd::Vector<Series> key_cols(const dstd::Vector<Series>& data) const {
        dstd::Vector<Series> out;
        if (key_idx_.empty()) {
            out.reserve(data.size());
            for (const Series& c : data) out.push_back(c.share());
        } else {
            out.reserve(key_idx_.size());
            for (std::int64_t k : key_idx_)
                out.push_back(data[static_cast<std::size_t>(k)].share());
        }
        return out;
    }

    bool already_seen(const dstd::String& key) const {
        const std::uint64_t h = dstd::hash::xxh3_64(key.data(), key.size());
        const auto len = static_cast<std::uint32_t>(key.size());
        if (!seen_p_.empty())
            return seen_p_[h % seen_p_.size()]->contains(h, key.data(), len);
        return seen_.contains(h, key.data(), len);
    }

    // The arenas never leave the cursor, so they live in its Scratch pool.
    memory::Pool& arena_pool() const {
        return gov_.pool() ? *gov_.pool() : memory::current_pool();
    }

    std::uint64_t arena_bytes() const {
        std::uint64_t total = seen_.bytes();
        for (const dstd::UniquePtr<HashArena>& a : seen_p_) total += a->bytes();
        return total;
    }

    dstd::UniquePtr<Cursor> in_;
    dstd::Vector<dstd::String> sch_;
    dstd::Vector<std::int64_t> key_idx_;
    std::uint64_t budget_;
    // Declared before the arenas, so its pool closes after they free.
    Governor gov_{"unique"};
    HashArena seen_;
    dstd::Vector<dstd::UniquePtr<HashArena>> seen_p_;
    std::size_t fast_bytes_ = 0;
    std::int64_t next_row_id_ = 0;
    bool spilling_ = false;
    dstd::UniquePtr<FirstRowsSpill> external_;
};

}  // namespace

namespace lazy_internal {

dstd::UniquePtr<Cursor> make_head_by(dstd::UniquePtr<Cursor> in,
                                     dstd::Vector<std::int64_t> key_idx,
                                     std::int64_t n, std::uint64_t budget) {
    return dstd::make_unique<HeadByCursor>(std::move(in), std::move(key_idx), n,
                                           budget);
}

dstd::UniquePtr<Cursor> make_unique_rows(dstd::UniquePtr<Cursor> in,
                                         dstd::Vector<dstd::String> sch,
                                         dstd::Vector<std::int64_t> key_idx,
                                         std::uint64_t budget) {
    return dstd::make_unique<UniqueCursor>(std::move(in), std::move(sch),
                                           std::move(key_idx), budget);
}

}  // namespace lazy_internal

}  // namespace dftracer::utils::dataframe
