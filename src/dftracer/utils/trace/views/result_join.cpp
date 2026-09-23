#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/hash/constants.h>
#include <dftracer/utils/dataframe/kernels/filter.h>
#include <dftracer/utils/trace/views/result_join.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dataframe = dftracer::utils::dataframe;

namespace dftracer::utils::trace::views {

namespace {

struct KeyHash {
    std::size_t operator()(const std::vector<std::string>& k) const {
        std::size_t h = dftracer::utils::hash::FNV1A_OFFSET_BASIS_LEGACY;
        for (const std::string& s : k) {
            std::size_t sh = std::hash<std::string>{}(s);
            h ^= sh + dftracer::utils::hash::GOLDEN_RATIO + (h << 6) + (h >> 2);
        }
        return h;
    }
};

std::vector<std::string> key_of(const dataframe::DataFrame& b,
                                std::int64_t n_key, std::int64_t row) {
    std::vector<std::string> k;
    k.reserve(static_cast<std::size_t>(n_key));
    for (std::int64_t i = 0; i < n_key; ++i)
        k.emplace_back(b.columns[static_cast<std::size_t>(i)].string_at(row));
    return k;
}

}  // namespace

dataframe::DataFrame join_batches(const dataframe::DataFrame& left,
                                  const dataframe::DataFrame& right,
                                  std::int64_t n_key, JoinType type) {
    // The key schema is the first n_key column names; they must agree.
    if (n_key > static_cast<std::int64_t>(right.names.size())) return {};
    for (std::int64_t i = 0; i < n_key; ++i)
        if (left.names[static_cast<std::size_t>(i)] !=
            right.names[static_cast<std::size_t>(i)])
            return {};

    // Keys are matched via string_at(), which reads the offset buffer a
    // non-string column does not have. Aggregation keys are always strings;
    // reject a non-string key loudly instead of dereferencing null.
    for (std::int64_t i = 0; i < n_key; ++i)
        if (left.columns[static_cast<std::size_t>(i)].type() !=
                dataframe::TypeId::String ||
            right.columns[static_cast<std::size_t>(i)].type() !=
                dataframe::TypeId::String)
            throw DFTUtilsException::cat(
                ErrorCode::INVALID_ARGUMENT,
                "join/compare key columns must be string-typed");

    const bool anti = type == JoinType::LEFT_ANTI;
    const bool left_only = type == JoinType::LEFT_SEMI || anti;
    const bool want_left_only =
        type == JoinType::LEFT || type == JoinType::FULL || anti;
    const bool want_right_only =
        type == JoinType::RIGHT || type == JoinType::FULL;

    std::unordered_map<std::vector<std::string>, std::int64_t, KeyHash> ridx;
    ridx.reserve(static_cast<std::size_t>(right.num_rows()));
    for (std::int64_t r = 0; r < right.num_rows(); ++r)
        ridx.emplace(key_of(right, n_key, r), r);
    std::unordered_set<std::vector<std::string>, KeyHash> matched;

    struct Row {
        std::vector<std::string> key;
        std::int64_t li;
        std::int64_t ri;
    };
    std::vector<Row> rows;
    for (std::int64_t l = 0; l < left.num_rows(); ++l) {
        auto key = key_of(left, n_key, l);
        auto it = ridx.find(key);
        if (it != ridx.end()) {
            if (anti) continue;
            if (want_right_only) matched.insert(key);
            rows.push_back({std::move(key), l, left_only ? -1 : it->second});
        } else if (want_left_only) {
            rows.push_back({std::move(key), l, -1});
        }
    }
    if (want_right_only)
        for (std::int64_t r = 0; r < right.num_rows(); ++r) {
            auto key = key_of(right, n_key, r);
            if (!matched.count(key)) rows.push_back({std::move(key), -1, r});
        }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.key < b.key; });

    std::vector<std::int64_t> li, ri;
    li.reserve(rows.size());
    ri.reserve(rows.size());
    std::vector<std::vector<std::string>> okeys(
        static_cast<std::size_t>(n_key));
    for (const Row& row : rows) {
        li.push_back(row.li);
        ri.push_back(row.ri);
        for (std::int64_t k = 0; k < n_key; ++k)
            okeys[static_cast<std::size_t>(k)].push_back(
                row.key[static_cast<std::size_t>(k)]);
    }

    dataframe::DataFrame out;
    for (std::int64_t k = 0; k < n_key; ++k) {
        out.names.push_back(left.names[static_cast<std::size_t>(k)]);
        out.columns.push_back(
            dataframe::Series::strings(okeys[static_cast<std::size_t>(k)]));
    }
    for (std::size_t j = static_cast<std::size_t>(n_key);
         j < left.columns.size(); ++j) {
        out.names.push_back("l_" + left.names[j]);
        out.columns.push_back(dataframe::take(left.columns[j], li));
    }
    if (!left_only)
        for (std::size_t j = static_cast<std::size_t>(n_key);
             j < right.columns.size(); ++j) {
            out.names.push_back("r_" + right.names[j]);
            out.columns.push_back(dataframe::take(right.columns[j], ri));
        }
    return out;
}

coro::CoroTask<dataframe::DataFrame> AggregatedView::join(
    const AggregatedView& other, JoinType how) const {
    dataframe::DataFrame left = co_await collect().collect();
    dataframe::DataFrame right = co_await other.collect().collect();
    const std::int64_t n_key =
        (plan_->time_bucket_us > 0 ? 1 : 0) +
        static_cast<std::int64_t>(plan_->group_by.size());
    co_return join_batches(left, right, n_key, how);
}

}  // namespace dftracer::utils::trace::views
