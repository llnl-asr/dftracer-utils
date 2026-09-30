#include <dftracer/utils/core/common/hash/splitmix64.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/grace_join.h>
#include <dftracer/utils/duql/vectorize.h>

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace dftracer::utils::dataframe {

namespace {

constexpr std::size_t MIN_PARTS = 2;
constexpr std::size_t MAX_PARTS = 32;
constexpr int MAX_LEVEL = 4;
constexpr std::uint64_t SEED_STEP = 0x9e3779b97f4a7c15ULL;

std::size_t index_of(const std::vector<std::string>& names,
                     const std::string& name) {
    const auto it = std::find(names.begin(), names.end(), name);
    if (it == names.end())
        throw std::out_of_range("join: no column named " + name);
    return static_cast<std::size_t>(it - names.begin());
}

std::size_t parts_for(std::uint64_t bytes, std::uint64_t budget) {
    const std::uint64_t target = std::max<std::uint64_t>(budget / 2, 1);
    std::size_t n = MIN_PARTS;
    while (n < MAX_PARTS && static_cast<std::uint64_t>(n) * target < bytes)
        n *= 2;
    return n;
}

Series flat(const Series& c) {
    return c.encoding() == Encoding::Flat ? c.share() : c.materialize();
}

}  // namespace

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
                     std::string suffix, std::uint64_t budget)
    : left_names_(std::move(left_names)),
      right_names_(std::move(right_names)),
      left_on_(std::move(left_on)),
      right_on_(std::move(right_on)),
      how_(how),
      suffix_(std::move(suffix)),
      budget_(budget),
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
    const std::uint64_t seed = static_cast<std::uint64_t>(level) * SEED_STEP;
    const std::uint64_t mask = into.size() - 1;
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
        const std::uint64_t h =
            keyed ? hash::splitmix64(std::hash<std::string_view>{}(text) ^ seed)
                  : 0;
        picks[h & mask].push_back(r);
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
    if (parts.empty()) {
        DataFrame t;
        t.names = right_template_->names;
        for (const Series& c : right_template_->columns)
            t.columns.push_back(c.share());
        co_return t;
    }
    co_return merge_morsels(parts);
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
    for (auto it = subs.rbegin(); it != subs.rend(); ++it)
        queue_.push_front(Work{std::move(*it), level});
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
            if (work.level < MAX_LEVEL && p.right.total_bytes() > budget_ &&
                p.right_rows > 1) {
                co_await split_part(std::move(work), max_rows);
                continue;
            }
            DataFrame right = co_await right_rows(p, max_rows);
            cur_.emplace();
            cur_->join.emplace(std::move(right), left_on_, right_on_, how_,
                               suffix_);
            cur_->left = p.left.reader();
            cur_->part = std::move(work.part);
        }
        if (auto m = co_await cur_->left->next(max_rows)) {
            DataFrame left;
            left.names = left_names_;
            left.columns = std::move(m->columns);
            DataFrame out = cur_->join->probe(left);
            if (out.num_rows() > 0) co_return std::move(out);
            continue;
        }
        DataFrame rest = cur_->join->flush(left_names_, left_templates_);
        cur_.reset();
        if (rest.num_rows() > 0) co_return std::move(rest);
    }
}

}  // namespace dftracer::utils::dataframe
