// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/grace_join.h>
#include <dftracer/utils/dataframe/internal/budget_share.h>
#include <dftracer/utils/dataframe/internal/errors.h>
#include <dftracer/utils/dataframe/lazy/common.h>
#include <dftracer/utils/dstd/hash/partition.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/vectorize.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <stdexcept>
#include <utility>

namespace dftracer::utils::dataframe {

namespace {

constexpr std::size_t MIN_PARTS = 2;
constexpr std::size_t MAX_PARTS = 32;
constexpr int MAX_LEVEL = 4;
// Left rows probed before the first probe has shown how many a row makes.
constexpr std::int64_t FIRST_PROBE_ROWS = 8;

std::atomic<std::uint64_t> g_blocks{0};

// The joins whose left rows that no block matched leave once, at the end.
bool defers_left(JoinHow how) {
    return how == JoinHow::Left || how == JoinHow::Outer ||
           how == JoinHow::Semi || how == JoinHow::Anti || how == JoinHow::Nest;
}

dstd::Result<std::size_t> index_of(const dstd::Vector<dstd::String>& names,
                                   const dstd::String& name) {
    const auto it = std::find(names.begin(), names.end(), name);
    if (it == names.end()) return not_found<"join: no column named {}">(name);
    return static_cast<std::size_t>(it - names.begin());
}

std::size_t parts_for(std::uint64_t bytes, std::uint64_t budget) {
    const std::uint64_t target =
        std::max<std::uint64_t>(budget / share::SPILL_STATE, 1);
    std::size_t n = MIN_PARTS;
    while (n < MAX_PARTS && static_cast<std::uint64_t>(n) * target < bytes)
        n *= 2;
    return n;
}

Series flat(const Series& c) {
    return c.encoding() == Encoding::Flat ? c.share() : c.materialize();
}

std::uint64_t block_bytes(std::uint64_t budget) {
    return std::max<std::uint64_t>(budget / share::SPILL_STATE / 4, 1);
}

dstd::Vector<Series> flat_keys(const DataFrame& f,
                               const dstd::Vector<std::size_t>& key) {
    dstd::Vector<Series> out;
    out.reserve(key.size());
    for (const std::size_t k : key) out.push_back(flat(f.columns[k]));
    return out;
}

// The value key of row `r`; false when a part has none.
bool row_key(dstd::String& text, const dstd::Vector<Series>& keys,
             std::int64_t r) {
    text.clear();
    for (const Series& k : keys)
        if (!duql::append_cell_key(text, k, r)) return false;
    return true;
}

}  // namespace

std::uint64_t grace_join_blocks() {
    return g_blocks.load(std::memory_order_relaxed);
}

dstd::Result<DataFrame> merge_morsels(dstd::Vector<DataFrame>& parts) {
    if (parts.empty()) return DataFrame{};
    if (parts.size() == 1) return std::move(parts[0]);
    bool uniform = true;
    for (std::size_t i = 1; i < parts.size() && uniform; ++i) {
        if (parts[i].names != parts[0].names ||
            parts[i].columns.size() != parts[0].columns.size()) {
            uniform = false;
            break;
        }
        for (std::size_t c = 0; c < parts[0].columns.size(); ++c)
            if (parts[i].columns[c].type() != parts[0].columns[c].type())
                uniform = false;
    }
    dstd::Vector<const DataFrame*> ptrs;
    ptrs.reserve(parts.size());
    for (const DataFrame& p : parts) ptrs.push_back(&p);
    return concat(ptrs, uniform ? ConcatHow::Vertical : ConcatHow::Diagonal);
}

GraceJoin::GraceJoin(dstd::Vector<dstd::String> left_names,
                     dstd::Vector<dstd::String> right_names,
                     dstd::Vector<dstd::String> left_on,
                     dstd::Vector<dstd::String> right_on, JoinHow how,
                     dstd::String suffix, std::uint64_t budget,
                     bool nulls_equal, dstd::SharedPtr<spill::Dir> dir,
                     dstd::Vector<std::size_t> left_key,
                     dstd::Vector<std::size_t> right_key)
    : left_names_(std::move(left_names)),
      right_names_(std::move(right_names)),
      left_on_(std::move(left_on)),
      right_on_(std::move(right_on)),
      left_key_(std::move(left_key)),
      right_key_(std::move(right_key)),
      how_(how),
      suffix_(std::move(suffix)),
      budget_(budget),
      nulls_equal_(nulls_equal),
      dir_(std::move(dir)) {}

dstd::Result<dstd::UniquePtr<GraceJoin>> GraceJoin::make(
    dstd::Vector<dstd::String> left_names,
    dstd::Vector<dstd::String> right_names, dstd::Vector<dstd::String> left_on,
    dstd::Vector<dstd::String> right_on, JoinHow how, dstd::String suffix,
    std::uint64_t budget, bool nulls_equal) {
    if (left_on.size() != right_on.size() || left_on.empty())
        return invalid_argument<"join: uneven or empty key list">();
    dstd::Vector<std::size_t> left_key;
    for (const auto& n : left_on) {
        auto at = index_of(left_names, n);
        if (!at) return at.error();
        left_key.push_back(*at);
    }
    dstd::Vector<std::size_t> right_key;
    for (const auto& n : right_on) {
        auto at = index_of(right_names, n);
        if (!at) return at.error();
        right_key.push_back(*at);
    }
    auto dir = spill::Dir::create();
    if (!dir) return dir.error();
    return dstd::make_unique<GraceJoin>(
        std::move(left_names), std::move(right_names), std::move(left_on),
        std::move(right_on), how, std::move(suffix), budget, nulls_equal,
        std::move(*dir), std::move(left_key), std::move(right_key));
}

GraceJoin::Parts GraceJoin::make_parts(std::size_t n) const {
    Parts out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        out.push_back(dstd::make_unique<Part>(0, dir_));
    return out;
}

// Splits `rows` by the hash of their value keys into `into`.
dstd::Result<void> GraceJoin::split(const DataFrame& rows, bool right_side,
                                    int level, Parts& into) {
    const std::int64_t n = rows.num_rows();
    if (n == 0) return {};
    const dstd::Vector<std::size_t>& key = right_side ? right_key_ : left_key_;
    dstd::Vector<Series> flats;
    flats.reserve(rows.columns.size());
    for (const Series& c : rows.columns) flats.push_back(flat(c));
    dstd::Vector<dstd::Vector<std::int64_t>> picks(into.size());
    dstd::String text;
    for (std::int64_t r = 0; r < n; ++r) {
        text.clear();
        bool keyed = true;
        for (const std::size_t k : key)
            if (!duql::append_cell_key(text, flats[k], r)) {
                keyed = false;
                break;
            }
        picks[keyed ? dstd::hash::partition_of(dstd::StringView(text),
                                               into.size(), level)
                    : 0]
            .push_back(r);
    }
    for (std::size_t p = 0; p < into.size(); ++p) {
        if (picks[p].empty()) continue;
        dstd::Vector<Series> cols;
        cols.reserve(flats.size());
        for (const Series& c : flats) cols.push_back(c.take(picks[p]));
        const auto count = static_cast<std::int64_t>(picks[p].size());
        if (right_side) {
            auto added = into[p]->right.add(std::move(cols), count);
            if (!added) return added;
            into[p]->right_rows += count;
        } else {
            auto added = into[p]->left.add(std::move(cols), count);
            if (!added) return added;
            into[p]->left_rows += count;
        }
    }
    return {};
}

dstd::Result<void> GraceJoin::add_right(const DataFrame& right) {
    if (right.names != right_names_)
        return invalid_argument<
            "join: a right side whose columns change between morsels cannot "
            "spill">();
    if (top_.empty()) top_ = make_parts(MAX_PARTS);
    if (!right_template_) {
        DataFrame t;
        t.names = right_names_;
        for (const Series& c : right.columns)
            t.columns.push_back(c.take(dstd::Vector<std::int64_t>{}));
        right_template_ = std::move(t);
    }
    return split(right, true, 0, top_);
}

dstd::Result<void> GraceJoin::add_left(const DataFrame& left) {
    return split(left, false, 0, top_);
}

void GraceJoin::start(dstd::Vector<Series> left_templates) {
    left_templates_ = std::move(left_templates);
    for (auto& p : top_) queue_.push_back(Work{std::move(p), 0});
    top_.clear();
}

coro::CoroTask<dstd::Result<DataFrame>> GraceJoin::right_rows(
    Part& part, std::int64_t max_rows) {
    dstd::Vector<DataFrame> parts;
    auto opened = part.right.reader();
    if (!opened) co_return opened.error();
    auto reader = std::move(*opened);
    for (;;) {
        auto task = reader->next(max_rows);
        auto m = co_await std::move(task).or_fail();
        if (!m) break;
        DataFrame f;
        f.names = right_names_;
        f.columns = std::move(m->columns);
        parts.push_back(std::move(f));
    }
    if (parts.empty()) co_return empty_right();
    co_return merge_morsels(parts);
}

DataFrame GraceJoin::empty_right() const {
    DataFrame t;
    t.names = right_template_->names;
    for (const Series& c : right_template_->columns)
        t.columns.push_back(c.share());
    return t;
}

// Splits a partition whose right side is over the budget again, with the
// seed of the next level, and queues its parts first.
coro::CoroTask<dstd::Result<void>> GraceJoin::split_part(
    Work work, std::int64_t max_rows) {
    const std::size_t n = parts_for(work.part->right.total_bytes(), budget_);
    Parts subs = make_parts(n);
    const int level = work.level + 1;
    {
        auto opened = work.part->right.reader();
        if (!opened) co_return opened.error();
        auto reader = std::move(*opened);
        for (;;) {
            auto task = reader->next(max_rows);
            auto m = co_await std::move(task).or_fail();
            if (!m) break;
            DataFrame f;
            f.names = right_names_;
            f.columns = std::move(m->columns);
            auto split_ok = split(f, true, level, subs);
            if (!split_ok) co_return split_ok;
        }
    }
    {
        auto opened = work.part->left.reader();
        if (!opened) co_return opened.error();
        auto reader = std::move(*opened);
        for (;;) {
            auto task = reader->next(max_rows);
            auto m = co_await std::move(task).or_fail();
            if (!m) break;
            DataFrame f;
            f.names = left_names_;
            f.columns = std::move(m->columns);
            auto split_ok = split(f, false, level, subs);
            if (!split_ok) co_return split_ok;
        }
    }
    // One key holds the rows when a split leaves them all in one part: that
    // part goes block by block instead of being split again.
    const std::int64_t rows = work.part->right_rows;
    for (auto it = subs.rbegin(); it != subs.rend(); ++it) {
        const bool uncut = (*it)->right_rows == rows;
        queue_.push_front(Work{std::move(*it), level, uncut});
    }
    co_return dstd::Result<void>{};
}

// The next block of the right side of the current partition, about a quarter
// of the budget share; nothing when it is used up.
coro::CoroTask<FrameResult> GraceJoin::right_block(std::int64_t max_rows) {
    dstd::Vector<DataFrame> parts;
    std::uint64_t bytes = 0;
    const std::uint64_t limit = block_bytes(budget_);
    while (bytes < limit) {
        auto task = cur_->right->next(max_rows);
        auto m = co_await std::move(task).or_fail();
        if (!m) break;
        bytes += spill::columns_bytes(m->columns);
        DataFrame f;
        f.names = right_names_;
        f.columns = std::move(m->columns);
        parts.push_back(std::move(f));
    }
    if (parts.empty()) co_return dstd::nullopt;
    g_blocks.fetch_add(1, std::memory_order_relaxed);
    auto merged = merge_morsels(parts);
    if (!merged) co_return merged.error();
    co_return dstd::Optional<DataFrame>(std::move(*merged));
}

// Lookup over a partition joined block by block: the first right row of each
// key, as one frame in order of first appearance, and in `conflicts` per row
// of it the first value column in which a later row of its key differs (-1
// for none), found across blocks as the in-memory join finds it.
coro::CoroTask<dstd::Result<DataFrame>> GraceJoin::lookup_heads(
    dstd::Vector<std::int64_t>& conflicts, std::int64_t max_rows) {
    struct Head {
        std::size_t part;
        std::int64_t row;
    };
    dstd::StringViewMap<std::size_t> index;
    dstd::Vector<Head> heads;
    dstd::Vector<DataFrame> frames;
    dstd::Vector<std::size_t> values;
    for (std::size_t c = 0; c < right_names_.size(); ++c)
        if (std::find(right_key_.begin(), right_key_.end(), c) ==
            right_key_.end())
            values.push_back(c);
    dstd::String text;
    for (;;) {
        auto block_task = right_block(max_rows);
        auto block = co_await std::move(block_task).or_fail();
        if (!block) break;
        const dstd::Vector<Series> keys = flat_keys(*block, right_key_);
        const std::int64_t n = block->num_rows();
        const std::size_t part = frames.size();
        dstd::Vector<std::int64_t> picks;
        dstd::Vector<std::int64_t> at(static_cast<std::size_t>(n), -1);
        for (std::int64_t r = 0; r < n; ++r) {
            if (!row_key(text, keys, r)) continue;
            const auto [it, fresh] = index.try_emplace(text, heads.size());
            if (fresh) {
                heads.push_back(
                    Head{part, static_cast<std::int64_t>(picks.size())});
                picks.push_back(r);
                conflicts.push_back(-1);
            }
            at[static_cast<std::size_t>(r)] =
                static_cast<std::int64_t>(it->second);
        }
        if (!picks.empty()) {
            DataFrame f;
            f.names = right_names_;
            for (const Series& c : block->columns)
                f.columns.push_back(c.take(picks));
            frames.push_back(std::move(f));
        }
        for (std::int64_t r = 0; r < n; ++r) {
            const std::int64_t i = at[static_cast<std::size_t>(r)];
            if (i < 0 || conflicts[static_cast<std::size_t>(i)] >= 0) continue;
            const Head& h = heads[static_cast<std::size_t>(i)];
            if (h.part == part && picks[static_cast<std::size_t>(h.row)] == r)
                continue;
            const DataFrame& head = frames[h.part];
            for (const std::size_t c : values)
                if (!duql::same_cell(head.columns[c], h.row, block->columns[c],
                                     r)) {
                    conflicts[static_cast<std::size_t>(i)] =
                        static_cast<std::int64_t>(c);
                    break;
                }
        }
    }
    if (frames.empty()) co_return empty_right();
    co_return merge_morsels(frames);
}

// Nest over a partition joined block by block: reads the right side once,
// totals the bytes of each key from the average row of its morsel, and packs
// the keys into groups of about a block; a key past a block is a group alone.
coro::CoroTask<dstd::Result<void>> GraceJoin::assign_groups(
    std::int64_t max_rows) {
    Current& c = *cur_;
    dstd::String text;
    auto opened = c.part->right.reader();
    if (!opened) co_return opened.error();
    auto reader = std::move(*opened);
    for (;;) {
        auto task = reader->next(max_rows);
        auto m = co_await std::move(task).or_fail();
        if (!m) break;
        DataFrame f;
        f.names = right_names_;
        f.columns = std::move(m->columns);
        const std::int64_t n = f.num_rows();
        if (n == 0) continue;
        const std::uint64_t row_bytes = std::max<std::uint64_t>(
            spill::columns_bytes(f.columns) / static_cast<std::uint64_t>(n), 1);
        const dstd::Vector<Series> keys = flat_keys(f, right_key_);
        for (std::int64_t r = 0; r < n; ++r)
            if (row_key(text, keys, r)) c.group_of[text] += row_bytes;
    }
    const std::uint64_t limit = block_bytes(budget_);
    std::uint64_t used = 0;
    std::uint64_t group = 0;
    for (auto& entry : c.group_of) {
        if (used > 0 && used + entry.second > limit) {
            ++group;
            used = 0;
        }
        used += entry.second;
        entry.second = group;
    }
    c.group = 0;
    c.groups = c.group_of.empty() ? 0 : group + 1;
    co_return dstd::Result<void>{};
}

// The right rows of the next key group, in right order; nothing after the
// last group.
coro::CoroTask<FrameResult> GraceJoin::next_group(std::int64_t max_rows) {
    Current& c = *cur_;
    if (c.group >= c.groups) co_return dstd::nullopt;
    const std::uint64_t want = c.group++;
    g_blocks.fetch_add(1, std::memory_order_relaxed);
    auto opened = c.part->right.reader();
    if (!opened) co_return opened.error();
    c.right = std::move(*opened);
    dstd::Vector<DataFrame> parts;
    dstd::String text;
    for (;;) {
        auto task = c.right->next(max_rows);
        auto m = co_await std::move(task).or_fail();
        if (!m) break;
        DataFrame f;
        f.names = right_names_;
        f.columns = std::move(m->columns);
        const std::int64_t n = f.num_rows();
        const dstd::Vector<Series> keys = flat_keys(f, right_key_);
        dstd::Vector<std::int64_t> picks;
        for (std::int64_t r = 0; r < n; ++r) {
            if (!row_key(text, keys, r)) continue;
            const auto it = c.group_of.find(text);
            if (it != c.group_of.end() && it->second == want)
                picks.push_back(r);
        }
        if (picks.empty()) continue;
        DataFrame g;
        g.names = right_names_;
        for (const Series& col : f.columns)
            g.columns.push_back(col.take(picks));
        parts.push_back(std::move(g));
    }
    if (parts.empty()) co_return dstd::Optional<DataFrame>(empty_right());
    auto merged = merge_morsels(parts);
    if (!merged) co_return merged.error();
    co_return dstd::Optional<DataFrame>(std::move(*merged));
}

// Reads the next block of the right side of a partition joined block by
// block (for Nest, the next key group) and builds its join; the left
// partition is then read again from the start. False when the right side is
// used up.
coro::CoroTask<dstd::Result<bool>> GraceJoin::next_block(
    std::int64_t max_rows) {
    dstd::Optional<DataFrame> block;
    if (how_ == JoinHow::Nest) {
        auto group_task = next_group(max_rows);
        block = co_await std::move(group_task).or_fail();
    } else {
        auto block_task = right_block(max_rows);
        block = co_await std::move(block_task).or_fail();
    }
    if (!block) co_return false;
    cur_->join.reset();
    auto joined = HashJoin::make(std::move(*block), left_on_, right_on_, how_,
                                 suffix_, nulls_equal_);
    if (!joined) co_return joined.error();
    cur_->join.emplace(std::move(*joined));
    auto left = cur_->part->left.reader();
    if (!left) co_return left.error();
    cur_->left = std::move(*left);
    cur_->left_rows.reset();
    cur_->left_at = 0;
    cur_->left_base = 0;
    co_return true;
}

// The next probe of the current join: as many left rows as the output share
// allows from what the last probes made. Nothing once the left side is read.
coro::CoroTask<FrameResult> GraceJoin::probe_left(std::int64_t max_rows) {
    Current& c = *cur_;
    while (!c.left_rows || c.left_at >= c.left_rows->num_rows()) {
        if (c.left_rows) c.left_base += c.left_rows->num_rows();
        auto task = c.left->next(max_rows);
        auto m = co_await std::move(task).or_fail();
        if (!m) {
            c.left_rows.reset();
            co_return dstd::nullopt;
        }
        DataFrame f;
        f.names = left_names_;
        f.columns = std::move(m->columns);
        c.left_rows = std::move(f);
        c.left_at = 0;
    }
    const std::int64_t left = c.left_rows->num_rows() - c.left_at;
    const std::int64_t k = c.out_row_bytes == 0
                               ? std::min(left, FIRST_PROBE_ROWS)
                               : lazy_internal::rows_within(
                                     lazy_internal::output_share(budget_),
                                     static_cast<std::uint64_t>(
                                         static_cast<double>(c.out_row_bytes) *
                                         std::max(c.expansion, 1.0)),
                                     left);
    const DataFrame piece = c.left_rows->slice(c.left_at, k);
    auto probed =
        c.blocks && defers_left(how_)
            ? c.join->probe(piece, &c.matched, c.left_base + c.left_at)
            : c.join->probe(piece);
    if (!probed) co_return probed.error();
    DataFrame out = std::move(*probed);
    c.left_at += k;
    const std::int64_t made = out.num_rows();
    const double e = static_cast<double>(made) / static_cast<double>(k);
    c.expansion = std::max(e, c.expansion * 0.5);
    if (made > 0)
        c.out_row_bytes =
            std::max<std::uint64_t>(spill::columns_bytes(out.columns) /
                                        static_cast<std::uint64_t>(made),
                                    1);
    co_return dstd::Optional<DataFrame>(std::move(out));
}

// After the last block: the left rows that leave on no match anywhere (Left,
// Outer and Anti), or on a match in some block (Semi), one left morsel at a
// time, joined against an empty right side for the join's output layout.
coro::CoroTask<FrameResult> GraceJoin::deferred_left(std::int64_t max_rows) {
    Current& c = *cur_;
    const std::uint8_t keep = how_ == JoinHow::Semi ? 1 : 0;
    for (;;) {
        auto task = c.left->next(max_rows);
        auto m = co_await std::move(task).or_fail();
        if (!m) break;
        DataFrame f;
        f.names = left_names_;
        f.columns = std::move(m->columns);
        const std::int64_t n = f.num_rows();
        dstd::Vector<std::int64_t> picks;
        for (std::int64_t i = 0; i < n; ++i)
            if (c.matched[static_cast<std::size_t>(c.left_base + i)] == keep)
                picks.push_back(i);
        c.left_base += n;
        if (picks.empty()) continue;
        auto probed = c.join->probe(f.take(picks));
        if (!probed) co_return probed.error();
        co_return dstd::Optional<DataFrame>(std::move(*probed));
    }
    co_return dstd::nullopt;
}

// An empty right side gives the output layout: every row a probe sees is
// unmatched, and an Anti join keeps them all.
dstd::Result<void> GraceJoin::start_deferred() {
    cur_->join.reset();
    auto joined = HashJoin::make(empty_right(), left_on_, right_on_,
                                 how_ == JoinHow::Semi ? JoinHow::Anti : how_,
                                 suffix_, nulls_equal_);
    if (!joined) return joined.error();
    cur_->join.emplace(std::move(*joined));
    auto left = cur_->part->left.reader();
    if (!left) return left.error();
    cur_->left = std::move(*left);
    cur_->left_base = 0;
    cur_->last_pass = true;
    return {};
}

coro::CoroTask<FrameResult> GraceJoin::next(std::int64_t max_rows) {
    for (;;) {
        if (!cur_) {
            if (queue_.empty()) co_return dstd::nullopt;
            Work work = std::move(queue_.front());
            queue_.pop_front();
            Part& p = *work.part;
            if (p.right_rows == 0 && p.left_rows == 0) continue;
            const bool over =
                p.right.total_bytes() > budget_ && p.right_rows > 1;
            if (over && !work.blocks && work.level < MAX_LEVEL) {
                auto splitting = split_part(std::move(work), max_rows);
                auto split_ok = co_await std::move(splitting);
                if (!split_ok) co_return split_ok.error();
                continue;
            }
            cur_.emplace();
            cur_->part = std::move(work.part);
            if (over && how_ == JoinHow::Lookup) {
                auto right = cur_->part->right.reader();
                if (!right) co_return right.error();
                cur_->right = std::move(*right);
                dstd::Vector<std::int64_t> conflicts;
                auto heads_task = lookup_heads(conflicts, max_rows);
                DataFrame heads = co_await std::move(heads_task).or_fail();
                auto joined =
                    HashJoin::make(std::move(heads), left_on_, right_on_, how_,
                                   suffix_, nulls_equal_);
                if (!joined) co_return joined.error();
                cur_->join.emplace(std::move(*joined));
                cur_->join->set_lookup_conflicts(std::move(conflicts));
                auto left = cur_->part->left.reader();
                if (!left) co_return left.error();
                cur_->left = std::move(*left);
            } else if (over) {
                cur_->blocks = true;
                auto right = cur_->part->right.reader();
                if (!right) co_return right.error();
                cur_->right = std::move(*right);
                if (defers_left(how_))
                    cur_->matched.assign(
                        static_cast<std::size_t>(cur_->part->left_rows), 0);
                if (how_ == JoinHow::Nest) {
                    auto grouping = assign_groups(max_rows);
                    auto grouped = co_await std::move(grouping);
                    if (!grouped) co_return grouped.error();
                }
                auto block_task = next_block(max_rows);
                auto next_block_ok = co_await std::move(block_task);
                if (!next_block_ok.has_value()) co_return next_block_ok.error();
                if (!next_block_ok.value()) {
                    auto deferred = start_deferred();
                    if (!deferred) co_return deferred.error();
                }
            } else {
                auto right_task = right_rows(*cur_->part, max_rows);
                DataFrame right = co_await std::move(right_task).or_fail();
                auto joined =
                    HashJoin::make(std::move(right), left_on_, right_on_, how_,
                                   suffix_, nulls_equal_);
                if (!joined) co_return joined.error();
                cur_->join.emplace(std::move(*joined));
                auto left = cur_->part->left.reader();
                if (!left) co_return left.error();
                cur_->left = std::move(*left);
            }
        }
        if (cur_->last_pass) {
            auto deferred_task = deferred_left(max_rows);
            auto out = co_await std::move(deferred_task).or_fail();
            if (out) {
                co_return dstd::Optional<DataFrame>(std::move(*out));
            }
            cur_.reset();
            continue;
        }
        auto probe_task = probe_left(max_rows);
        auto out = co_await std::move(probe_task).or_fail();
        if (out) {
            if (out->num_rows() > 0)
                co_return dstd::Optional<DataFrame>(std::move(*out));
            continue;
        }
        DataFrame rest = cur_->join->flush(left_names_, left_templates_);
        bool exhausted = false;
        if (cur_->blocks) {
            auto block_task = next_block(max_rows);
            auto more = co_await std::move(block_task);
            if (!more.has_value()) co_return more.error();
            exhausted = !more.value();
        }
        if (exhausted) {
            if (defers_left(how_)) {
                auto deferred = start_deferred();
                if (!deferred) co_return deferred.error();
            } else {
                cur_.reset();
            }
        } else if (!cur_->blocks) {
            cur_.reset();
        }
        if (rest.num_rows() > 0)
            co_return dstd::Optional<DataFrame>(std::move(rest));
    }
}

}  // namespace dftracer::utils::dataframe
