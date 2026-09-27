#include <ankerl/unordered_dense.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/duql/overlap.h>
#include <dftracer/utils/duql/vectorize.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace dftracer::utils::duql {

namespace {

namespace df = dataframe;

bool is_int64_safe(df::TypeId t) {
    return t == df::TypeId::Int8 || t == df::TypeId::Int16 ||
           t == df::TypeId::Int32 || t == df::TypeId::Int64 ||
           t == df::TypeId::Uint8 || t == df::TypeId::Uint16 ||
           t == df::TypeId::Uint32;
}

bool is_number(df::TypeId t) {
    return is_int64_safe(t) || t == df::TypeId::Uint64 ||
           t == df::TypeId::Float32 || t == df::TypeId::Float64;
}

template <class T>
struct Numbers {
    std::vector<T> value;
    std::vector<std::uint8_t> ok;
};

template <class T, class U>
void copy_column(const df::Series& s, Numbers<T>& out) {
    const U* p = s.data<U>();
    for (std::int64_t i = 0; i < s.length(); ++i) {
        const auto at = static_cast<std::size_t>(i);
        out.ok[at] = !s.is_null(i);
        out.value[at] = static_cast<T>(p[at]);
    }
}

template <class T>
Numbers<T> read_numbers(const df::Series& s, const char* what) {
    if (!is_number(s.type()))
        throw std::invalid_argument(
            std::string("overlap: the ") + what + " is " +
            std::string(df::type_name(s.type())) + ", not a number");
    const auto n = static_cast<std::size_t>(s.length());
    Numbers<T> out{std::vector<T>(n), std::vector<std::uint8_t>(n)};
    switch (s.type()) {
        case df::TypeId::Int8:
            copy_column<T, std::int8_t>(s, out);
            break;
        case df::TypeId::Int16:
            copy_column<T, std::int16_t>(s, out);
            break;
        case df::TypeId::Int32:
            copy_column<T, std::int32_t>(s, out);
            break;
        case df::TypeId::Int64:
            copy_column<T, std::int64_t>(s, out);
            break;
        case df::TypeId::Uint8:
            copy_column<T, std::uint8_t>(s, out);
            break;
        case df::TypeId::Uint16:
            copy_column<T, std::uint16_t>(s, out);
            break;
        case df::TypeId::Uint32:
            copy_column<T, std::uint32_t>(s, out);
            break;
        case df::TypeId::Uint64:
            copy_column<T, std::uint64_t>(s, out);
            break;
        case df::TypeId::Float32:
            copy_column<T, float>(s, out);
            break;
        default:
            copy_column<T, double>(s, out);
            break;
    }
    return out;
}

template <class T>
T add_saturating(T a, T b) {
    if constexpr (std::is_floating_point_v<T>) {
        return a + b;
    } else {
        T r;
        if (!__builtin_add_overflow(a, b, &r)) return r;
        return b > 0 ? std::numeric_limits<T>::max()
                     : std::numeric_limits<T>::lowest();
    }
}

template <class T>
bool valid_interval(T start, T duration, bool ok) {
    if (!ok) return false;
    if constexpr (std::is_floating_point_v<T>)
        if (std::isnan(start) || std::isnan(duration)) return false;
    return duration >= 0;
}

// One frame's intervals as numbers; a row that cannot match has ok == 0.
template <class T>
struct Intervals {
    std::vector<T> start;
    std::vector<T> duration;
    std::vector<std::uint8_t> ok;
};

template <class T>
Intervals<T> read_intervals(const OverlapColumns& c) {
    Numbers<T> s = read_numbers<T>(c.start, "start");
    Numbers<T> d = read_numbers<T>(c.duration, "duration");
    if (s.value.size() != d.value.size())
        throw std::invalid_argument("overlap: start and duration differ");
    Intervals<T> out;
    out.ok.resize(s.value.size());
    for (std::size_t i = 0; i < s.value.size(); ++i) {
        if constexpr (std::is_floating_point_v<T>) d.value[i] *= c.scale;
        out.ok[i] =
            valid_interval<T>(s.value[i], d.value[i], s.ok[i] && d.ok[i]);
    }
    out.start = std::move(s.value);
    out.duration = std::move(d.value);
    return out;
}

bool row_key(std::string& out, const std::vector<df::Series>& keys,
             std::int64_t r) {
    out.clear();
    for (const df::Series& k : keys)
        if (!append_cell_key(out, k, r)) return false;
    return true;
}

// The side rows of one key, by start, with a max-end tree over them.
template <class T>
class Group {
   public:
    void add(T start, T end, std::int64_t row) {
        items_.push_back({start, end, row});
    }

    void seal() {
        std::sort(
            items_.begin(), items_.end(), [](const Item& a, const Item& b) {
                return a.start != b.start ? a.start < b.start : a.row < b.row;
            });
        starts_.reserve(items_.size());
        for (const Item& i : items_) starts_.push_back(i.start);
        size_ = 1;
        while (size_ < items_.size()) size_ <<= 1;
        tree_.assign(2 * size_, std::numeric_limits<T>::lowest());
        for (std::size_t i = 0; i < items_.size(); ++i)
            tree_[size_ + i] = items_[i].end;
        for (std::size_t i = size_; i-- > 1;)
            tree_[i] = std::max(tree_[2 * i], tree_[2 * i + 1]);
    }

    // The side rows overlapping `[a, a + d)`, unordered, appended to `out`.
    void matches(T a, T d, std::vector<std::int64_t>& out) const {
        const T stop = add_saturating(a, d);
        const auto first_after =
            std::upper_bound(starts_.begin(), starts_.end(), a);
        const auto prefix = static_cast<std::size_t>(
            (d > 0 ? first_after
                   : std::lower_bound(starts_.begin(), starts_.end(), a)) -
            starts_.begin());
        report(1, 0, size_, prefix, a, out);
        if (d > 0) {
            const auto lo =
                static_cast<std::size_t>(first_after - starts_.begin());
            const auto hi = static_cast<std::size_t>(
                std::lower_bound(starts_.begin(), starts_.end(), stop) -
                starts_.begin());
            for (std::size_t i = lo; i < hi; ++i) out.push_back(items_[i].row);
        }
    }

   private:
    struct Item {
        T start;
        T end;
        std::int64_t row;
    };

    // The items of `[lo, hi)` below `prefix` with an end after `a`.
    void report(std::size_t node, std::size_t lo, std::size_t hi,
                std::size_t prefix, T a, std::vector<std::int64_t>& out) const {
        if (lo >= prefix || !(tree_[node] > a)) return;
        if (hi - lo == 1) {
            out.push_back(items_[lo].row);
            return;
        }
        const std::size_t mid = lo + (hi - lo) / 2;
        report(2 * node, lo, mid, prefix, a, out);
        report(2 * node + 1, mid, hi, prefix, a, out);
    }

    std::vector<Item> items_;
    std::vector<T> starts_;
    std::vector<T> tree_;
    std::size_t size_ = 1;
};

template <class T>
OverlapMatches run(const OverlapColumns& rows, const OverlapColumns& side) {
    const Intervals<T> mine = read_intervals<T>(rows);
    const Intervals<T> theirs = read_intervals<T>(side);
    ankerl::unordered_dense::map<std::string, Group<T>> groups;
    std::string key;
    const auto m = static_cast<std::int64_t>(theirs.start.size());
    for (std::int64_t r = 0; r < m; ++r) {
        const auto at = static_cast<std::size_t>(r);
        if (!theirs.ok[at] || !row_key(key, side.keys, r)) continue;
        groups[key].add(theirs.start[at],
                        add_saturating(theirs.start[at], theirs.duration[at]),
                        r);
    }
    for (auto& [k, g] : groups) g.seal();

    OverlapMatches out;
    const auto n = static_cast<std::int64_t>(mine.start.size());
    out.offsets.reserve(static_cast<std::size_t>(n) + 1);
    out.offsets.push_back(0);
    std::vector<std::int64_t> found;
    for (std::int64_t i = 0; i < n; ++i) {
        const auto at = static_cast<std::size_t>(i);
        if (mine.ok[at] && row_key(key, rows.keys, i)) {
            if (const auto g = groups.find(key); g != groups.end()) {
                found.clear();
                g->second.matches(mine.start[at], mine.duration[at], found);
                std::sort(found.begin(), found.end());
                out.rows.insert(out.rows.end(), found.begin(), found.end());
            }
        }
        out.offsets.push_back(static_cast<std::int64_t>(out.rows.size()));
    }
    return out;
}

}  // namespace

OverlapMatches overlap_matches(const OverlapColumns& rows,
                               const OverlapColumns& side) {
    if (rows.keys.size() != side.keys.size())
        throw std::invalid_argument("overlap: the key columns differ in count");
    const bool exact = rows.scale == 1 && side.scale == 1 &&
                       is_int64_safe(rows.start.type()) &&
                       is_int64_safe(rows.duration.type()) &&
                       is_int64_safe(side.start.type()) &&
                       is_int64_safe(side.duration.type());
    return exact ? run<std::int64_t>(rows, side) : run<double>(rows, side);
}

}  // namespace dftracer::utils::duql
