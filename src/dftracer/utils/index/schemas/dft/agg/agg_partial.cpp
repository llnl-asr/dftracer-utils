// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/varint.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>

#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace dftracer::utils::index::schemas::dft::agg {

namespace {

using dataframe::FieldStat;
using dataframe::FieldStatDomain;

constexpr std::uint8_t TAG_LIST = 0;
constexpr std::uint8_t TAG_SPARSE = 1;
constexpr std::uint8_t TAG_DENSE = 2;

constexpr std::uint8_t HAS_TIMES = 1;
constexpr std::uint8_t HAS_DURATION = 2;
constexpr std::uint8_t HAS_SIZE = 4;
constexpr std::uint8_t HAS_OFFSET = 8;
constexpr std::uint8_t HAS_BINS = 16;
constexpr std::uint8_t KNOWN_FLAGS = 31;

constexpr std::uint8_t STAT_DOMAIN_MASK = 3;
constexpr std::uint8_t STAT_MOMENTS = 4;

// Every element takes at least one byte, so a count above the bytes left is
// malformed. Checking it first keeps a reserve small.
bool get_count(dstd::LeReader& r, std::uint64_t limit, std::uint64_t& n) {
    return dstd::read_varint(r, n) && n <= limit && n <= r.remaining();
}

std::uint64_t mix(std::uint64_t id) {
    return dftracer::utils::dstd::hash::splitmix64(id);
}

void put_stat(dstd::Vector<std::uint8_t>& buf, const FieldStat& s) {
    const bool moments = s.shift != 0.0 || s.cmean != 0.0 || s.cm2 != 0.0 ||
                         s.cm3 != 0.0 || s.cm4 != 0.0;
    buf.push_back(static_cast<std::uint8_t>(
        static_cast<std::uint8_t>(s.domain) | (moments ? STAT_MOMENTS : 0)));
    dstd::append_varint(buf, s.n);
    dstd::append_f64_le(buf, s.sum);
    if (s.domain == FieldStatDomain::F64) {
        dstd::append_f64_le(buf, s.min);
        dstd::append_f64_le(buf, s.max);
    } else {
        // min and max are the doubles of emin and emax, so they are not kept.
        dstd::append_varint(buf, std::bit_cast<std::uint64_t>(s.esum));
        dstd::append_varint(buf, std::bit_cast<std::uint64_t>(s.emin));
        dstd::append_varint(buf, std::bit_cast<std::uint64_t>(s.emax));
        dstd::append_varint(buf, std::bit_cast<std::uint64_t>(s.ecarry));
    }
    if (moments) {
        dstd::append_f64_le(buf, s.shift);
        dstd::append_f64_le(buf, s.cmean);
        dstd::append_f64_le(buf, s.cm2);
        dstd::append_f64_le(buf, s.cm3);
        dstd::append_f64_le(buf, s.cm4);
    }
}

bool get_stat(dstd::LeReader& r, FieldStat& s) {
    std::uint8_t b = 0;
    if (!r.u8(b) || (b & ~(STAT_DOMAIN_MASK | STAT_MOMENTS)) != 0) {
        return false;
    }
    const std::uint8_t domain = b & STAT_DOMAIN_MASK;
    if (domain > static_cast<std::uint8_t>(FieldStatDomain::F64)) return false;
    s = FieldStat{};
    s.domain = static_cast<FieldStatDomain>(domain);
    if (!dstd::read_varint(r, s.n) || !r.f64(s.sum)) return false;
    if (s.domain == FieldStatDomain::F64) {
        if (!r.f64(s.min) || !r.f64(s.max)) return false;
    } else {
        std::uint64_t es = 0, emin = 0, emax = 0, carry = 0;
        if (!dstd::read_varint(r, es) || !dstd::read_varint(r, emin) ||
            !dstd::read_varint(r, emax) || !dstd::read_varint(r, carry)) {
            return false;
        }
        s.esum = std::bit_cast<std::int64_t>(es);
        s.emin = std::bit_cast<std::int64_t>(emin);
        s.emax = std::bit_cast<std::int64_t>(emax);
        s.ecarry = std::bit_cast<std::int64_t>(carry);
        if (s.domain == FieldStatDomain::I64) {
            s.min = static_cast<double>(s.emin);
            s.max = static_cast<double>(s.emax);
        } else {
            s.min = static_cast<double>(emin);
            s.max = static_cast<double>(emax);
        }
    }
    if ((b & STAT_MOMENTS) != 0) {
        if (!r.f64(s.shift) || !r.f64(s.cmean) || !r.f64(s.cm2) ||
            !r.f64(s.cm3) || !r.f64(s.cm4)) {
            return false;
        }
    }
    return true;
}

void put_quantiles(dstd::Vector<std::uint8_t>& buf, const QuantileStore& q) {
    if (q.count() == 0) {
        buf.push_back(0);
        return;
    }
    buf.push_back(1);
    const std::size_t at = buf.size();
    dstd::append_u32_le(buf, 0);
    q.serialize_into(buf);
    dstd::write_u32_le(buf.data() + at,
                       static_cast<std::uint32_t>(buf.size() - at - 4));
}

bool get_quantiles(dstd::LeReader& r, QuantileStore& out, bool keep = true) {
    std::uint8_t present = 0;
    if (!r.u8(present) || present > 1) return false;
    if (present == 0) return true;
    std::uint32_t len = 0;
    if (!r.u32(len)) return false;
    const std::uint8_t* at = r.take(len);
    if (at == nullptr) return false;
    if (!keep) return true;
    auto q = QuantileStore::deserialize(at, len);
    if (!q) return false;
    out = std::move(*q);
    return true;
}

}  // namespace

DistinctIds::DistinctIds() { new (&list_) List(); }

DistinctIds::DistinctIds(DistinctIds&& other) noexcept
    : sketch_mode_(other.sketch_mode_) {
    if (sketch_mode_)
        new (&sketch_) SketchPtr(std::move(other.sketch_));
    else
        new (&list_) List(std::move(other.list_));
}

DistinctIds& DistinctIds::operator=(DistinctIds&& other) noexcept {
    if (this == &other) return *this;
    destroy();
    sketch_mode_ = other.sketch_mode_;
    if (sketch_mode_)
        new (&sketch_) SketchPtr(std::move(other.sketch_));
    else
        new (&list_) List(std::move(other.list_));
    return *this;
}

DistinctIds::~DistinctIds() { destroy(); }

void DistinctIds::destroy() noexcept {
    if (sketch_mode_)
        sketch_.~SketchPtr();
    else
        list_.~List();
}

void DistinctIds::to_sketch() {
    if (sketch_mode_) return;
    auto sketch = dstd::make_unique<Sketch>();
    for (std::uint64_t id : list_) sketch->add_hash(mix(id));
    list_.~List();
    new (&sketch_) SketchPtr(std::move(sketch));
    sketch_mode_ = true;
}

void DistinctIds::insert(std::uint64_t id) {
    if (sketch_mode_) {
        sketch_->add_hash(mix(id));
        return;
    }
    const auto it = std::lower_bound(list_.begin(), list_.end(), id);
    if (it != list_.end() && *it == id) return;
    if (list_.size() == DISTINCT_EXACT_CAPACITY) {
        to_sketch();
        sketch_->add_hash(mix(id));
        return;
    }
    list_.insert(it, id);
}

void DistinctIds::merge(const DistinctIds& other) {
    if (other.empty()) return;
    if (!sketch_mode_ && !other.sketch_mode_) {
        const std::size_t na = list_.size();
        const std::size_t nb = other.list_.size();
        std::size_t i = 0, j = 0, shared = 0;
        while (i < na && j < nb) {
            if (list_[i] < other.list_[j]) {
                ++i;
            } else if (list_[i] > other.list_[j]) {
                ++j;
            } else {
                ++shared;
                ++i;
                ++j;
            }
        }
        const std::size_t total = na + nb - shared;
        if (total <= DISTINCT_EXACT_CAPACITY) {
            list_.resize(total);
            std::size_t a = na, b = nb, w = total;
            while (b > 0) {
                if (a > 0 && list_[a - 1] > other.list_[b - 1]) {
                    list_[--w] = list_[--a];
                } else if (a > 0 && list_[a - 1] == other.list_[b - 1]) {
                    list_[--w] = list_[--a];
                    --b;
                } else {
                    list_[--w] = other.list_[--b];
                }
            }
            return;
        }
        to_sketch();
        for (std::uint64_t id : other.list_) sketch_->add_hash(mix(id));
        return;
    }
    to_sketch();
    if (other.sketch_mode_) {
        sketch_->merge_from(*other.sketch_);
    } else {
        for (std::uint64_t id : other.list_) sketch_->add_hash(mix(id));
    }
}

std::uint64_t DistinctIds::count() const {
    return sketch_mode_ ? sketch_->estimate() : list_.size();
}

void DistinctIds::serialize_into(dstd::Vector<std::uint8_t>& buf) const {
    if (!sketch_mode_) {
        buf.push_back(TAG_LIST);
        dstd::append_varint(buf, list_.size());
        std::uint64_t prev = 0;
        for (std::uint64_t id : list_) {
            dstd::append_varint(buf, id - prev);
            prev = id;
        }
        return;
    }
    if (!sketch_->dense_registers().empty()) {
        const auto& regs = sketch_->dense_registers();
        buf.push_back(TAG_DENSE);
        buf.insert(buf.end(), regs.begin(), regs.end());
        return;
    }
    const auto& raw = sketch_->sparse_hashes();
    dstd::Vector<std::uint64_t> hashes(raw.begin(), raw.end());
    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
    buf.push_back(TAG_SPARSE);
    dstd::append_varint(buf, hashes.size());
    std::uint64_t prev = 0;
    for (std::uint64_t h : hashes) {
        dstd::append_varint(buf, h - prev);
        prev = h;
    }
}

bool DistinctIds::read_from(dstd::LeReader& r) {
    std::uint8_t tag = 0;
    if (!r.u8(tag)) return false;
    if (tag == TAG_LIST) {
        std::uint64_t n = 0;
        if (!get_count(r, DISTINCT_EXACT_CAPACITY, n)) return false;
        List ids;
        ids.reserve(n);
        std::uint64_t prev = 0;
        for (std::uint64_t i = 0; i < n; ++i) {
            std::uint64_t d = 0;
            if (!dstd::read_varint(r, d)) return false;
            if (i > 0 && (d == 0 || d > UINT64_MAX - prev)) return false;
            prev += d;
            ids.push_back(prev);
        }
        list_ = std::move(ids);
        return true;
    }
    auto sketch = dstd::make_unique<Sketch>();
    if (tag == TAG_SPARSE) {
        std::uint64_t n = 0;
        if (!get_count(r, Sketch::SPARSE_LIMIT, n)) return false;
        // keep std: set_sparse_hashes takes std::vector
        std::vector<std::uint64_t> hashes;
        hashes.reserve(n);
        std::uint64_t prev = 0;
        for (std::uint64_t i = 0; i < n; ++i) {
            std::uint64_t d = 0;
            if (!dstd::read_varint(r, d)) return false;
            if (i > 0 && (d == 0 || d > UINT64_MAX - prev)) return false;
            prev += d;
            hashes.push_back(prev);
        }
        sketch->set_sparse_hashes(std::move(hashes));
    } else if (tag == TAG_DENSE) {
        const std::uint8_t* regs = r.take(Sketch::REGISTERS);
        if (regs == nullptr) return false;
        sketch->set_dense_registers(
            // keep std: set_dense_registers takes std::vector
            std::vector<std::uint8_t>(regs, regs + Sketch::REGISTERS));
    } else {
        return false;
    }
    list_.~List();
    new (&sketch_) SketchPtr(std::move(sketch));
    sketch_mode_ = true;
    return true;
}

bool DistinctIds::skip(dstd::LeReader& r) {
    std::uint8_t tag = 0;
    if (!r.u8(tag)) return false;
    if (tag == TAG_DENSE) return r.take(Sketch::REGISTERS) != nullptr;
    if (tag != TAG_LIST && tag != TAG_SPARSE) return false;
    std::uint64_t n = 0;
    const std::uint64_t limit =
        tag == TAG_LIST ? DISTINCT_EXACT_CAPACITY : Sketch::SPARSE_LIMIT;
    if (!get_count(r, limit, n)) return false;
    std::uint64_t d = 0;
    for (std::uint64_t i = 0; i < n; ++i)
        if (!dstd::read_varint(r, d)) return false;
    return true;
}

AggPartial::AggPartial(double relative_accuracy)
    : duration_q(relative_accuracy),
      size_q(relative_accuracy),
      offset_q(relative_accuracy),
      accuracy(relative_accuracy) {}

void AggPartial::clear() {
    count = 0;
    ts = UINT64_MAX;
    te = 0;
    ts_last_start = 0;
    duration_values = 0;
    size_values = 0;
    offset_values = 0;
    duration = {};
    size = {};
    offset = {};
    duration_q = QuantileStore(accuracy);
    size_q = QuantileStore(accuracy);
    offset_q = QuantileStore(accuracy);
    pids = DistinctIds();
    tids = DistinctIds();
    files = DistinctIds();
    pid_tids = DistinctIds();
    pid_files = DistinctIds();
    pid_spans.clear();
    bins.clear();
    bin_interval = 0;
}

void AggPartial::observe(std::uint64_t pid, std::uint64_t tid,
                         std::uint64_t file_id) {
    pids.insert(pid);
    tids.insert(tid);
    pid_tids.insert(pid_tid_id(pid, tid));
    if (file_id != 0) {
        files.insert(file_id);
        pid_files.insert(pid_file_id(pid, file_id));
    }
}

void AggPartial::add_span(std::uint64_t pid, std::uint64_t ts_in,
                          std::uint64_t end) {
    if (ts_in < ts) ts = ts_in;
    if (end > te) te = end;
    if (ts_in > ts_last_start) ts_last_start = ts_in;
    const auto it = std::lower_bound(
        pid_spans.begin(), pid_spans.end(), pid,
        [](const PidSpan& s, std::uint64_t p) { return s.pid < p; });
    if (it != pid_spans.end() && it->pid == pid) {
        if (ts_in < it->first) it->first = ts_in;
        if (end > it->last) it->last = end;
        return;
    }
    pid_spans.insert(it, PidSpan{pid, ts_in, end});
}

void AggPartial::merge(const AggPartial& other) {
    bins.clear();
    bin_interval = 0;
    count += other.count;
    ts = std::min(ts, other.ts);
    te = std::max(te, other.te);
    ts_last_start = std::max(ts_last_start, other.ts_last_start);
    duration_values += other.duration_values;
    size_values += other.size_values;
    offset_values += other.offset_values;

    duration.merge(other.duration);
    size.merge(other.size);
    offset.merge(other.offset);
    duration_q.merge(other.duration_q);
    size_q.merge(other.size_q);
    offset_q.merge(other.offset_q);

    pids.merge(other.pids);
    tids.merge(other.tids);
    files.merge(other.files);
    pid_tids.merge(other.pid_tids);
    pid_files.merge(other.pid_files);

    if (!other.pid_spans.empty()) {
        const std::size_t na = pid_spans.size();
        const std::size_t nb = other.pid_spans.size();
        std::size_t i = 0, j = 0, shared = 0;
        while (i < na && j < nb) {
            if (pid_spans[i].pid < other.pid_spans[j].pid) {
                ++i;
            } else if (pid_spans[i].pid > other.pid_spans[j].pid) {
                ++j;
            } else {
                ++shared;
                ++i;
                ++j;
            }
        }
        const std::size_t total = na + nb - shared;
        pid_spans.resize(total);
        std::size_t a = na, b = nb, w = total;
        while (b > 0) {
            const PidSpan& o = other.pid_spans[b - 1];
            if (a > 0 && pid_spans[a - 1].pid > o.pid) {
                pid_spans[--w] = pid_spans[--a];
            } else if (a > 0 && pid_spans[a - 1].pid == o.pid) {
                PidSpan s = pid_spans[--a];
                s.first = std::min(s.first, o.first);
                s.last = std::max(s.last, o.last);
                pid_spans[--w] = s;
                --b;
            } else {
                pid_spans[--w] = o;
                --b;
            }
        }
    }
}

void AggPartial::normalize_bins() {
    const auto key_less = [](const TimeBin& a, const TimeBin& b) {
        if (a.pid != b.pid) return a.pid < b.pid;
        if (a.tid != b.tid) return a.tid < b.tid;
        return a.bin < b.bin;
    };
    if (!std::is_sorted(bins.begin(), bins.end(), key_less))
        std::sort(bins.begin(), bins.end(), key_less);
    std::size_t out = 0;
    for (std::size_t i = 0; i < bins.size(); ++i) {
        if (out != 0 && bins[out - 1].pid == bins[i].pid &&
            bins[out - 1].tid == bins[i].tid &&
            bins[out - 1].bin == bins[i].bin) {
            TimeBin& to = bins[out - 1];
            to.count += bins[i].count;
            to.duration_n += bins[i].duration_n;
            to.duration_sum += bins[i].duration_sum;
            to.size_n += bins[i].size_n;
            to.size_sum += bins[i].size_sum;
            continue;
        }
        bins[out++] = bins[i];
    }
    bins.resize(out);
}

void AggPartial::serialize_into(dstd::Vector<std::uint8_t>& buf) const {
    std::uint8_t flags = 0;
    if (ts <= te) flags |= HAS_TIMES;
    if (duration.n != 0) flags |= HAS_DURATION;
    if (size.n != 0) flags |= HAS_SIZE;
    if (offset.n != 0) flags |= HAS_OFFSET;
    if (bin_interval != 0 && !bins.empty()) flags |= HAS_BINS;
    buf.push_back(flags);
    dstd::append_varint(buf, count);
    if ((flags & HAS_TIMES) != 0) {
        dstd::append_varint(buf, ts);
        dstd::append_varint(buf, te - ts);
        dstd::append_varint(buf, ts_last_start - ts);
    }
    if ((flags & HAS_DURATION) != 0) {
        put_stat(buf, duration);
        put_quantiles(buf, duration_q);
        dstd::append_varint(buf, duration_values);
    }
    if ((flags & HAS_SIZE) != 0) {
        put_stat(buf, size);
        put_quantiles(buf, size_q);
        dstd::append_varint(buf, size_values);
    }
    if ((flags & HAS_OFFSET) != 0) {
        put_stat(buf, offset);
        put_quantiles(buf, offset_q);
        dstd::append_varint(buf, offset_values);
    }
    pids.serialize_into(buf);
    tids.serialize_into(buf);
    files.serialize_into(buf);
    pid_tids.serialize_into(buf);
    pid_files.serialize_into(buf);

    dstd::append_varint(buf, pid_spans.size());
    std::uint64_t prev = 0;
    for (const PidSpan& s : pid_spans) {
        dstd::append_varint(buf, s.pid - prev);
        dstd::append_varint(buf, s.first);
        dstd::append_varint(buf, s.last - s.first);
        prev = s.pid;
    }

    if ((flags & HAS_BINS) == 0) return;
    // Per bin: pid delta, tid (a delta within one pid), bin (a delta within one
    // thread), count, events without a duration, duration sum, size count and
    // size sum.
    dstd::append_varint(buf, bin_interval);
    dstd::append_varint(buf, bins.size());
    const TimeBin* prev_bin = nullptr;
    for (const TimeBin& b : bins) {
        const bool same_pid = prev_bin != nullptr && prev_bin->pid == b.pid;
        const bool same_tid = same_pid && prev_bin->tid == b.tid;
        dstd::append_varint(
            buf, prev_bin == nullptr ? b.pid : b.pid - prev_bin->pid);
        dstd::append_varint(buf, same_pid ? b.tid - prev_bin->tid : b.tid);
        dstd::append_varint(buf, same_tid ? b.bin - prev_bin->bin : b.bin);
        dstd::append_varint(buf, b.count);
        dstd::append_varint(buf, b.count - b.duration_n);
        dstd::append_varint(buf, b.duration_sum);
        dstd::append_varint(buf, b.size_n);
        dstd::append_varint(buf, b.size_sum);
        prev_bin = &b;
    }
}

dstd::Expected<AggPartial, common::ErrorCode> AggPartial::deserialize(
    dstd::Span<const std::uint8_t> bytes, double relative_accuracy) {
    return deserialize(bytes, relative_accuracy, Fields{});
}

dstd::Expected<AggPartial, common::ErrorCode> AggPartial::deserialize(
    dstd::Span<const std::uint8_t> bytes, double relative_accuracy,
    Fields keep) {
    dstd::LeReader r(bytes.data(), bytes.size());
    AggPartial p(relative_accuracy);
    const auto bad = [] { return dstd::Unexpected(common::ErrorCode::Parse); };

    std::uint8_t flags = 0;
    if (!r.u8(flags) || (flags & ~KNOWN_FLAGS) != 0) return bad();
    if (!dstd::read_varint(r, p.count)) return bad();
    if ((flags & HAS_TIMES) != 0) {
        std::uint64_t span = 0, last = 0;
        if (!dstd::read_varint(r, p.ts) || !dstd::read_varint(r, span) ||
            !dstd::read_varint(r, last) || span > UINT64_MAX - p.ts ||
            last > UINT64_MAX - p.ts) {
            return bad();
        }
        p.te = p.ts + span;
        p.ts_last_start = p.ts + last;
    }
    if ((flags & HAS_DURATION) != 0 &&
        (!get_stat(r, p.duration) ||
         !get_quantiles(r, p.duration_q, keep.quantiles) ||
         !dstd::read_varint(r, p.duration_values))) {
        return bad();
    }
    if ((flags & HAS_SIZE) != 0 &&
        (!get_stat(r, p.size) || !get_quantiles(r, p.size_q, keep.quantiles) ||
         !dstd::read_varint(r, p.size_values))) {
        return bad();
    }
    if ((flags & HAS_OFFSET) != 0) {
        if (!get_stat(r, p.offset) ||
            !get_quantiles(r, p.offset_q, keep.quantiles && keep.offset) ||
            !dstd::read_varint(r, p.offset_values)) {
            return bad();
        }
        if (!keep.offset) {
            p.offset = {};
            p.offset_values = 0;
        }
    }
    const auto read_ids = [&r](DistinctIds& ids, bool kept) {
        return kept ? ids.read_from(r) : DistinctIds::skip(r);
    };
    if (!read_ids(p.pids, keep.pids) || !read_ids(p.tids, keep.tids) ||
        !read_ids(p.files, keep.files) ||
        !read_ids(p.pid_tids, keep.pid_tids) ||
        !read_ids(p.pid_files, keep.pid_files)) {
        return bad();
    }

    std::uint64_t n = 0;
    if (!get_count(r, UINT64_MAX, n)) return bad();
    if (keep.pid_spans) p.pid_spans.reserve(n);
    std::uint64_t prev = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        std::uint64_t d = 0, first = 0, len = 0;
        if (!dstd::read_varint(r, d) || !dstd::read_varint(r, first) ||
            !dstd::read_varint(r, len)) {
            return bad();
        }
        if (i > 0 && (d == 0 || d > UINT64_MAX - prev)) return bad();
        if (len > UINT64_MAX - first) return bad();
        prev += d;
        if (keep.pid_spans)
            p.pid_spans.push_back(PidSpan{prev, first, first + len});
    }

    if ((flags & HAS_BINS) != 0) {
        std::uint64_t interval = 0;
        if (!dstd::read_varint(r, interval) || interval == 0 ||
            !get_count(r, UINT64_MAX, n) || n == 0) {
            return bad();
        }
        if (keep.bins) {
            p.bin_interval = interval;
            p.bins.reserve(n);
        }
        TimeBin prev_bin;
        for (std::uint64_t i = 0; i < n; ++i) {
            std::uint64_t dp = 0, t = 0, b = 0, missing = 0;
            TimeBin bin;
            if (!dstd::read_varint(r, dp) || !dstd::read_varint(r, t) ||
                !dstd::read_varint(r, b) || !dstd::read_varint(r, bin.count) ||
                !dstd::read_varint(r, missing) ||
                !dstd::read_varint(r, bin.duration_sum) ||
                !dstd::read_varint(r, bin.size_n) ||
                !dstd::read_varint(r, bin.size_sum) || missing > bin.count) {
                return bad();
            }
            const bool same_pid = i > 0 && dp == 0;
            if (i > 0 && dp > UINT64_MAX - prev_bin.pid) return bad();
            bin.pid = i == 0 ? dp : prev_bin.pid + dp;
            if (same_pid && t > UINT64_MAX - prev_bin.tid) return bad();
            bin.tid = same_pid ? prev_bin.tid + t : t;
            const bool same_tid = same_pid && t == 0;
            if (same_tid && (b == 0 || b > UINT64_MAX - prev_bin.bin))
                return bad();
            bin.bin = same_tid ? prev_bin.bin + b : b;
            bin.duration_n = bin.count - missing;
            prev_bin = bin;
            if (keep.bins) p.bins.push_back(bin);
        }
    }
    if (r.remaining() != 0) return bad();
    return p;
}

}  // namespace dftracer::utils::index::schemas::dft::agg
