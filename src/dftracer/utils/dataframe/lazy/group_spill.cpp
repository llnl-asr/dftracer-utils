// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/internal/governor.h>
#include <dftracer/utils/dataframe/lazy/cursors.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdio>

namespace dftracer::utils::dataframe {

using namespace lazy_internal;

namespace {

constexpr int GROUP_SPILL_MAX_DEPTH = 3;

constexpr std::size_t GROUP_SPILL_PARTS = 64;

constexpr std::int64_t FIRST_SLICE_ROWS = 1024;

// The rows of `in` in slices of at most `cap` rows, counting the rows taken,
// so a group-by whose state grows fast per row (many dyn names) is checked
// against its budget before one morsel has outgrown it.
class SlicedRows : public Cursor {
   public:
    SlicedRows(Cursor& in, const std::int64_t& cap, std::int64_t& taken)
        : in_(in), cap_(cap), taken_(taken) {}

    coro::CoroTask<dstd::Optional<Morsel>> next(
        std::int64_t max_rows) override {
        const auto fill_ok = co_await feed_.fill(in_, max_rows);
        if (!fill_ok) co_return dstd::nullopt;
        Morsel m = feed_.take(std::min(max_rows, cap_));
        taken_ += m.rows;
        co_return m;
    }

   private:
    Cursor& in_;
    const std::int64_t& cap_;
    std::int64_t& taken_;
    MorselFeed feed_;
};

// One run: single-group AggState blobs (agg_extract_group + agg_serialize),
// length-prefixed, in ascending composite-key order (agg_sort_groups). The
// on-disk unit a bounded k-way merge reads back one group at a time.
// Streaming group-by: fold every morsel into one mergeable AggState. While the
// state stays under half the budget the result is that one state, finalized
// directly. Past it, the state is cut by a hash of the group key into
// GROUP_SPILL_PARTS part files and a fresh state starts, so every flush of a
// key lands in the same part; each part is then merged in memory on its own
// (split again by a salted hash when its merged state still exceeds the
// budget) and its groups are emitted, at most max_rows per call.
class GroupByCursor : public Cursor {
   public:
    GroupByCursor(dstd::UniquePtr<Cursor> in, dstd::Vector<dstd::String> sch,
                  dstd::Vector<dstd::String> keys, dstd::Vector<GroupAgg> aggs,
                  std::uint64_t budget, dstd::Vector<AggDynSpec> dyn = {},
                  dstd::String dyn_prefix = {})
        : in_(std::move(in)),
          sch_(std::move(sch)),
          keys_(std::move(keys)),
          aggs_(std::move(aggs)),
          budget_(budget),
          dyn_specs_(std::move(dyn)),
          dyn_prefix_(std::move(dyn_prefix)) {}

    dstd::Optional<dstd::Vector<dstd::String>> out_names() const override {
        return out_names_;
    }

    std::uint64_t resident_bytes() const override { return gov_.reserved(); }

    coro::CoroTask<dstd::Optional<Morsel>> next(
        std::int64_t max_rows) override {
        if (!built_) co_await build(max_rows);
        if (!spilled_) {
            if (!done_) {
                done_ = true;
                chunks_.set(std::move(result_));
            }
            if (chunks_.empty()) co_return dstd::nullopt;
            co_return chunks_.next(max_rows, output_share(budget_));
        }
        co_return merge_next(max_rows);
    }

   private:
    static int index_in(const dstd::Vector<dstd::String>& s,
                        const dstd::String& n) {
        return require_col(s, n, "group_by");
    }

    static Morsel to_morsel(const DataFrame& r) {
        Morsel out;
        out.rows = r.num_rows();
        out.columns.reserve(r.columns.size());
        for (const Series& c : r.columns)
            out.columns.push_back(c.materialize());
        return out;
    }

    coro::CoroTask<void> build(std::int64_t max_rows) {
        dstd::Vector<int> key_idx;
        key_idx.reserve(keys_.size());
        for (const dstd::String& k : keys_)
            key_idx.push_back(index_in(sch_, k));
        dstd::Vector<int>
            value_idx;  // sch indices of the deduped value columns
        dstd::HashMap<dstd::String, std::int32_t> dedup;
        auto resolve = [&](const dstd::String& name) -> std::int32_t {
            auto it = dedup.find(name);
            if (it != dedup.end()) return it->second;
            const std::int32_t idx =
                static_cast<std::int32_t>(value_idx.size());
            value_idx.push_back(index_in(sch_, name));
            dedup.emplace(name, idx);
            return idx;
        };
        specs_.reserve(aggs_.size());
        for (const GroupAgg& a : aggs_) {
            AggSpec sp;
            sp.op = to_agg_op(a.op);
            sp.out = a.out;
            sp.param = a.param;
            sp.value_col = sp.op == AggOp::Count ? -1 : resolve(a.column);
            if (agg_uses_by_col(sp.op)) sp.by_col = resolve(a.by);
            specs_.push_back(std::move(sp));
        }

        // Dyn columns are the prefix-tagged plan columns, and the
        // prefix-tagged fields of the rest column.
        dstd::Vector<dstd::Pair<int, dstd::String>> sch_dyn;
        if (!dyn_specs_.empty() && !dyn_prefix_.empty())
            for (std::size_t i = 0; i < sch_.size(); ++i)
                if (sch_[i].rfind(dyn_prefix_, 0) == 0)
                    sch_dyn.emplace_back(static_cast<int>(i),
                                         sch_[i].substr(dyn_prefix_.size()));
        const bool rest_dyn =
            !dyn_specs_.empty() && !dyn_prefix_.empty() && has_rest(sch_);

        AggStatePtr state = agg_new(specs_, dyn_specs_);
        auto accumulate = [&](AggState& st, const Morsel& m,
                              std::int64_t row_base) {
            agg_set_row_base(st, row_base);
            dstd::Vector<const Series*> keys;
            keys.reserve(key_idx.size());
            for (int ki : key_idx) keys.push_back(&m.columns[ki]);
            dstd::Vector<const Series*> values;
            values.reserve(value_idx.size());
            for (int vi : value_idx) values.push_back(&m.columns[vi]);
            if (dyn_specs_.empty()) {
                agg_accumulate(st, keys, values);
                return;
            }
            dstd::Vector<AggDynInput> dyn;
            const DataFrame rest =
                rest_dyn ? rest_fields(m.columns.back()) : DataFrame{};
            dyn.reserve(sch_dyn.size() + rest.columns.size());
            for (const auto& [ci, name] : sch_dyn)
                dyn.push_back({name, &m.columns[static_cast<std::size_t>(ci)]});
            for (std::size_t i = 0; i < rest.columns.size(); ++i)
                if (rest.names[i].rfind(dyn_prefix_, 0) == 0)
                    dyn.push_back({rest.names[i].substr(dyn_prefix_.size()),
                                   &rest.columns[i]});
            agg_accumulate(st, keys, values, dyn);
        };
        // Rows per slice: what keeps a slice's state within an eighth of the
        // spill threshold, from the state bytes per row seen so far; few rows
        // until the first batch has shown it.
        std::int64_t cap = has_cap(budget_) ? std::min<std::int64_t>(
                                                  max_rows, FIRST_SLICE_ROWS)
                                            : max_rows;
        std::int64_t taken = 0;
        SlicedRows sliced(*in_, cap, taken);
        const std::uint64_t threshold = budget_ / share::SPILL_STATE;
        std::uint64_t state_bytes = 0;
        std::uint64_t batch_bytes = 0;
        co_await accumulate_batches(
            sliced, max_rows, budget_, state,
            [&] { return agg_new(specs_, dyn_specs_); }, accumulate,
            [&](dstd::Vector<Morsel>&, AggStatePtr& st) {
                if (budget_ == 0) return;
                const std::uint64_t bytes = agg_approx_bytes(*st);
                if (taken > 0 && has_cap(budget_)) {
                    const std::uint64_t per_row = std::max<std::uint64_t>(
                        bytes / static_cast<std::uint64_t>(taken), 1);
                    cap = rows_within(threshold / 8, per_row, max_rows);
                }
                state_bytes = bytes;
                if (bytes > threshold || asked_ ||
                    (has_cap(budget_) && gov_.check(bytes) != 0)) {
                    spill_state(*st);
                    st = agg_new(specs_, dyn_specs_);
                    taken = 0;
                    asked_ = false;
                    state_bytes = 0;
                    gov_.spilled(0);
                }
            },
            [] { return true; },
            [&](const dstd::Vector<Morsel>& batch) {
                if (!has_cap(budget_)) return false;
                if (batch.size() == 1) batch_bytes = 0;
                batch_bytes += spill::columns_bytes(batch.back().columns);
                asked_ = gov_.check(state_bytes + batch_bytes) != 0;
                return asked_;
            });

        if (!spilled_) {
            DataFrame r = agg_finalize(*state, keys_);
            out_names_ = r.names;
            result_ = to_morsel(r);
        } else {
            if (agg_group_count(*state) > 0) spill_state(*state);
            for (std::size_t p = 0; p < part_out_.size(); ++p) {
                std::FILE* f = part_out_[p].release();
                if (!f || std::fclose(f) != 0)
                    throw common::DFTUtilsException::cat(
                        common::ErrorCode::Io,
                        "group_by spill: cannot write a part file");
                if (part_bytes_[p])
                    todo_.push_back({part_paths_[p], part_bytes_[p], 0});
            }
        }
        built_ = true;
    }

    // A spilled state is cut into hash parts, each appended to its part file as
    // one blob, so equal keys from every flush end up in the same part and a
    // part can later be merged in memory on its own.
    void spill_state(const AggState& state) {
        if (!spilled_) {
            spilled_ = true;
            for (std::size_t p = 0; p < GROUP_SPILL_PARTS; ++p) {
                part_paths_.push_back(dir_.run_path(dir_.next_run()));
                part_out_.emplace_back(
                    std::fopen(part_paths_.back().c_str(), "wb"));
                part_bytes_.push_back(0);
            }
        }
        agg_dyn_layout_add(layout_, state);
        write_split(state, part_out_, part_bytes_, 0);
    }

    void write_split(const AggState& state, dstd::Vector<dstd::ScopedFile>& out,
                     dstd::Vector<std::uint64_t>& bytes, int salt) const {
        const auto split = agg_split_groups(state, out.size(), salt);
        for (std::size_t p = 0; p < split.size(); ++p) {
            if (split[p].empty()) continue;
            const dstd::String blob =
                agg_serialize(*agg_extract_groups(state, split[p]), true);
            if (blob.size() > std::numeric_limits<std::uint32_t>::max())
                throw common::DFTUtilsException::cat(
                    common::ErrorCode::Io,
                    "group_by spill: a part holds over 4 GiB of group states");
            const auto len = static_cast<std::uint32_t>(blob.size());
            std::FILE* f = out[p].get();
            if (!f || std::fwrite(&len, sizeof len, 1, f) != 1 ||
                std::fwrite(blob.data(), 1, blob.size(), f) != blob.size())
                throw common::DFTUtilsException::cat(
                    common::ErrorCode::Io,
                    "group_by spill: cannot write a part file");
            bytes[p] += sizeof len + blob.size();
            dir_.charge().add(sizeof len + blob.size());
        }
    }

    // Reads blobs in batches of about `limit` bytes, deserializes each batch
    // in parallel and calls `fn` on it until it returns false.
    template <class F>
    static void read_batches(const dstd::String& path, std::uint64_t limit,
                             F&& fn) {
        dstd::ScopedFile is(std::fopen(path.c_str(), "rb"));
        if (!is)
            throw common::DFTUtilsException::cat(
                common::ErrorCode::Io, "group_by spill: cannot open part file ",
                path);
        const auto sized = dstd::fs::file_size(dstd::PathView(path));
        if (!sized)
            throw common::DFTUtilsException::cat(
                common::ErrorCode::Io, "group_by spill: cannot stat part file ",
                path);
        const std::uint64_t file_size = *sized;
        std::uint64_t consumed = 0;
        dstd::Vector<dstd::String> raw;
        std::uint64_t have = 0;
        auto flush = [&] {
            if (raw.empty()) return true;
            dstd::Vector<AggStatePtr> states(raw.size());
            parallel_for(
                static_cast<std::int64_t>(raw.size()), 1,
                [&](std::int64_t b, std::int64_t e) {
                    for (std::int64_t j = b; j < e; ++j)
                        states[static_cast<std::size_t>(j)] = agg_deserialize(
                            raw[static_cast<std::size_t>(j)], true);
                });
            raw.clear();
            have = 0;
            return fn(states);
        };
        std::uint32_t len = 0;
        while (std::fread(&len, 1, sizeof len, is.get()) == sizeof len) {
            consumed += sizeof len;
            if (len > file_size - consumed)
                throw common::DFTUtilsException::cat(
                    common::ErrorCode::Io, "group_by spill: corrupt part file");
            consumed += len;
            dstd::String blob(len, '\0');
            if (std::fread(blob.data(), 1, len, is.get()) != len)
                throw common::DFTUtilsException::cat(
                    common::ErrorCode::Io,
                    "group_by spill: truncated part file");
            have += len;
            raw.push_back(std::move(blob));
            if (have >= limit && !flush()) return;
        }
        flush();
    }

    AggStatePtr fold(AggStatePtr acc, dstd::Vector<AggStatePtr>& batch) const {
        for (const AggStatePtr& st : batch) agg_merge(*acc, *st);
        return acc;
    }

    std::uint64_t batch_bytes() const {
        return std::max<std::uint64_t>(budget_ / share::SMALL, 1 << 16);
    }

    struct Part {
        dstd::String path;
        std::uint64_t bytes;
        int depth;
    };

    // A part whose merged state does not fit the budget is split again by a
    // salted hash, so the same keys spread over new parts. Blobs are folded
    // into one state first, so the pieces written are as large as a flush.
    void split_part(const Part& part) {
        dstd::Vector<dstd::String> paths;
        dstd::Vector<dstd::ScopedFile> out;
        dstd::Vector<std::uint64_t> bytes(GROUP_SPILL_PARTS, 0);
        for (std::size_t p = 0; p < GROUP_SPILL_PARTS; ++p) {
            paths.push_back(dir_.run_path(dir_.next_run()));
            out.emplace_back(std::fopen(paths.back().c_str(), "wb"));
        }
        AggStatePtr acc = agg_new(specs_, dyn_specs_);
        read_batches(part.path, batch_bytes(),
                     [&](dstd::Vector<AggStatePtr>& batch) {
                         acc = fold(std::move(acc), batch);
                         const std::uint64_t held = agg_approx_bytes(*acc);
                         if (held > budget_ / share::SPILL_STATE ||
                             (has_cap(budget_) && gov_.check(held) != 0)) {
                             write_split(*acc, out, bytes, part.depth + 1);
                             acc = agg_new(specs_, dyn_specs_);
                             gov_.spilled(0);
                         }
                         return true;
                     });
        if (agg_group_count(*acc) > 0)
            write_split(*acc, out, bytes, part.depth + 1);
        for (std::size_t p = 0; p < out.size(); ++p) {
            std::FILE* f = out[p].release();
            if (!f || std::fclose(f) != 0)
                throw common::DFTUtilsException::cat(
                    common::ErrorCode::Io,
                    "group_by spill: cannot write a part file");
            if (bytes[p]) todo_.push_back({paths[p], bytes[p], part.depth + 1});
        }
        dir_.remove_run(part.path);
    }

    // Merges one part of the spill at a time and emits its groups. With dyn
    // columns every part finalizes under the union of the names the flushed
    // states held, noted as they were written.
    dstd::Optional<Morsel> merge_next(std::int64_t max_rows) {
        while (true) {
            if (!chunks_.empty())
                return chunks_.next(max_rows, output_share(budget_));
            if (todo_.empty()) return dstd::nullopt;
            const Part part = std::move(todo_.back());
            todo_.pop_back();
            AggStatePtr merged = agg_new(specs_, dyn_specs_);
            bool too_big = false;
            read_batches(
                part.path, batch_bytes(),
                [&](dstd::Vector<AggStatePtr>& batch) {
                    merged = fold(std::move(merged), batch);
                    const std::uint64_t held = agg_approx_bytes(*merged);
                    too_big = part.depth < GROUP_SPILL_MAX_DEPTH &&
                              (held > budget_ ||
                               (has_cap(budget_) && gov_.check(held) != 0));
                    return !too_big;
                });
            if (too_big) {
                merged.reset();
                gov_.spilled(0);
                split_part(part);
                continue;
            }
            if (agg_group_count(*merged) == 0) continue;
            DataFrame r = dyn_specs_.empty()
                              ? agg_finalize(*merged, keys_)
                              : agg_finalize(*merged, keys_, layout_);
            out_names_ = r.names;
            chunks_.set(to_morsel(r));
        }
    }

    dstd::UniquePtr<Cursor> in_;
    dstd::Vector<dstd::String> sch_;
    dstd::Vector<dstd::String> keys_;
    dstd::Vector<GroupAgg> aggs_;
    std::uint64_t budget_;
    dstd::Vector<AggDynSpec> dyn_specs_;
    dstd::String dyn_prefix_;
    dstd::Vector<AggSpec> specs_;
    dstd::Optional<dstd::Vector<dstd::String>> out_names_;
    bool built_ = false;
    bool done_ = false;
    bool spilled_ = false;
    AggDynLayout layout_;
    Morsel result_;
    spill::Dir dir_;
    dstd::Vector<dstd::String> part_paths_;
    dstd::Vector<dstd::ScopedFile> part_out_;
    dstd::Vector<std::uint64_t> part_bytes_;
    dstd::Vector<Part> todo_;
    ResultChunks chunks_;
    Governor gov_{"group_by"};
    // The governor asked for bytes while a batch was read, so spill after it.
    bool asked_ = false;
};

// Tumbling/sliding time windows over an ascending Int64 time column, grid
// anchored on the first event (the minimum, as the time ascends), matching
// DataFrame::group_by_dynamic. A window [s, s + period) is final once a time of
// s + period has been seen, so it leaves then: the state holds only the open
// windows, about period / every of them. A time below the one before refuses
// the input, since a window it belongs to may have left already.
class GroupByDynamicCursor : public Cursor {
   public:
    GroupByDynamicCursor(dstd::UniquePtr<Cursor> in, dstd::String time_col,
                         int time_idx, dstd::Vector<int> value_idx,
                         dstd::Vector<AggSpec> specs, std::int64_t every,
                         std::int64_t period, std::int64_t origin,
                         bool origin_min, std::uint64_t budget)
        : in_(std::move(in)),
          time_col_(std::move(time_col)),
          time_idx_(time_idx),
          value_idx_(std::move(value_idx)),
          specs_(std::move(specs)),
          every_(every),
          period_(period),
          origin_(origin),
          origin_min_(origin_min),
          budget_(budget),
          state_(agg_new(specs_)) {}

    coro::CoroTask<dstd::Optional<Morsel>> next(
        std::int64_t max_rows) override {
        while (chunks_.empty()) {
            if (eof_) co_return dstd::nullopt;
            auto m = co_await in_->next(max_rows);
            if (!m) {
                eof_ = true;
                // The open windows, or a typed result with no rows.
                chunks_.set(morsel_of(agg_finalize(*state_, time_col_)
                                          .sort_by(time_col_, false)));
                break;
            }
            accumulate(*m);
            close_windows();
        }
        co_return chunks_.next(max_rows, output_share(budget_));
    }

   private:
    void accumulate(const Morsel& m) {
        const Series& tc = m.columns[static_cast<std::size_t>(time_idx_)];
        if (tc.type() != TypeId::Int64)
            throw std::invalid_argument(
                ("group_by_dynamic: " + time_col_ + " must be an Int64 column")
                    .c_str());
        const std::int64_t* t = tc.data<std::int64_t>();
        dstd::Vector<std::int64_t> keyv, rowsv;
        for (std::int64_t i = 0; i < m.rows; ++i) {
            if (tc.is_null(i)) continue;
            const std::int64_t ts = t[i];
            if (seen_ && ts < last_)
                throw std::invalid_argument(
                    ("group_by_dynamic: " + time_col_ + " must ascend, " +
                     dstd::to_string(ts) + " follows " + dstd::to_string(last_))
                        .c_str());
            if (!seen_) {
                if (origin_min_) origin_ = ts;
                start0_ = origin_ + floor_to_multiple(ts - origin_, every_);
                seen_ = true;
            }
            last_ = ts;
            for (std::int64_t k = (ts - start0_) / every_; k >= 0; --k) {
                const std::int64_t s = start0_ + k * every_;
                if (s <= ts - period_) break;
                keyv.push_back(s);
                rowsv.push_back(i);
            }
        }
        if (keyv.empty()) return;
        const Series keyc = Series::flat_i64(
            keyv.data(), static_cast<std::int64_t>(keyv.size()));
        dstd::Vector<Series> gathered;
        gathered.reserve(value_idx_.size());
        for (int vi : value_idx_)
            gathered.push_back(
                m.columns[static_cast<std::size_t>(vi)].take(rowsv));
        dstd::Vector<const Series*> values;
        values.reserve(gathered.size());
        for (const Series& g : gathered) values.push_back(&g);
        agg_set_row_base(*state_, rows_seen_);
        rows_seen_ += static_cast<std::int64_t>(rowsv.size());
        agg_accumulate(*state_, keyc, values);
    }

    // Hands on the windows no later row can reach and keeps the others.
    void close_windows() {
        if (!seen_ || agg_group_count(*state_) == 0) return;
        DataFrame all = agg_finalize(*state_, time_col_);
        const std::int64_t* start = all.columns.front().data<std::int64_t>();
        dstd::Vector<std::int64_t> closed, open;
        for (std::int64_t g = 0; g < all.num_rows(); ++g)
            (start[g] <= last_ - period_ ? closed : open).push_back(g);
        if (closed.empty()) return;
        AggStatePtr kept = agg_new(specs_);
        if (!open.empty()) agg_merge(*kept, *state_, open);
        state_ = std::move(kept);
        DataFrame done;
        done.names = all.names;
        for (const Series& c : all.columns)
            done.columns.push_back(c.take(closed));
        chunks_.set(morsel_of(done.sort_by(time_col_, false)));
    }

    dstd::UniquePtr<Cursor> in_;
    dstd::String time_col_;
    int time_idx_;
    dstd::Vector<int> value_idx_;
    dstd::Vector<AggSpec> specs_;
    std::int64_t every_, period_;
    std::int64_t origin_;
    bool origin_min_;
    std::uint64_t budget_;
    AggStatePtr state_;
    ResultChunks chunks_;
    bool seen_ = false;
    bool eof_ = false;
    std::int64_t start0_ = 0;
    std::int64_t last_ = 0;
    std::int64_t rows_seen_ = 0;
};

}  // namespace

namespace lazy_internal {

dstd::UniquePtr<Cursor> make_group_by(dstd::UniquePtr<Cursor> in,
                                      dstd::Vector<dstd::String> sch,
                                      dstd::Vector<dstd::String> keys,
                                      dstd::Vector<GroupAgg> aggs,
                                      std::uint64_t budget,
                                      dstd::Vector<AggDynSpec> dyn,
                                      dstd::String dyn_prefix) {
    return dstd::make_unique<GroupByCursor>(
        std::move(in), std::move(sch), std::move(keys), std::move(aggs), budget,
        std::move(dyn), std::move(dyn_prefix));
}

dstd::UniquePtr<Cursor> make_group_by_dynamic(
    dstd::UniquePtr<Cursor> in, dstd::Vector<dstd::String> sch,
    dstd::String time_col, std::int64_t every, std::int64_t period,
    dstd::Vector<GroupAgg> aggs, std::int64_t origin, bool origin_min,
    std::uint64_t budget) {
    if (every <= 0)
        throw std::invalid_argument("group_by_dynamic: every must be > 0");
    const int time_idx = require_col(sch, time_col, "group_by_dynamic");
    dstd::Vector<int> value_idx;
    dstd::Vector<dstd::String> used;
    const auto resolve = [&](const dstd::String& name) -> std::int32_t {
        const auto it = std::find(used.begin(), used.end(), name);
        if (it != used.end())
            return static_cast<std::int32_t>(it - used.begin());
        value_idx.push_back(require_col(sch, name, "group_by"));
        used.push_back(name);
        return static_cast<std::int32_t>(used.size() - 1);
    };
    dstd::Vector<AggSpec> specs;
    specs.reserve(aggs.size());
    for (const GroupAgg& a : aggs) {
        AggSpec sp;
        sp.op = to_agg_op(a.op);
        sp.out = a.out;
        sp.param = a.param;
        sp.value_col = sp.op == AggOp::Count ? -1 : resolve(a.column);
        if (agg_uses_by_col(sp.op)) sp.by_col = resolve(a.by);
        specs.push_back(std::move(sp));
    }
    return dstd::make_unique<GroupByDynamicCursor>(
        std::move(in), std::move(time_col), time_idx, std::move(value_idx),
        std::move(specs), every, period <= 0 ? every : period, origin,
        origin_min, budget);
}

}  // namespace lazy_internal

}  // namespace dftracer::utils::dataframe
