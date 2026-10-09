#include <dftracer/utils/dataframe/lazy/cursors.h>

namespace dftracer::utils::dataframe {

using namespace lazy_internal;

namespace {

// Radix-partition `keys` by hash into `seen_p.size()` buckets, each with its
// own running hash set, and mark keep[i] the first time each key is seen
// (order-preserving): buckets never overlap, so each is probed by exactly one
// worker with no lock. `seen_p` carries state across calls, bounded by the
// distinct-key count rather than the row count. An empty `seen_p` means no
// parallel backend is installed; the single `seen_serial` set is used instead
// so the partition bookkeeping is never paid for nothing.
std::vector<std::uint8_t> first_seen_mask(
    std::vector<std::string>& keys, std::int64_t n,
    ankerl::unordered_dense::set<std::string>& seen_serial,
    std::vector<ankerl::unordered_dense::set<std::string>>& seen_p) {
    std::vector<std::uint8_t> keep_mask(static_cast<std::size_t>(n), 0);
    if (seen_p.empty()) {
        for (std::int64_t i = 0; i < n; ++i)
            if (seen_serial.insert(std::move(keys[static_cast<std::size_t>(i)]))
                    .second)
                keep_mask[static_cast<std::size_t>(i)] = 1;
        return keep_mask;
    }
    const std::size_t p = seen_p.size();
    std::vector<std::vector<std::int64_t>> buckets(p);
    for (std::int64_t i = 0; i < n; ++i)
        buckets[std::hash<std::string>{}(keys[static_cast<std::size_t>(i)]) % p]
            .push_back(i);
    parallel_for(
        static_cast<std::int64_t>(p), 1, [&](std::int64_t pb, std::int64_t pe) {
            for (std::int64_t g = pb; g < pe; ++g)
                for (std::int64_t i : buckets[static_cast<std::size_t>(g)])
                    if (seen_p[static_cast<std::size_t>(g)]
                            .insert(
                                std::move(keys[static_cast<std::size_t>(i)]))
                            .second)
                        keep_mask[static_cast<std::size_t>(i)] = 1;
        });
    return keep_mask;
}

// Appends cell `i` to a head_by key. Numbers encode by value, so equal numbers
// of different numeric types (1, 1u, 1.0, and 0.0 / -0.0) share one key and
// every NaN is one key; any other cell uses append_cell's exact bytes.
void append_value_key(std::string& key, const Series& c, std::int64_t i) {
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
    using Already = std::function<std::int64_t(const std::string&)>;

    FirstRowsSpill(std::uint64_t budget, std::int64_t keep,
                   std::int64_t first_id, Already already)
        : budget_(budget),
          keep_(keep),
          next_id_(first_id),
          replay_id_(first_id),
          already_(std::move(already)),
          spool_(budget / share::SPOOL),
          parts_(dir_, UNIQUE_SPILL_FANOUT) {}

    void add(Morsel&& m, std::vector<std::string> keys) {
        const std::int64_t n = m.rows;
        std::vector<std::vector<std::int64_t>> ids(UNIQUE_SPILL_FANOUT);
        std::vector<std::vector<std::string>> ks(UNIQUE_SPILL_FANOUT);
        for (std::int64_t i = 0; i < n; ++i) {
            std::string& key = keys[static_cast<std::size_t>(i)];
            if (already_(key) >= keep_) continue;
            const std::size_t p = hash::partition_of(key, UNIQUE_SPILL_FANOUT);
            ids[p].push_back(next_id_ + i);
            ks[p].push_back(std::move(key));
        }
        for (std::size_t p = 0; p < UNIQUE_SPILL_FANOUT; ++p) {
            if (ids[p].empty()) continue;
            const auto count = static_cast<std::int64_t>(ids[p].size());
            std::vector<Series> cols;
            cols.push_back(Series::flat_i64(ids[p].data(), count));
            cols.push_back(Series::strings(ks[p]));
            parts_.write(p, cols, count);
        }
        next_id_ += n;
        spool_.add(std::move(m.columns), n);
    }

    coro::CoroTask<void> finish() {
        parts_.close();
        for (std::size_t p = 0; p < parts_.size(); ++p)
            co_await settle(parts_.path(p), parts_.bytes(p), parts_.rows(p), 0);
        while (survivor_paths_.size() > MAX_OPEN_SURVIVORS) {
            std::vector<std::string> merged;
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
        for (const std::string& path : survivor_paths_)
            runs_.emplace_back(std::make_unique<spill::Reader>(path));
        replay_ = spool_.reader();
    }

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) {
        if (runs_.empty()) co_return std::nullopt;
        while (auto m = co_await replay_->next(max_rows)) {
            const std::int64_t lo = replay_id_;
            const std::int64_t hi = lo + m->rows;
            replay_id_ = hi;
            std::vector<std::int64_t> keep;
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
        co_return std::nullopt;
    }

   private:
    struct Run {
        explicit Run(std::unique_ptr<spill::Reader> r) : reader(std::move(r)) {}
        std::unique_ptr<spill::Reader> reader;
        std::optional<Morsel> cur;
        std::int64_t pos = 0;
        bool done = false;
    };

    // The sorted row ids of several survivor files as one sorted file.
    coro::CoroTask<std::string> merge_survivors(
        std::vector<std::string> paths) {
        std::vector<Run> heads;
        heads.reserve(paths.size());
        for (const std::string& path : paths)
            heads.emplace_back(std::make_unique<spill::Reader>(path));
        using Entry = std::pair<std::int64_t, std::size_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>>
            heap;
        const auto load = [&](std::size_t k) -> coro::CoroTask<void> {
            Run& r = heads[k];
            r.cur = co_await r.reader->next(SURVIVOR_CHUNK);
            r.pos = 0;
            if (r.cur)
                heap.emplace(r.cur->columns[0].data<std::int64_t>()[0], k);
        };
        for (std::size_t k = 0; k < heads.size(); ++k) co_await load(k);
        const std::string out_path = dir_.run_path(dir_.next_run());
        spill::Writer out(out_path);
        std::vector<std::int64_t> ids;
        ids.reserve(SURVIVOR_CHUNK);
        const auto flush = [&] {
            if (ids.empty()) return;
            const auto n = static_cast<std::int64_t>(ids.size());
            std::vector<Series> cols;
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
        std::error_code ec;
        for (const std::string& path : paths) fs::remove(path, ec);
        co_return out_path;
    }

    // A partition that fits a fraction of the budget is counted in memory; a
    // larger one is split again (until the depth limit or too few rows).
    coro::CoroTask<void> settle(const std::string& path, std::uint64_t bytes,
                                std::int64_t rows, int depth) {
        if (depth < UNIQUE_SPILL_MAX_DEPTH && bytes > budget_ / share::SMALL &&
            rows > UNIQUE_SPILL_MIN_LEAF_ROWS) {
            spill::HashPartitions sub(dir_, UNIQUE_SPILL_FANOUT);
            spill::Reader reader(path);
            while (auto m = co_await reader.next(DEFAULT_MORSEL_ROWS)) {
                std::vector<std::vector<std::int64_t>> pick(
                    UNIQUE_SPILL_FANOUT);
                for (std::int64_t i = 0; i < m->rows; ++i)
                    pick[hash::partition_of(m->columns[1].string_at(i),
                                            UNIQUE_SPILL_FANOUT, depth + 1)]
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
        StringViewMap<std::int64_t> emitted;
        const std::string out_path = dir_.run_path(dir_.next_run());
        spill::Writer out(out_path);
        std::vector<std::int64_t> ids;
        ids.reserve(SURVIVOR_CHUNK);
        bool any = false;
        auto flush = [&] {
            if (ids.empty()) return;
            const auto n = static_cast<std::int64_t>(ids.size());
            std::vector<Series> cols;
            cols.push_back(Series::flat_i64(ids.data(), n));
            out.write(cols, n);
            ids.clear();
            any = true;
        };
        spill::Reader reader(path);
        while (auto m = co_await reader.next(DEFAULT_MORSEL_ROWS)) {
            const std::int64_t* row_ids = m->columns[0].data<std::int64_t>();
            for (std::int64_t i = 0; i < m->rows; ++i) {
                const std::string_view key = m->columns[1].string_at(i);
                auto it = emitted.find(key);
                if (it == emitted.end())
                    it = emitted
                             .emplace(std::string(key),
                                      already_(std::string(key)))
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

    std::uint64_t budget_;
    std::int64_t keep_;
    std::int64_t next_id_;
    std::int64_t replay_id_;
    Already already_;
    spill::Spool spool_;
    spill::Dir dir_;
    spill::HashPartitions parts_;
    std::vector<std::string> survivor_paths_;
    std::vector<Run> runs_;
    std::unique_ptr<Cursor> replay_;
};

// Keeps the first n rows of each distinct key tuple, in input order. Streams:
// holds one counter per distinct key. A null key is its own key.
class HeadByCursor : public Cursor {
   public:
    HeadByCursor(std::unique_ptr<Cursor> in, std::vector<std::int64_t> key_idx,
                 std::int64_t n, std::uint64_t budget)
        : in_(std::move(in)),
          key_idx_(std::move(key_idx)),
          n_(n),
          budget_(budget) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (n_ <= 0) co_return std::nullopt;
        if (!spilling_) {
            while (auto m = co_await in_->next(max_rows)) {
                const std::int64_t rows = m->rows;
                std::vector<std::string> keys = keys_of(*m);
                std::vector<std::int64_t> keep;
                keep.reserve(static_cast<std::size_t>(rows));
                for (std::int64_t i = 0; i < rows; ++i) {
                    std::string& key = keys[static_cast<std::size_t>(i)];
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
                if (budget_ > 0 && fast_bytes_ > budget_ / share::SPILL_STATE)
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
            if (!spilling_) co_return std::nullopt;
        }
        if (!external_) {
            external_ = std::make_unique<FirstRowsSpill>(
                budget_, n_, next_row_id_, [this](const std::string& key) {
                    const auto it = counts_.find(key);
                    return it == counts_.end() ? std::int64_t{0} : it->second;
                });
            while (auto m = co_await in_->next(max_rows)) {
                std::vector<std::string> keys = keys_of(*m);
                external_->add(std::move(*m), std::move(keys));
            }
            co_await external_->finish();
        }
        co_return co_await external_->next(max_rows);
    }

   private:
    std::vector<std::string> keys_of(const Morsel& m) const {
        std::vector<std::string> keys(static_cast<std::size_t>(m.rows));
        parallel_for(
            m.rows, std::int64_t{1} << 13, [&](std::int64_t b, std::int64_t e) {
                for (std::int64_t i = b; i < e; ++i) {
                    std::string& k = keys[static_cast<std::size_t>(i)];
                    for (std::int64_t c : key_idx_)
                        append_value_key(
                            k, m.columns[static_cast<std::size_t>(c)], i);
                }
            });
        return keys;
    }

    std::unique_ptr<Cursor> in_;
    std::vector<std::int64_t> key_idx_;
    std::int64_t n_;
    std::uint64_t budget_;
    ankerl::unordered_dense::map<std::string, std::int64_t> counts_;
    std::size_t fast_bytes_ = 0;
    std::int64_t next_row_id_ = 0;
    bool spilling_ = false;
    std::unique_ptr<FirstRowsSpill> external_;
};

// Streaming distinct (keep first occurrence, original order). The fast path
// holds only the set of distinct row keys - which is the result itself,
// materialized by collect anyway - and streams input and output morsel by
// morsel. When that set outgrows half of `budget_`, the rest of the input
// goes through FirstRowsSpill with one row kept per key.
class UniqueCursor : public Cursor {
   public:
    UniqueCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
                 std::vector<std::int64_t> key_idx, std::uint64_t budget)
        : in_(std::move(in)),
          sch_(std::move(sch)),
          key_idx_(std::move(key_idx)),
          budget_(budget) {
        if (parallel_backend_installed()) seen_p_.resize(DEDUP_PARTITIONS);
    }

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (!spilling_) {
            while (auto m = co_await in_->next(max_rows)) {
                const std::int64_t n = m->rows;
                // Build the exact row keys in parallel (scalar string work,
                // one per row, independent), then dedupe (radix-partitioned
                // when a parallel backend is installed, else one serial set).
                std::vector<std::string> keys(static_cast<std::size_t>(n));
                const std::vector<Series> kc = key_cols(m->columns);
                parallel_for(n, std::int64_t{1} << 13,
                             [&](std::int64_t b, std::int64_t e) {
                                 for (std::int64_t i = b; i < e; ++i)
                                     keys[static_cast<std::size_t>(i)] =
                                         row_key(kc, i);
                             });
                std::vector<std::uint32_t> key_len(static_cast<std::size_t>(n));
                for (std::int64_t i = 0; i < n; ++i)
                    key_len[static_cast<std::size_t>(i)] =
                        static_cast<std::uint32_t>(
                            keys[static_cast<std::size_t>(i)].size());
                std::vector<std::uint8_t> keep_mask =
                    first_seen_mask(keys, n, seen_, seen_p_);
                std::vector<std::int64_t> keep;
                keep.reserve(static_cast<std::size_t>(n));
                for (std::int64_t i = 0; i < n; ++i) {
                    if (!keep_mask[static_cast<std::size_t>(i)]) continue;
                    keep.push_back(i);
                    fast_bytes_ += key_len[static_cast<std::size_t>(i)] +
                                   DEDUP_ENTRY_OVERHEAD;
                }
                next_row_id_ += n;
                if (budget_ > 0 && fast_bytes_ > budget_ / share::SPILL_STATE)
                    spilling_ = true;
                if (!keep.empty()) {
                    DataFrame mf = frame_of(std::move(m->columns), sch_);
                    co_return morsel_of(take(mf, keep));
                }
                if (spilling_) break;
            }
            if (!spilling_) co_return std::nullopt;
        }
        if (!external_) {
            external_ = std::make_unique<FirstRowsSpill>(
                budget_, 1, next_row_id_, [this](const std::string& key) {
                    return already_seen(key) ? std::int64_t{1}
                                             : std::int64_t{0};
                });
            while (auto m = co_await in_->next(max_rows)) {
                std::vector<std::string> keys(
                    static_cast<std::size_t>(m->rows));
                const std::vector<Series> kc = key_cols(m->columns);
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
    std::vector<Series> key_cols(const std::vector<Series>& data) const {
        std::vector<Series> out;
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

    bool already_seen(const std::string& key) const {
        if (!seen_p_.empty())
            return seen_p_[std::hash<std::string>{}(key) % seen_p_.size()]
                .contains(key);
        return seen_.contains(key);
    }

    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    std::vector<std::int64_t> key_idx_;
    std::uint64_t budget_;
    ankerl::unordered_dense::set<std::string> seen_;
    std::vector<ankerl::unordered_dense::set<std::string>> seen_p_;
    std::size_t fast_bytes_ = 0;
    std::int64_t next_row_id_ = 0;
    bool spilling_ = false;
    std::unique_ptr<FirstRowsSpill> external_;
};

}  // namespace

namespace lazy_internal {

std::unique_ptr<Cursor> make_head_by(std::unique_ptr<Cursor> in,
                                     std::vector<std::int64_t> key_idx,
                                     std::int64_t n, std::uint64_t budget) {
    return std::make_unique<HeadByCursor>(std::move(in), std::move(key_idx), n,
                                          budget);
}

std::unique_ptr<Cursor> make_unique_rows(std::unique_ptr<Cursor> in,
                                         std::vector<std::string> sch,
                                         std::vector<std::int64_t> key_idx,
                                         std::uint64_t budget) {
    return std::make_unique<UniqueCursor>(std::move(in), std::move(sch),
                                          std::move(key_idx), budget);
}

}  // namespace lazy_internal

}  // namespace dftracer::utils::dataframe
