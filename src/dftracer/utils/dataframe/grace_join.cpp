#include <dftracer/utils/core/common/hash/partition.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/grace_join.h>
#include <dftracer/utils/dataframe/internal/budget_share.h>
#include <dftracer/utils/dataframe/lazy/common.h>
#include <dftracer/utils/duql/vectorize.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <stdexcept>
#include <string_view>
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

std::size_t index_of(const std::vector<std::string>& names,
                     const std::string& name) {
    const auto it = std::find(names.begin(), names.end(), name);
    if (it == names.end())
        throw std::out_of_range("join: no column named " + name);
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

std::vector<Series> flat_keys(const DataFrame& f,
                              const std::vector<std::size_t>& key) {
    std::vector<Series> out;
    out.reserve(key.size());
    for (const std::size_t k : key) out.push_back(flat(f.columns[k]));
    return out;
}

// The value key of row `r`; false when a part has none.
bool row_key(std::string& text, const std::vector<Series>& keys,
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

DataFrame merge_morsels(std::vector<DataFrame>& parts) {
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
    std::vector<const DataFrame*> ptrs;
    ptrs.reserve(parts.size());
    for (const DataFrame& p : parts) ptrs.push_back(&p);
    return concat(ptrs, uniform ? ConcatHow::Vertical : ConcatHow::Diagonal);
}

GraceJoin::GraceJoin(std::vector<std::string> left_names,
                     std::vector<std::string> right_names,
                     std::vector<std::string> left_on,
                     std::vector<std::string> right_on, JoinHow how,
                     std::string suffix, std::uint64_t budget, bool nulls_equal)
    : left_names_(std::move(left_names)),
      right_names_(std::move(right_names)),
      left_on_(std::move(left_on)),
      right_on_(std::move(right_on)),
      how_(how),
      suffix_(std::move(suffix)),
      budget_(budget),
      nulls_equal_(nulls_equal),
      dir_(std::make_shared<spill::Dir>()) {
    if (left_on_.size() != right_on_.size() || left_on_.empty())
        throw std::invalid_argument("join: uneven or empty key list");
    for (const auto& n : left_on_)
        left_key_.push_back(index_of(left_names_, n));
    for (const auto& n : right_on_)
        right_key_.push_back(index_of(right_names_, n));
}

GraceJoin::Parts GraceJoin::make_parts(std::size_t n) const {
    Parts out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        out.push_back(std::make_unique<Part>(0, dir_));
    return out;
}

// Splits `rows` by the hash of their value keys into `into`.
void GraceJoin::split(const DataFrame& rows, bool right_side, int level,
                      Parts& into) {
    const std::int64_t n = rows.num_rows();
    if (n == 0) return;
    const std::vector<std::size_t>& key = right_side ? right_key_ : left_key_;
    std::vector<Series> flats;
    flats.reserve(rows.columns.size());
    for (const Series& c : rows.columns) flats.push_back(flat(c));
    std::vector<std::vector<std::int64_t>> picks(into.size());
    std::string text;
    for (std::int64_t r = 0; r < n; ++r) {
        text.clear();
        bool keyed = true;
        for (const std::size_t k : key)
            if (!duql::append_cell_key(text, flats[k], r)) {
                keyed = false;
                break;
            }
        picks[keyed ? hash::partition_of(std::string_view(text), into.size(),
                                         level)
                    : 0]
            .push_back(r);
    }
    for (std::size_t p = 0; p < into.size(); ++p) {
        if (picks[p].empty()) continue;
        std::vector<Series> cols;
        cols.reserve(flats.size());
        for (const Series& c : flats) cols.push_back(c.take(picks[p]));
        const auto count = static_cast<std::int64_t>(picks[p].size());
        if (right_side) {
            into[p]->right.add(std::move(cols), count);
            into[p]->right_rows += count;
        } else {
            into[p]->left.add(std::move(cols), count);
            into[p]->left_rows += count;
        }
    }
}

void GraceJoin::add_right(const DataFrame& right) {
    if (right.names != right_names_)
        throw std::invalid_argument(
            "join: a right side whose columns change between morsels cannot "
            "spill");
    if (top_.empty()) top_ = make_parts(MAX_PARTS);
    if (!right_template_) {
        DataFrame t;
        t.names = right_names_;
        for (const Series& c : right.columns)
            t.columns.push_back(c.take(std::vector<std::int64_t>{}));
        right_template_ = std::move(t);
    }
    split(right, true, 0, top_);
}

void GraceJoin::add_left(const DataFrame& left) { split(left, false, 0, top_); }

void GraceJoin::start(std::vector<Series> left_templates) {
    left_templates_ = std::move(left_templates);
    for (auto& p : top_) queue_.push_back(Work{std::move(p), 0});
    top_.clear();
}

coro::CoroTask<DataFrame> GraceJoin::right_rows(Part& part,
                                                std::int64_t max_rows) {
    std::vector<DataFrame> parts;
    auto reader = part.right.reader();
    while (auto m = co_await reader->next(max_rows)) {
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
coro::CoroTask<void> GraceJoin::split_part(Work work, std::int64_t max_rows) {
    const std::size_t n = parts_for(work.part->right.total_bytes(), budget_);
    Parts subs = make_parts(n);
    const int level = work.level + 1;
    {
        auto reader = work.part->right.reader();
        while (auto m = co_await reader->next(max_rows)) {
            DataFrame f;
            f.names = right_names_;
            f.columns = std::move(m->columns);
            split(f, true, level, subs);
        }
    }
    {
        auto reader = work.part->left.reader();
        while (auto m = co_await reader->next(max_rows)) {
            DataFrame f;
            f.names = left_names_;
            f.columns = std::move(m->columns);
            split(f, false, level, subs);
        }
    }
    // One key holds the rows when a split leaves them all in one part: that
    // part goes block by block instead of being split again.
    const std::int64_t rows = work.part->right_rows;
    for (auto it = subs.rbegin(); it != subs.rend(); ++it) {
        const bool uncut = (*it)->right_rows == rows;
        queue_.push_front(Work{std::move(*it), level, uncut});
    }
}

// The next block of the right side of the current partition, about a quarter
// of the budget share; nothing when it is used up.
coro::CoroTask<std::optional<DataFrame>> GraceJoin::right_block(
    std::int64_t max_rows) {
    std::vector<DataFrame> parts;
    std::uint64_t bytes = 0;
    const std::uint64_t limit = block_bytes(budget_);
    while (bytes < limit) {
        auto m = co_await cur_->right->next(max_rows);
        if (!m) break;
        bytes += spill::columns_bytes(m->columns);
        DataFrame f;
        f.names = right_names_;
        f.columns = std::move(m->columns);
        parts.push_back(std::move(f));
    }
    if (parts.empty()) co_return std::nullopt;
    g_blocks.fetch_add(1, std::memory_order_relaxed);
    co_return merge_morsels(parts);
}

// Lookup over a partition joined block by block: the first right row of each
// key, as one frame in order of first appearance, and in `conflicts` per row
// of it the first value column in which a later row of its key differs (-1
// for none), found across blocks as the in-memory join finds it.
coro::CoroTask<DataFrame> GraceJoin::lookup_heads(
    std::vector<std::int64_t>& conflicts, std::int64_t max_rows) {
    struct Head {
        std::size_t part;
        std::int64_t row;
    };
    StringViewMap<std::size_t> index;
    std::vector<Head> heads;
    std::vector<DataFrame> frames;
    std::vector<std::size_t> values;
    for (std::size_t c = 0; c < right_names_.size(); ++c)
        if (std::find(right_key_.begin(), right_key_.end(), c) ==
            right_key_.end())
            values.push_back(c);
    std::string text;
    while (auto block = co_await right_block(max_rows)) {
        const std::vector<Series> keys = flat_keys(*block, right_key_);
        const std::int64_t n = block->num_rows();
        const std::size_t part = frames.size();
        std::vector<std::int64_t> picks;
        std::vector<std::int64_t> at(static_cast<std::size_t>(n), -1);
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
coro::CoroTask<void> GraceJoin::assign_groups(std::int64_t max_rows) {
    Current& c = *cur_;
    std::string text;
    auto reader = c.part->right.reader();
    while (auto m = co_await reader->next(max_rows)) {
        DataFrame f;
        f.names = right_names_;
        f.columns = std::move(m->columns);
        const std::int64_t n = f.num_rows();
        if (n == 0) continue;
        const std::uint64_t row_bytes = std::max<std::uint64_t>(
            spill::columns_bytes(f.columns) / static_cast<std::uint64_t>(n), 1);
        const std::vector<Series> keys = flat_keys(f, right_key_);
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
}

// The right rows of the next key group, in right order; nothing after the
// last group.
coro::CoroTask<std::optional<DataFrame>> GraceJoin::next_group(
    std::int64_t max_rows) {
    Current& c = *cur_;
    if (c.group >= c.groups) co_return std::nullopt;
    const std::uint64_t want = c.group++;
    g_blocks.fetch_add(1, std::memory_order_relaxed);
    c.right = c.part->right.reader();
    std::vector<DataFrame> parts;
    std::string text;
    while (auto m = co_await c.right->next(max_rows)) {
        DataFrame f;
        f.names = right_names_;
        f.columns = std::move(m->columns);
        const std::int64_t n = f.num_rows();
        const std::vector<Series> keys = flat_keys(f, right_key_);
        std::vector<std::int64_t> picks;
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
    if (parts.empty()) co_return empty_right();
    co_return merge_morsels(parts);
}

// Reads the next block of the right side of a partition joined block by
// block (for Nest, the next key group) and builds its join; the left
// partition is then read again from the start. False when the right side is
// used up.
coro::CoroTask<bool> GraceJoin::next_block(std::int64_t max_rows) {
    std::optional<DataFrame> block;
    if (how_ == JoinHow::Nest)
        block = co_await next_group(max_rows);
    else
        block = co_await right_block(max_rows);
    if (!block) co_return false;
    cur_->join.reset();
    cur_->join.emplace(std::move(*block), left_on_, right_on_, how_, suffix_,
                       nulls_equal_);
    cur_->left = cur_->part->left.reader();
    cur_->left_rows.reset();
    cur_->left_at = 0;
    cur_->left_base = 0;
    co_return true;
}

// The next probe of the current join: as many left rows as the output share
// allows from what the last probes made. Nothing once the left side is read.
coro::CoroTask<std::optional<DataFrame>> GraceJoin::probe_left(
    std::int64_t max_rows) {
    Current& c = *cur_;
    while (!c.left_rows || c.left_at >= c.left_rows->num_rows()) {
        if (c.left_rows) c.left_base += c.left_rows->num_rows();
        auto m = co_await c.left->next(max_rows);
        if (!m) {
            c.left_rows.reset();
            co_return std::nullopt;
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
    DataFrame out =
        c.blocks && defers_left(how_)
            ? c.join->probe(piece, &c.matched, c.left_base + c.left_at)
            : c.join->probe(piece);
    c.left_at += k;
    const std::int64_t made = out.num_rows();
    const double e = static_cast<double>(made) / static_cast<double>(k);
    c.expansion = std::max(e, c.expansion * 0.5);
    if (made > 0)
        c.out_row_bytes =
            std::max<std::uint64_t>(spill::columns_bytes(out.columns) /
                                        static_cast<std::uint64_t>(made),
                                    1);
    co_return out;
}

// After the last block: the left rows that leave on no match anywhere (Left,
// Outer and Anti), or on a match in some block (Semi), one left morsel at a
// time, joined against an empty right side for the join's output layout.
coro::CoroTask<std::optional<DataFrame>> GraceJoin::deferred_left(
    std::int64_t max_rows) {
    Current& c = *cur_;
    const std::uint8_t keep = how_ == JoinHow::Semi ? 1 : 0;
    while (auto m = co_await c.left->next(max_rows)) {
        DataFrame f;
        f.names = left_names_;
        f.columns = std::move(m->columns);
        const std::int64_t n = f.num_rows();
        std::vector<std::int64_t> picks;
        for (std::int64_t i = 0; i < n; ++i)
            if (c.matched[static_cast<std::size_t>(c.left_base + i)] == keep)
                picks.push_back(i);
        c.left_base += n;
        if (picks.empty()) continue;
        co_return c.join->probe(f.take(picks));
    }
    co_return std::nullopt;
}

// An empty right side gives the output layout: every row a probe sees is
// unmatched, and an Anti join keeps them all.
void GraceJoin::start_deferred() {
    cur_->join.reset();
    cur_->join.emplace(empty_right(), left_on_, right_on_,
                       how_ == JoinHow::Semi ? JoinHow::Anti : how_, suffix_,
                       nulls_equal_);
    cur_->left = cur_->part->left.reader();
    cur_->left_base = 0;
    cur_->last_pass = true;
}

coro::CoroTask<std::optional<DataFrame>> GraceJoin::next(
    std::int64_t max_rows) {
    for (;;) {
        if (!cur_) {
            if (queue_.empty()) co_return std::nullopt;
            Work work = std::move(queue_.front());
            queue_.pop_front();
            Part& p = *work.part;
            if (p.right_rows == 0 && p.left_rows == 0) continue;
            const bool over =
                p.right.total_bytes() > budget_ && p.right_rows > 1;
            if (over && !work.blocks && work.level < MAX_LEVEL) {
                co_await split_part(std::move(work), max_rows);
                continue;
            }
            cur_.emplace();
            cur_->part = std::move(work.part);
            if (over && how_ == JoinHow::Lookup) {
                cur_->right = cur_->part->right.reader();
                std::vector<std::int64_t> conflicts;
                DataFrame heads = co_await lookup_heads(conflicts, max_rows);
                cur_->join.emplace(std::move(heads), left_on_, right_on_, how_,
                                   suffix_, nulls_equal_);
                cur_->join->set_lookup_conflicts(std::move(conflicts));
                cur_->left = cur_->part->left.reader();
            } else if (over) {
                cur_->blocks = true;
                cur_->right = cur_->part->right.reader();
                if (defers_left(how_))
                    cur_->matched.assign(
                        static_cast<std::size_t>(cur_->part->left_rows), 0);
                if (how_ == JoinHow::Nest) co_await assign_groups(max_rows);
                if (!co_await next_block(max_rows)) start_deferred();
            } else {
                DataFrame right = co_await right_rows(*cur_->part, max_rows);
                cur_->join.emplace(std::move(right), left_on_, right_on_, how_,
                                   suffix_, nulls_equal_);
                cur_->left = cur_->part->left.reader();
            }
        }
        if (cur_->last_pass) {
            if (auto out = co_await deferred_left(max_rows)) {
                co_return std::move(*out);
            }
            cur_.reset();
            continue;
        }
        if (auto out = co_await probe_left(max_rows)) {
            if (out->num_rows() > 0) co_return std::move(*out);
            continue;
        }
        DataFrame rest = cur_->join->flush(left_names_, left_templates_);
        if (cur_->blocks && !co_await next_block(max_rows)) {
            if (defers_left(how_))
                start_deferred();
            else
                cur_.reset();
        } else if (!cur_->blocks) {
            cur_.reset();
        }
        if (rest.num_rows() > 0) co_return std::move(rest);
    }
}

}  // namespace dftracer::utils::dataframe
