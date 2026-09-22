#include <dftracer/utils/core/common/hash/hash_combine.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/internal/cell_ops.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/varwidth_offsets.h>
#include <dftracer/utils/dataframe/join.h>
#include <dftracer/utils/dataframe/kernels/filter.h>
#include <dftracer/utils/dataframe/parallel.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace dftracer::utils::dataframe {

namespace {

constexpr const char* DEFAULT_SUFFIX = "_right";

bool keeps_unmatched_left(JoinHow how) {
    return how == JoinHow::Left || how == JoinHow::Outer;
}

bool keeps_unmatched_right(JoinHow how) {
    return how == JoinHow::Right || how == JoinHow::Outer;
}

bool left_only(JoinHow how) {
    return how == JoinHow::Semi || how == JoinHow::Anti;
}

Series flat(const Series& c) {
    return c.encoding() == Encoding::Flat ? c.share() : materialize(c);
}

std::int64_t index_of(const std::vector<std::string>& names,
                      const std::string& name) {
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return static_cast<std::int64_t>(i);
    return -1;
}

// A key column as the probe reads it: the buffers resolved once, so the
// per-row hash, null test and compare are pointer arithmetic, not calls
// across the C ABI.
struct KeyView {
    enum Kind { Fixed, Bool, Str32, Str64 } kind = Fixed;
    const std::uint8_t* data = nullptr;
    const std::uint8_t* validity = nullptr;
    const std::int32_t* off32 = nullptr;
    const std::int64_t* off64 = nullptr;
    std::size_t width = 0;

    explicit KeyView(const Series& c) {
        const dftu_series& h = *c.handle();
        if (!is_orderable_type(h.type))
            throw std::invalid_argument(std::string("join: key column type '") +
                                        type_name(h.type) +
                                        "' has no per-row value to key on");
        data = h.data ? h.data->data() : nullptr;
        validity = h.validity ? h.validity->data() : nullptr;
        const TypeId narrow = narrow_varwidth_type(h.type);
        if (narrow == TypeId::String || narrow == TypeId::Binary) {
            if (h.offsets64) {
                kind = Str64;
                off64 =
                    reinterpret_cast<const std::int64_t*>(h.offsets64->data());
            } else {
                kind = Str32;
                off32 =
                    reinterpret_cast<const std::int32_t*>(h.offsets->data());
            }
        } else if (h.type == TypeId::Bool) {
            kind = Bool;
        } else {
            kind = Fixed;
            width = byte_width(h.type, h.fixed_size).value_or(0);
        }
    }
    bool is_null(std::int64_t i) const {
        return validity && !((validity[i >> 3] >> (i & 7)) & 1);
    }
    std::string_view bytes(std::int64_t i) const {
        if (kind == Str64)
            return std::string_view(
                reinterpret_cast<const char*>(data) + off64[i],
                static_cast<std::size_t>(off64[i + 1] - off64[i]));
        return std::string_view(
            reinterpret_cast<const char*>(data) + off32[i],
            static_cast<std::size_t>(off32[i + 1] - off32[i]));
    }
    // The value's bits for a fixed-width cell (a single integer key hashes
    // to itself), the bytes' hash otherwise.
    std::uint64_t hash(std::int64_t i) const {
        switch (kind) {
            case Fixed: {
                std::uint64_t bits = 0;
                const std::uint8_t* p =
                    data + static_cast<std::size_t>(i) * width;
                if (width == 8) {
                    std::memcpy(&bits, p, 8);
                    return bits;
                }
                std::memcpy(&bits, p,
                            width < sizeof(bits) ? width : sizeof(bits));
                if (width > sizeof(bits))
                    bits ^= std::hash<std::string_view>{}(std::string_view(
                        reinterpret_cast<const char*>(p), width));
                return bits;
            }
            case Bool:
                return (data[i >> 3] >> (i & 7)) & 1;
            case Str32:
            case Str64:
                return std::hash<std::string_view>{}(bytes(i));
        }
        return 0;
    }
    bool equals(std::int64_t i, const KeyView& o, std::int64_t j) const {
        switch (kind) {
            case Fixed: {
                const std::uint8_t* a =
                    data + static_cast<std::size_t>(i) * width;
                const std::uint8_t* b =
                    o.data + static_cast<std::size_t>(j) * width;
                // The common widths compare as one load each; memcmp is a
                // call per row, which the probe loop cannot afford.
                if (width == 8) {
                    std::uint64_t x, y;
                    std::memcpy(&x, a, 8);
                    std::memcpy(&y, b, 8);
                    return x == y;
                }
                if (width == 4) {
                    std::uint32_t x, y;
                    std::memcpy(&x, a, 4);
                    std::memcpy(&y, b, 4);
                    return x == y;
                }
                return std::memcmp(a, b, width) == 0;
            }
            case Bool:
                return ((data[i >> 3] >> (i & 7)) & 1) ==
                       ((o.data[j >> 3] >> (j & 7)) & 1);
            case Str32:
            case Str64:
                return bytes(i) == o.bytes(j);
        }
        return false;
    }
};

std::vector<KeyView> key_views(const std::vector<Series>& keys) {
    std::vector<KeyView> out;
    out.reserve(keys.size());
    for (const Series& k : keys) out.emplace_back(k);
    return out;
}

bool any_null(const std::vector<KeyView>& keys, std::int64_t i) {
    for (const KeyView& k : keys)
        if (k.is_null(i)) return true;
    return false;
}

std::uint64_t row_hash(const std::vector<KeyView>& keys, std::int64_t i) {
    if (keys.size() == 1) return keys[0].hash(i);
    std::size_t h = 0;
    for (const KeyView& k : keys)
        dftracer::utils::hash_combine(h, static_cast<std::size_t>(k.hash(i)));
    return h;
}

bool rows_equal(const std::vector<KeyView>& a, std::int64_t i,
                const std::vector<KeyView>& b, std::int64_t j) {
    for (std::size_t k = 0; k < a.size(); ++k)
        if (!a[k].equals(i, b[k], j)) return false;
    return true;
}

// The key hash per row, built in parallel; a row with a null key is
// reported through `null_row` and gets no hash.
std::vector<std::uint64_t> build_hashes(const std::vector<KeyView>& keys,
                                        std::int64_t n,
                                        std::vector<std::uint8_t>& null_row) {
    std::vector<std::uint64_t> out(static_cast<std::size_t>(n));
    null_row.assign(static_cast<std::size_t>(n), 0);
    parallel_for(n, std::int64_t{1} << 15, [&](std::int64_t b, std::int64_t e) {
        for (std::int64_t i = b; i < e; ++i) {
            if (any_null(keys, i)) {
                null_row[static_cast<std::size_t>(i)] = 1;
                continue;
            }
            out[static_cast<std::size_t>(i)] = row_hash(keys, i);
        }
    });
    return out;
}

}  // namespace

bool valid_join_how(JoinHow how) noexcept {
    switch (how) {
        case JoinHow::Inner:
        case JoinHow::Left:
        case JoinHow::Right:
        case JoinHow::Outer:
        case JoinHow::Semi:
        case JoinHow::Anti:
        case JoinHow::Cross:
            return true;
    }
    return false;
}

HashJoin::HashJoin(DataFrame right, std::vector<std::string> left_on,
                   std::vector<std::string> right_on, JoinHow how,
                   std::string suffix)
    : right_(std::move(right)),
      left_on_(std::move(left_on)),
      right_on_(std::move(right_on)),
      how_(how),
      suffix_(suffix.empty() ? DEFAULT_SUFFIX : std::move(suffix)) {
    if (!valid_join_how(how_))
        throw std::invalid_argument("join: unknown join kind " +
                                    std::to_string(static_cast<int>(how_)));
    if (how_ == JoinHow::Cross) {
        left_on_.clear();
        right_on_.clear();
    } else {
        if (left_on_.empty())
            throw std::invalid_argument("join: no key columns");
        if (left_on_.size() != right_on_.size())
            throw std::invalid_argument(
                "join: left_on and right_on differ in length");
    }
    right_keys_.reserve(right_on_.size());
    for (const std::string& k : right_on_) {
        const std::int64_t ri = index_of(right_.names, k);
        if (ri < 0) throw std::out_of_range("join: no right column named " + k);
        right_keys_.push_back(
            flat(right_.columns[static_cast<std::size_t>(ri)]));
    }

    const std::int64_t n = right_.num_rows();
    if (keeps_unmatched_right(how_)) {
        right_matched_n_ = static_cast<std::size_t>(n);
        right_matched_ =
            std::make_unique<std::atomic<std::uint8_t>[]>(right_matched_n_);
    }
    if (how_ == JoinHow::Cross) return;

    std::vector<std::uint8_t> null_row;
    const std::vector<KeyView> views = key_views(right_keys_);
    std::vector<std::uint64_t> hashes = build_hashes(views, n, null_row);
    next_.assign(static_cast<std::size_t>(n), -1);

    // A single integer key whose values span a range not much wider than
    // the build side is looked up by direct address: the key's bits index
    // an array, no hashing on either side. Signed negatives read as huge
    // unsigned values and widen the span past the limit, which is the
    // fall-back to the hash table.
    if (views.size() == 1 && views[0].kind == KeyView::Fixed &&
        views[0].width == 8 &&
        (right_keys_[0].type() == TypeId::Int64 ||
         right_keys_[0].type() == TypeId::Uint64)) {
        std::uint64_t lo = ~std::uint64_t{0}, hi = 0;
        bool any = false;
        for (std::int64_t i = 0; i < n; ++i) {
            if (null_row[static_cast<std::size_t>(i)]) continue;
            const std::uint64_t v = hashes[static_cast<std::size_t>(i)];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            any = true;
        }
        const std::uint64_t span = any ? hi - lo + 1 : 0;
        if (any && span <= static_cast<std::uint64_t>(n) * 4 + 1024) {
            direct_base_ = lo;
            direct_.assign(static_cast<std::size_t>(span), -1);
            for (std::int64_t i = n - 1; i >= 0; --i) {
                if (null_row[static_cast<std::size_t>(i)]) continue;
                std::int64_t& slot = direct_[static_cast<std::size_t>(
                    hashes[static_cast<std::size_t>(i)] - lo)];
                if (slot >= 0) next_[static_cast<std::size_t>(i)] = slot;
                slot = i;
            }
            return;
        }
    }

    first_.reserve(static_cast<std::size_t>(n));
    // Insert back to front so each chain lists its rows in ascending order.
    for (std::int64_t i = n - 1; i >= 0; --i) {
        if (null_row[static_cast<std::size_t>(i)]) continue;
        auto [it, inserted] =
            first_.try_emplace(hashes[static_cast<std::size_t>(i)], i);
        if (!inserted) {
            next_[static_cast<std::size_t>(i)] = it->second;
            it->second = i;
        }
    }
}

std::vector<std::int64_t> HashJoin::left_key_indices(
    const std::vector<std::string>& left_names) const {
    std::vector<std::int64_t> idx;
    idx.reserve(left_on_.size());
    for (const std::string& k : left_on_) {
        const std::int64_t li = index_of(left_names, k);
        if (li < 0) throw std::out_of_range("join: no left column named " + k);
        idx.push_back(li);
    }
    return idx;
}

std::int64_t HashJoin::shared_key_of(const std::vector<std::string>& left_names,
                                     std::size_t li) const {
    for (std::size_t p = 0; p < left_on_.size(); ++p)
        if (left_on_[p] == left_names[li] && right_on_[p] == left_names[li])
            return static_cast<std::int64_t>(p);
    return -1;
}

std::vector<std::string> HashJoin::out_names(
    const std::vector<std::string>& left_names) const {
    return join_out_names(left_names, right_.names, left_on_, right_on_, how_,
                          suffix_);
}

JoinRightLayout join_right_layout(const std::vector<std::string>& left_names,
                                  const std::vector<std::string>& right_names,
                                  const std::vector<std::string>& left_on,
                                  const std::vector<std::string>& right_on,
                                  JoinHow how, const std::string& suffix) {
    if (left_on.size() != right_on.size())
        throw std::invalid_argument(
            "join: left_on and right_on differ in length");
    JoinRightLayout out;
    if (left_only(how)) return out;
    const std::string& sfx = suffix.empty() ? DEFAULT_SUFFIX : suffix;
    for (std::size_t ri = 0; ri < right_names.size(); ++ri) {
        const std::string& name = right_names[ri];
        bool shared_key = false;
        for (std::size_t p = 0; p < right_on.size(); ++p)
            if (right_on[p] == name && left_on[p] == name) shared_key = true;
        if (shared_key) continue;
        out.keep.push_back(ri);
        out.names.push_back(index_of(left_names, name) >= 0 ? name + sfx
                                                            : name);
    }
    return out;
}

std::vector<std::string> join_out_names(
    const std::vector<std::string>& left_names,
    const std::vector<std::string>& right_names,
    const std::vector<std::string>& left_on,
    const std::vector<std::string>& right_on, JoinHow how,
    const std::string& suffix) {
    for (const std::string& k : left_on)
        if (index_of(left_names, k) < 0)
            throw std::out_of_range("join: no left column named " + k);
    for (const std::string& k : right_on)
        if (index_of(right_names, k) < 0)
            throw std::out_of_range("join: no right column named " + k);
    std::vector<std::string> out = left_names;
    JoinRightLayout layout = join_right_layout(left_names, right_names, left_on,
                                               right_on, how, suffix);
    out.insert(out.end(), layout.names.begin(), layout.names.end());
    return out;
}

std::vector<Field> join_out_fields(const std::vector<Field>& left,
                                   const std::vector<Field>& right,
                                   const std::vector<std::string>& left_on,
                                   const std::vector<std::string>& right_on,
                                   JoinHow how, const std::string& suffix) {
    std::vector<std::string> left_names;
    left_names.reserve(left.size());
    for (const Field& f : left) left_names.push_back(f.name);
    std::vector<std::string> right_names;
    right_names.reserve(right.size());
    for (const Field& f : right) right_names.push_back(f.name);
    (void)join_out_names(left_names, right_names, left_on, right_on, how,
                         suffix);
    std::vector<Field> out;
    out.reserve(left.size() + right.size());
    for (const Field& f : left) out.push_back(Field{f.name, f.type, true});
    JoinRightLayout layout = join_right_layout(left_names, right_names, left_on,
                                               right_on, how, suffix);
    for (std::size_t i = 0; i < layout.keep.size(); ++i)
        out.push_back(Field{layout.names[i], right[layout.keep[i]].type, true});
    return out;
}

DataFrame HashJoin::probe(const DataFrame& left) {
    const std::vector<std::int64_t> key_idx = left_key_indices(left.names);
    std::vector<Series> left_keys;
    left_keys.reserve(key_idx.size());
    for (std::size_t p = 0; p < key_idx.size(); ++p) {
        const Series& lc = left.columns[static_cast<std::size_t>(key_idx[p])];
        const Series& rc = right_keys_[p];
        if (lc.type() != rc.type())
            throw std::invalid_argument(
                "join: key '" + left_on_[p] + "' is " +
                std::string(type_name(lc.type())) + " on the left but '" +
                right_on_[p] + "' is " + std::string(type_name(rc.type())) +
                " on the right");
        left_keys.push_back(flat(lc));
    }

    const std::int64_t n = left.num_rows();
    const std::int64_t nright = right_.num_rows();

    // The pair lists a probe writes, then the gathers over them. The index
    // width is the caller's: 32-bit when both sides fit, since the lists
    // and the gathers are memory-bound and polars' are 32-bit.
    auto probe_into = [&]<class Idx>(std::vector<Idx>& left_idx,
                                     std::vector<Idx>& right_idx) {
        const std::vector<KeyView> lviews = key_views(left_keys);
        const std::vector<KeyView> rviews = key_views(right_keys_);
        // Probe in parallel: each chunk of left rows writes its own pair
        // lists, stitched in chunk order so the output stays in left order.
        constexpr std::int64_t GRAIN = std::int64_t{1} << 15;
        const std::int64_t chunks = n == 0 ? 0 : (n + GRAIN - 1) / GRAIN;
        std::vector<std::vector<Idx>> lparts(static_cast<std::size_t>(chunks));
        std::vector<std::vector<Idx>> rparts(static_cast<std::size_t>(chunks));
        const bool track_right = right_matched_n_ != 0;
        // The row loop over three probes: is the key null, the first
        // candidate row, does candidate r equal row i. Each mode below
        // passes its own three, inlined, so the common single 8-byte key
        // reads one word per row and compares one word per candidate.
        // A chunk's pair lists grow by direct stores through a cursor;
        // push_back is a call per row that the loop cannot afford.
        struct Appender {
            std::vector<Idx>& v;
            std::size_t n = 0;
            void push(Idx x) {
                if (n == v.size()) v.resize(v.size() * 2 + 1024);
                v[n++] = x;
            }
        };
        auto run = [&](auto is_null, auto first_of, auto equal) {
            parallel_for(n, GRAIN, [&](std::int64_t b, std::int64_t e) {
                Appender lp{lparts[static_cast<std::size_t>(b / GRAIN)]};
                Appender rp{rparts[static_cast<std::size_t>(b / GRAIN)]};
                lp.v.resize(static_cast<std::size_t>(e - b));
                if (!left_only(how_))
                    rp.v.resize(static_cast<std::size_t>(e - b));
                for (std::int64_t i = b; i < e; ++i) {
                    std::int64_t r = -1;
                    if (!is_null(i)) {
                        r = first_of(i);
                        // The first chain row whose key really equals; the
                        // rest of the chain is walked below the same way.
                        while (r >= 0 && !equal(i, r))
                            r = next_[static_cast<std::size_t>(r)];
                    }
                    if (r < 0) {
                        if (how_ == JoinHow::Anti) lp.push(static_cast<Idx>(i));
                        if (keeps_unmatched_left(how_)) {
                            lp.push(static_cast<Idx>(i));
                            rp.push(Idx{-1});
                        }
                        continue;
                    }
                    if (how_ == JoinHow::Semi) {
                        lp.push(static_cast<Idx>(i));
                        continue;
                    }
                    if (how_ == JoinHow::Anti) continue;
                    for (; r >= 0; r = next_[static_cast<std::size_t>(r)]) {
                        if (!equal(i, r)) continue;
                        lp.push(static_cast<Idx>(i));
                        rp.push(static_cast<Idx>(r));
                        if (track_right)
                            right_matched_[static_cast<std::size_t>(r)].store(
                                1, std::memory_order_relaxed);
                    }
                }
                lp.v.resize(lp.n);
                rp.v.resize(rp.n);
            });
        };
        const bool one_word = lviews.size() == 1 &&
                              lviews[0].kind == KeyView::Fixed &&
                              lviews[0].width == 8;
        if (one_word && !direct_.empty()) {
            const auto* lk =
                reinterpret_cast<const std::uint64_t*>(lviews[0].data);
            const auto* rk =
                reinterpret_cast<const std::uint64_t*>(rviews[0].data);
            const std::uint8_t* lv = lviews[0].validity;
            const std::uint64_t base = direct_base_;
            const std::uint64_t span = direct_.size();
            const std::int64_t* direct = direct_.data();
            run(
                [&](std::int64_t i) {
                    return lv && !((lv[i >> 3] >> (i & 7)) & 1);
                },
                [&](std::int64_t i) {
                    const std::uint64_t at = lk[i] - base;
                    return at < span ? direct[at] : std::int64_t{-1};
                },
                [&](std::int64_t i, std::int64_t r) { return lk[i] == rk[r]; });
        } else if (one_word) {
            const auto* lk =
                reinterpret_cast<const std::uint64_t*>(lviews[0].data);
            const auto* rk =
                reinterpret_cast<const std::uint64_t*>(rviews[0].data);
            const std::uint8_t* lv = lviews[0].validity;
            run(
                [&](std::int64_t i) {
                    return lv && !((lv[i >> 3] >> (i & 7)) & 1);
                },
                [&](std::int64_t i) {
                    auto it = first_.find(lk[i]);
                    return it != first_.end() ? it->second : std::int64_t{-1};
                },
                [&](std::int64_t i, std::int64_t r) { return lk[i] == rk[r]; });
        } else {
            run([&](std::int64_t i) { return any_null(lviews, i); },
                [&](std::int64_t i) {
                    const std::uint64_t h = row_hash(lviews, i);
                    if (!direct_.empty()) {
                        const std::uint64_t at = h - direct_base_;
                        return at < direct_.size()
                                   ? direct_[static_cast<std::size_t>(at)]
                                   : std::int64_t{-1};
                    }
                    auto it = first_.find(h);
                    return it != first_.end() ? it->second : std::int64_t{-1};
                },
                [&](std::int64_t i, std::int64_t r) {
                    return rows_equal(lviews, i, rviews, r);
                });
        }
        std::vector<std::size_t> at(lparts.size() + 1, 0);
        for (std::size_t c = 0; c < lparts.size(); ++c)
            at[c + 1] = at[c] + lparts[c].size();
        left_idx.resize(at.back());
        if (!left_only(how_)) right_idx.resize(at.back());
        parallel_for(
            static_cast<std::int64_t>(lparts.size()), 1,
            [&](std::int64_t b, std::int64_t e) {
                for (std::int64_t c = b; c < e; ++c) {
                    const auto ci = static_cast<std::size_t>(c);
                    std::copy(
                        lparts[ci].begin(), lparts[ci].end(),
                        left_idx.begin() + static_cast<std::ptrdiff_t>(at[ci]));
                    if (!rparts[ci].empty())
                        std::copy(rparts[ci].begin(), rparts[ci].end(),
                                  right_idx.begin() +
                                      static_cast<std::ptrdiff_t>(at[ci]));
                }
            });
    };
    auto emit = [&]<class Idx>(const std::vector<Idx>& left_idx,
                               const std::vector<Idx>& right_idx) {
        auto gather = [](const Series& c, const std::vector<Idx>& idx) {
            if constexpr (std::is_same_v<Idx, std::int32_t>)
                return take32(c, idx);
            else
                return take(c, idx);
        };
        DataFrame out;
        out.names = left.names;
        out.columns.reserve(left.columns.size() + right_.columns.size());
        for (const Series& c : left.columns)
            out.columns.push_back(gather(c, left_idx));
        JoinRightLayout layout = join_right_layout(
            left.names, right_.names, left_on_, right_on_, how_, suffix_);
        for (std::size_t i = 0; i < layout.keep.size(); ++i) {
            out.names.push_back(layout.names[i]);
            out.columns.push_back(
                gather(right_.columns[layout.keep[i]], right_idx));
        }
        return out;
    };

    if (how_ == JoinHow::Cross) {
        std::vector<std::int64_t> left_idx;
        std::vector<std::int64_t> right_idx;
        left_idx.reserve(static_cast<std::size_t>(n * nright));
        right_idx.reserve(static_cast<std::size_t>(n * nright));
        for (std::int64_t i = 0; i < n; ++i)
            for (std::int64_t r = 0; r < nright; ++r) {
                left_idx.push_back(i);
                right_idx.push_back(r);
            }
        return emit(left_idx, right_idx);
    }
    constexpr std::int64_t FITS32 = std::numeric_limits<std::int32_t>::max();
    if (n < FITS32 && nright < FITS32) {
        std::vector<std::int32_t> left_idx;
        std::vector<std::int32_t> right_idx;
        probe_into(left_idx, right_idx);
        return emit(left_idx, right_idx);
    }
    std::vector<std::int64_t> left_idx;
    std::vector<std::int64_t> right_idx;
    probe_into(left_idx, right_idx);
    return emit(left_idx, right_idx);
}

DataFrame HashJoin::flush(const std::vector<std::string>& left_names,
                          const std::vector<Series>& left_templates) {
    DataFrame out;
    if (flushed_ || !keeps_unmatched_right(how_)) return out;
    flushed_ = true;
    if (left_templates.size() != left_names.size())
        throw std::invalid_argument(
            "join: left templates do not match the left column count");
    std::vector<std::int64_t> right_idx;
    for (std::size_t r = 0; r < right_matched_n_; ++r)
        if (!right_matched_[r].load(std::memory_order_relaxed))
            right_idx.push_back(static_cast<std::int64_t>(r));
    if (right_idx.empty()) return out;
    const std::vector<std::int64_t> nulls(right_idx.size(), -1);

    out.names = left_names;
    for (std::size_t li = 0; li < left_names.size(); ++li) {
        const std::int64_t p = shared_key_of(left_names, li);
        out.columns.push_back(
            p >= 0 ? take(right_keys_[static_cast<std::size_t>(p)], right_idx)
                   : take(left_templates[li], nulls));
    }
    JoinRightLayout layout = join_right_layout(
        left_names, right_.names, left_on_, right_on_, how_, suffix_);
    for (std::size_t i = 0; i < layout.keep.size(); ++i) {
        out.names.push_back(layout.names[i]);
        out.columns.push_back(take(right_.columns[layout.keep[i]], right_idx));
    }
    return out;
}

DataFrame join(const DataFrame& left, const DataFrame& right,
               const std::vector<std::string>& left_on,
               const std::vector<std::string>& right_on, JoinHow how,
               const std::string& suffix) {
    HashJoin hj(DataFrame{right.names,
                          [&] {
                              std::vector<Series> cols;
                              cols.reserve(right.columns.size());
                              for (const Series& c : right.columns)
                                  cols.push_back(c.share());
                              return cols;
                          }()},
                left_on, right_on, how, suffix);
    DataFrame matched = hj.probe(left);
    DataFrame rest = hj.flush(left.names, left.columns);
    if (rest.num_rows() == 0) return matched;
    return concat({&matched, &rest});
}

namespace {

DataFrame prefix_metrics(const DataFrame& b, std::size_t n_key,
                         const char* prefix) {
    DataFrame out;
    out.names.reserve(b.names.size());
    out.columns.reserve(b.columns.size());
    for (std::size_t i = 0; i < b.names.size(); ++i) {
        out.names.push_back(i < n_key ? b.names[i] : prefix + b.names[i]);
        out.columns.push_back(b.columns[i].share());
    }
    return out;
}

}  // namespace

DataFrame compare_agg(const DataFrame& base, const DataFrame& variant,
                      std::int64_t n_key) {
    if (n_key < 1) throw std::invalid_argument("compare_agg: n_key < 1");
    const auto nk = static_cast<std::size_t>(n_key);
    if (nk > base.names.size() || nk > variant.names.size())
        throw std::invalid_argument(
            "compare_agg: n_key exceeds a frame's column count");
    std::vector<std::string> keys;
    for (std::size_t i = 0; i < nk; ++i) {
        if (base.names[i] != variant.names[i])
            throw std::invalid_argument(
                "compare_agg: key column " + std::to_string(i) + " is '" +
                base.names[i] + "' vs '" + variant.names[i] + "'");
        keys.push_back(base.names[i]);
    }

    DataFrame joined = sort_by_multi(
        join(prefix_metrics(base, nk, "l_"), prefix_metrics(variant, nk, "r_"),
             keys, keys, JoinHow::Outer, "_right"),
        keys, false);

    const std::int64_t n = joined.num_rows();
    std::vector<double> hundred(static_cast<std::size_t>(n), 100.0);
    Series scale = Series::flat_f64(hundred.data(), n);
    for (std::size_t i = nk; i < base.names.size(); ++i) {
        const std::string& m = base.names[i];
        if (joined.column_index("r_" + m) < 0) continue;
        Series l = joined.column("l_" + m).materialize();
        Series r = joined.column("r_" + m).materialize();
        if (!is_numeric_dispatchable(l.type()) ||
            !is_numeric_dispatchable(r.type()))
            continue;
        Series delta = r.sub(l);
        Series pct =
            delta.cast(TypeId::Float64).div(l.cast(TypeId::Float64)).mul(scale);
        joined = with_column(joined, "delta_" + m, delta);
        joined = with_column(joined, "pct_" + m, pct);
    }
    return joined;
}

}  // namespace dftracer::utils::dataframe
