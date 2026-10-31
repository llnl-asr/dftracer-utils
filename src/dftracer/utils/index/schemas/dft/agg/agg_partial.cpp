#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/varint.h>
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
constexpr std::uint8_t KNOWN_FLAGS = 15;

constexpr std::uint8_t STAT_DOMAIN_MASK = 3;
constexpr std::uint8_t STAT_MOMENTS = 4;

bool get_varint(LeReader& r, std::uint64_t& out) {
    const std::uint8_t* p = r.position();
    if (!read_varint(p, p + r.remaining(), out)) return false;
    r.take(static_cast<std::size_t>(p - r.position()));
    return true;
}

// Every encoded element takes at least one byte, so a count above the bytes
// left is malformed; checking it first keeps a reserve small.
bool get_count(LeReader& r, std::uint64_t limit, std::uint64_t& n) {
    return get_varint(r, n) && n <= limit && n <= r.remaining();
}

std::uint64_t mix(std::uint64_t id) {
    return dftracer::utils::hash::splitmix64(id);
}

void put_stat(dstd::Vector<std::uint8_t>& buf, const FieldStat& s) {
    const bool moments = s.shift != 0.0 || s.cmean != 0.0 || s.cm2 != 0.0 ||
                         s.cm3 != 0.0 || s.cm4 != 0.0;
    buf.push_back(static_cast<std::uint8_t>(
        static_cast<std::uint8_t>(s.domain) | (moments ? STAT_MOMENTS : 0)));
    append_varint(buf, s.n);
    append_f64_le(buf, s.sum);
    if (s.domain == FieldStatDomain::F64) {
        append_f64_le(buf, s.min);
        append_f64_le(buf, s.max);
    } else {
        // min and max are the doubles of emin and emax, so they are not kept.
        append_varint(buf, std::bit_cast<std::uint64_t>(s.esum));
        append_varint(buf, std::bit_cast<std::uint64_t>(s.emin));
        append_varint(buf, std::bit_cast<std::uint64_t>(s.emax));
        append_varint(buf, std::bit_cast<std::uint64_t>(s.ecarry));
    }
    if (moments) {
        append_f64_le(buf, s.shift);
        append_f64_le(buf, s.cmean);
        append_f64_le(buf, s.cm2);
        append_f64_le(buf, s.cm3);
        append_f64_le(buf, s.cm4);
    }
}

bool get_stat(LeReader& r, FieldStat& s) {
    std::uint8_t b = 0;
    if (!r.u8(b) || (b & ~(STAT_DOMAIN_MASK | STAT_MOMENTS)) != 0) {
        return false;
    }
    const std::uint8_t domain = b & STAT_DOMAIN_MASK;
    if (domain > static_cast<std::uint8_t>(FieldStatDomain::F64)) return false;
    s = FieldStat{};
    s.domain = static_cast<FieldStatDomain>(domain);
    if (!get_varint(r, s.n) || !r.f64(s.sum)) return false;
    if (s.domain == FieldStatDomain::F64) {
        if (!r.f64(s.min) || !r.f64(s.max)) return false;
    } else {
        std::uint64_t es = 0, emin = 0, emax = 0, carry = 0;
        if (!get_varint(r, es) || !get_varint(r, emin) ||
            !get_varint(r, emax) || !get_varint(r, carry)) {
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
    append_u32_le(buf, 0);
    q.serialize_into(buf);
    write_u32_le(buf.data() + at,
                 static_cast<std::uint32_t>(buf.size() - at - 4));
}

bool get_quantiles(LeReader& r, QuantileStore& out, bool keep = true) {
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
        append_varint(buf, list_.size());
        std::uint64_t prev = 0;
        for (std::uint64_t id : list_) {
            append_varint(buf, id - prev);
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
    append_varint(buf, hashes.size());
    std::uint64_t prev = 0;
    for (std::uint64_t h : hashes) {
        append_varint(buf, h - prev);
        prev = h;
    }
}

bool DistinctIds::read_from(LeReader& r) {
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
            if (!get_varint(r, d)) return false;
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
        std::vector<std::uint64_t> hashes;
        hashes.reserve(n);
        std::uint64_t prev = 0;
        for (std::uint64_t i = 0; i < n; ++i) {
            std::uint64_t d = 0;
            if (!get_varint(r, d)) return false;
            if (i > 0 && (d == 0 || d > UINT64_MAX - prev)) return false;
            prev += d;
            hashes.push_back(prev);
        }
        sketch->set_sparse_hashes(std::move(hashes));
    } else if (tag == TAG_DENSE) {
        const std::uint8_t* regs = r.take(Sketch::REGISTERS);
        if (regs == nullptr) return false;
        sketch->set_dense_registers(
            std::vector<std::uint8_t>(regs, regs + Sketch::REGISTERS));
    } else {
        return false;
    }
    list_.~List();
    new (&sketch_) SketchPtr(std::move(sketch));
    sketch_mode_ = true;
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
    args.clear();
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

ArgStat& AggPartial::arg(std::uint32_t id) {
    const auto it = std::lower_bound(
        args.begin(), args.end(), id,
        [](const ArgStat& a, std::uint32_t v) { return a.id < v; });
    if (it != args.end() && it->id == id) return *it;
    return *args.emplace(it, id, accuracy);
}

void AggPartial::merge(const AggPartial& other) {
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

    if (!other.args.empty()) {
        const std::size_t na = args.size();
        const std::size_t nb = other.args.size();
        std::size_t i = 0, j = 0, shared = 0;
        while (i < na && j < nb) {
            if (args[i].id < other.args[j].id) {
                ++i;
            } else if (args[i].id > other.args[j].id) {
                ++j;
            } else {
                ++shared;
                ++i;
                ++j;
            }
        }
        const std::size_t total = na + nb - shared;
        args.resize(total);
        std::size_t a = na, b = nb, w = total;
        while (b > 0) {
            const ArgStat& o = other.args[b - 1];
            if (a > 0 && args[a - 1].id > o.id) {
                args[--w] = std::move(args[--a]);
            } else if (a > 0 && args[a - 1].id == o.id) {
                args[--w] = std::move(args[--a]);
                args[w].stat.merge(o.stat);
                args[w].quantiles.merge(o.quantiles);
                args[w].values += o.values;
                --b;
            } else {
                ArgStat fresh(o.id, accuracy);
                fresh.stat = o.stat;
                fresh.quantiles.merge(o.quantiles);
                fresh.values = o.values;
                args[--w] = std::move(fresh);
                --b;
            }
        }
    }
}

void AggPartial::serialize_into(dstd::Vector<std::uint8_t>& buf) const {
    std::uint8_t flags = 0;
    if (ts <= te) flags |= HAS_TIMES;
    if (duration.n != 0) flags |= HAS_DURATION;
    if (size.n != 0) flags |= HAS_SIZE;
    if (offset.n != 0) flags |= HAS_OFFSET;
    buf.push_back(flags);
    append_varint(buf, count);
    if ((flags & HAS_TIMES) != 0) {
        append_varint(buf, ts);
        append_varint(buf, te - ts);
        append_varint(buf, ts_last_start - ts);
    }
    if ((flags & HAS_DURATION) != 0) {
        put_stat(buf, duration);
        put_quantiles(buf, duration_q);
        append_varint(buf, duration_values);
    }
    if ((flags & HAS_SIZE) != 0) {
        put_stat(buf, size);
        put_quantiles(buf, size_q);
        append_varint(buf, size_values);
    }
    if ((flags & HAS_OFFSET) != 0) {
        put_stat(buf, offset);
        put_quantiles(buf, offset_q);
        append_varint(buf, offset_values);
    }
    pids.serialize_into(buf);
    tids.serialize_into(buf);
    files.serialize_into(buf);
    pid_tids.serialize_into(buf);
    pid_files.serialize_into(buf);

    append_varint(buf, pid_spans.size());
    std::uint64_t prev = 0;
    for (const PidSpan& s : pid_spans) {
        append_varint(buf, s.pid - prev);
        append_varint(buf, s.first);
        append_varint(buf, s.last - s.first);
        prev = s.pid;
    }

    append_varint(buf, args.size());
    std::uint32_t prev_id = 0;
    for (const ArgStat& a : args) {
        append_varint(buf, a.id - prev_id);
        put_stat(buf, a.stat);
        put_quantiles(buf, a.quantiles);
        append_varint(buf, a.values);
        prev_id = a.id;
    }
}

dstd::Expected<AggPartial, ErrorCode> AggPartial::deserialize(
    dstd::Span<const std::uint8_t> bytes, double relative_accuracy) {
    return deserialize(bytes, relative_accuracy, Fields{});
}

dstd::Expected<AggPartial, ErrorCode> AggPartial::deserialize(
    dstd::Span<const std::uint8_t> bytes, double relative_accuracy,
    Fields keep) {
    LeReader r(bytes.data(), bytes.size());
    AggPartial p(relative_accuracy);
    const auto bad = [] { return dstd::Unexpected(ErrorCode::Parse); };

    std::uint8_t flags = 0;
    if (!r.u8(flags) || (flags & ~KNOWN_FLAGS) != 0) return bad();
    if (!get_varint(r, p.count)) return bad();
    if ((flags & HAS_TIMES) != 0) {
        std::uint64_t span = 0, last = 0;
        if (!get_varint(r, p.ts) || !get_varint(r, span) ||
            !get_varint(r, last) || span > UINT64_MAX - p.ts ||
            last > UINT64_MAX - p.ts) {
            return bad();
        }
        p.te = p.ts + span;
        p.ts_last_start = p.ts + last;
    }
    if ((flags & HAS_DURATION) != 0 &&
        (!get_stat(r, p.duration) ||
         !get_quantiles(r, p.duration_q, keep.quantiles) ||
         !get_varint(r, p.duration_values))) {
        return bad();
    }
    if ((flags & HAS_SIZE) != 0 &&
        (!get_stat(r, p.size) || !get_quantiles(r, p.size_q, keep.quantiles) ||
         !get_varint(r, p.size_values))) {
        return bad();
    }
    if ((flags & HAS_OFFSET) != 0 &&
        (!get_stat(r, p.offset) ||
         !get_quantiles(r, p.offset_q, keep.quantiles) ||
         !get_varint(r, p.offset_values))) {
        return bad();
    }
    if (!p.pids.read_from(r) || !p.tids.read_from(r) || !p.files.read_from(r) ||
        !p.pid_tids.read_from(r) || !p.pid_files.read_from(r)) {
        return bad();
    }

    std::uint64_t n = 0;
    if (!get_count(r, UINT64_MAX, n)) return bad();
    p.pid_spans.reserve(n);
    std::uint64_t prev = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        std::uint64_t d = 0, first = 0, len = 0;
        if (!get_varint(r, d) || !get_varint(r, first) || !get_varint(r, len)) {
            return bad();
        }
        if (i > 0 && (d == 0 || d > UINT64_MAX - prev)) return bad();
        if (len > UINT64_MAX - first) return bad();
        prev += d;
        p.pid_spans.push_back(PidSpan{prev, first, first + len});
    }

    if (!get_count(r, UINT64_MAX, n)) return bad();
    if (keep.args) p.args.reserve(n);
    std::uint64_t prev_id = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        std::uint64_t d = 0;
        if (!get_varint(r, d)) return bad();
        if ((i > 0 && d == 0) ||
            d > std::numeric_limits<std::uint32_t>::max() - prev_id) {
            return bad();
        }
        prev_id += d;
        if (!keep.args) {
            FieldStat skipped;
            QuantileStore unused(relative_accuracy);
            std::uint64_t values = 0;
            if (!get_stat(r, skipped) || !get_quantiles(r, unused, false) ||
                !get_varint(r, values)) {
                return bad();
            }
            continue;
        }
        ArgStat a(static_cast<std::uint32_t>(prev_id), relative_accuracy);
        if (!get_stat(r, a.stat) ||
            !get_quantiles(r, a.quantiles, keep.quantiles) ||
            !get_varint(r, a.values)) {
            return bad();
        }
        p.args.push_back(std::move(a));
    }
    if (r.remaining() != 0) return bad();
    return p;
}

}  // namespace dftracer::utils::index::schemas::dft::agg
