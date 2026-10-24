#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/index/schemas/dft/agg/quantile_store.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace dftracer::utils::index::schemas::dft::agg {

namespace {

constexpr std::uint8_t TAG_EXACT = 0;
constexpr std::uint8_t TAG_SKETCH = 1;

// Keys and values of one block stay on the stack: 4 KiB of keys.
constexpr std::size_t BLOCK = 1024;

bool has_nan(const double* v, std::size_t n) {
    bool any = false;
    for (std::size_t i = 0; i < n; ++i) any |= v[i] != v[i];
    return any;
}

// `v` holds no NaN. One SIMD key pass, one sizing of the bins for the block's
// key range, then the scalar scatter.
void feed_block(dataframe::MergeSketch& sk, const double* v, std::size_t n) {
    if (n == 0) return;
    std::int32_t keys[BLOCK];
    dataframe::sketch_bucket_keys(v, static_cast<std::int64_t>(n),
                                  sk.log_gamma(), keys);
    std::int32_t lo = std::numeric_limits<std::int32_t>::max();
    std::int32_t hi = dataframe::SKETCH_ZERO_KEY;
    for (std::size_t i = 0; i < n; ++i) {
        if (keys[i] == dataframe::SKETCH_ZERO_KEY) continue;
        lo = std::min(lo, keys[i]);
        hi = std::max(hi, keys[i]);
    }
    if (lo <= hi) sk.reserve_keys(lo, hi);
    for (std::size_t i = 0; i < n; ++i) sk.add_key(keys[i], v[i]);
}

void feed(dataframe::MergeSketch& sk, const double* v, std::size_t n) {
    double clean[BLOCK];
    for (std::size_t at = 0; at < n; at += BLOCK) {
        const std::size_t m = std::min(BLOCK, n - at);
        if (!has_nan(v + at, m)) {
            feed_block(sk, v + at, m);
            continue;
        }
        std::size_t k = 0;
        for (std::size_t i = 0; i < m; ++i)
            if (v[at + i] == v[at + i]) clean[k++] = v[at + i];
        feed_block(sk, clean, k);
    }
}

}  // namespace

QuantileStore::QuantileStore(double relative_accuracy)
    : accuracy_(relative_accuracy) {
    new (&exact_) dstd::Vector<double>();
}

QuantileStore::QuantileStore(QuantileStore&& other) noexcept
    : accuracy_(other.accuracy_), sketch_mode_(other.sketch_mode_) {
    if (sketch_mode_)
        new (&sketch_) SketchPtr(std::move(other.sketch_));
    else
        new (&exact_) dstd::Vector<double>(std::move(other.exact_));
}

QuantileStore& QuantileStore::operator=(QuantileStore&& other) noexcept {
    if (this == &other) return *this;
    destroy();
    accuracy_ = other.accuracy_;
    sketch_mode_ = other.sketch_mode_;
    if (sketch_mode_)
        new (&sketch_) SketchPtr(std::move(other.sketch_));
    else
        new (&exact_) dstd::Vector<double>(std::move(other.exact_));
    return *this;
}

QuantileStore::~QuantileStore() { destroy(); }

void QuantileStore::destroy() noexcept {
    if (sketch_mode_)
        sketch_.~SketchPtr();
    else
        exact_.~ExactVec();
}

void QuantileStore::to_sketch(const double* extra, std::size_t n) {
    if (sketch_mode_) return;
    auto sketch = dstd::make_unique<dataframe::MergeSketch>(accuracy_);
    // The exact values and the head of `extra` share one block, so the bins
    // are sized once; this copy is bounded by BLOCK doubles.
    double head[BLOCK];
    std::size_t m = exact_.size();
    std::copy(exact_.begin(), exact_.end(), head);
    std::size_t used = 0;
    while (used < n && m < BLOCK) {
        if (extra[used] == extra[used]) head[m++] = extra[used];
        ++used;
    }
    feed_block(*sketch, head, m);
    feed(*sketch, extra + used, n - used);
    exact_.~ExactVec();
    new (&sketch_) SketchPtr(std::move(sketch));
    sketch_mode_ = true;
}

void QuantileStore::add_to(dataframe::DDSketch& out) const {
    if (!sketch_mode_) {
        for (double v : exact_) out.add(v);
        return;
    }
    for (const dataframe::HistogramBin& bin : sketch_->bins()) {
        const double at =
            bin.lower == 0.0 ? 0.0 : std::sqrt(bin.lower * bin.upper);
        out.add(at, static_cast<double>(bin.count));
    }
}

void QuantileStore::add(double value) {
    if (std::isnan(value)) return;
    if (!sketch_mode_) {
        if (exact_.size() < EXACT_CAPACITY) {
            exact_.insert(std::upper_bound(exact_.begin(), exact_.end(), value),
                          value);
            return;
        }
        to_sketch(&value, 1);
        return;
    }
    sketch_->add(value);
}

void QuantileStore::add_many(dstd::Span<const double> values) {
    if (!sketch_mode_) {
        std::size_t clean = 0;
        for (double v : values) clean += v == v;
        if (exact_.size() + clean > EXACT_CAPACITY) {
            to_sketch(values.data(), values.size());
            return;
        }
        const std::size_t mid = exact_.size();
        for (double v : values)
            if (v == v) exact_.push_back(v);
        const auto at = exact_.begin() + static_cast<std::ptrdiff_t>(mid);
        std::sort(at, exact_.end());
        std::inplace_merge(exact_.begin(), at, exact_.end());
        return;
    }
    feed(*sketch_, values.data(), values.size());
}

void QuantileStore::merge(const QuantileStore& other) {
    if (other.count() == 0) return;
    if (!sketch_mode_ && !other.sketch_mode_) {
        if (exact_.size() + other.exact_.size() <= EXACT_CAPACITY) {
            const std::size_t mid = exact_.size();
            exact_.insert(exact_.end(), other.exact_.begin(),
                          other.exact_.end());
            std::inplace_merge(
                exact_.begin(),
                exact_.begin() + static_cast<std::ptrdiff_t>(mid),
                exact_.end());
        } else {
            to_sketch(other.exact_.data(), other.exact_.size());
        }
        return;
    }
    if (other.sketch_mode_) {
        to_sketch(nullptr, 0);
        sketch_->merge(*other.sketch_);
    } else {
        feed(*sketch_, other.exact_.data(), other.exact_.size());
    }
}

double QuantileStore::quantile(double q) const {
    if (sketch_mode_) return sketch_->quantile(q);
    const std::size_t n = exact_.size();
    if (n == 0) return std::numeric_limits<double>::quiet_NaN();
    if (q <= 0.0) return exact_.front();
    if (q >= 1.0) return exact_.back();
    const double pos = q * static_cast<double>(n - 1);
    const auto lo = static_cast<std::size_t>(pos);
    const double frac = pos - static_cast<double>(lo);
    if (lo + 1 >= n) return exact_[lo];
    return exact_[lo] * (1.0 - frac) + exact_[lo + 1] * frac;
}

std::uint64_t QuantileStore::count() const {
    return sketch_mode_ ? sketch_->count() : exact_.size();
}

void QuantileStore::serialize_into(dstd::Vector<std::uint8_t>& buf) const {
    buf.push_back(sketch_mode_ ? TAG_SKETCH : TAG_EXACT);
    append_f64_le(buf, accuracy_);
    if (sketch_mode_) {
        const std::size_t n = sketch_->serialized_size();
        append_u32_le(buf, static_cast<std::uint32_t>(n));
        const std::size_t at = buf.size();
        buf.resize(at + n);
        sketch_->write_to(buf.data() + at);
        return;
    }
    append_u32_le(buf, static_cast<std::uint32_t>(exact_.size()));
    for (double v : exact_) append_f64_le(buf, v);
}

expected<QuantileStore, ErrorCode> QuantileStore::deserialize(
    const std::uint8_t* data, std::size_t len) {
    LeReader r(data, len);
    std::uint8_t tag = 0;
    double accuracy = 0.0;
    std::uint32_t n = 0;
    if (!r.u8(tag) || !r.f64(accuracy) || !r.u32(n)) {
        return unexpected(ErrorCode::Parse);
    }
    const std::size_t rest = r.remaining();
    if (!(accuracy > 0.0 && accuracy < 1.0)) {
        return unexpected(ErrorCode::Parse);
    }

    QuantileStore s(accuracy);
    if (tag == TAG_EXACT) {
        if (n > EXACT_CAPACITY || rest != std::size_t{n} * 8) {
            return unexpected(ErrorCode::Parse);
        }
        s.exact_.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            double v = 0.0;
            r.f64(v);
            if (std::isnan(v) || (!s.exact_.empty() && v < s.exact_.back())) {
                return unexpected(ErrorCode::Parse);
            }
            s.exact_.push_back(v);
        }
        return s;
    }
    if (tag == TAG_SKETCH) {
        if (rest != n) return unexpected(ErrorCode::Parse);
        s.to_sketch(nullptr, 0);
        *s.sketch_ = dataframe::MergeSketch::deserialize(r.position(), n);
        return s;
    }
    return unexpected(ErrorCode::Parse);
}

}  // namespace dftracer::utils::index::schemas::dft::agg
