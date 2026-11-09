// SPDX-License-Identifier: MIT
// The _advanced create functions take the tracked allocator.
#define ZSTD_STATIC_LINKING_ONLY
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/filesystem_info.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/spill_dir.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/spill.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/bits.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/ptr_hash.h>
#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/heap.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <stdexcept>

namespace dftracer::utils::dataframe::spill {

namespace {

template <class T>
void put_pod(dstd::String& out, T v) {
    out.append(reinterpret_cast<const char*>(&v), sizeof(T));
}

// The bytes of a blob being read. A read past the end sets `bad` and gives
// zeros, so the reader checks once per column instead of once per field.
struct In {
    const std::uint8_t*& p;
    const std::uint8_t* end;
    bool bad = false;
};

template <class T>
T get_pod(In& in) {
    if (in.bad || static_cast<std::size_t>(in.end - in.p) < sizeof(T)) {
        in.bad = true;
        return T{};
    }
    T v;
    std::memcpy(&v, in.p, sizeof(T));
    in.p += sizeof(T);
    return v;
}

void put_bytes(dstd::String& out, const void* data, std::size_t n) {
    out.append(static_cast<const char*>(data), n);
}

const std::uint8_t* take_bytes(In& in, std::size_t n) {
    if (in.bad || static_cast<std::size_t>(in.end - in.p) < n) {
        in.bad = true;
        return nullptr;
    }
    const std::uint8_t* start = in.p;
    in.p += n;
    return start;
}

dstd::Unexpected<dstd::Error> truncated_read() {
    return common::make_error<"spill: truncated read">(
        common::ErrorCode::Parse);
}

// Validity bitmap (Arrow layout, 1 = valid) built from per-row null flags.
dstd::Vector<std::uint8_t> validity_bitmap(const Series& s) {
    const std::int64_t n = s.length();
    dstd::Vector<std::uint8_t> bits(static_cast<std::size_t>((n + 7) / 8), 0);
    for (std::int64_t i = 0; i < n; ++i)
        if (!s.is_null(i))
            bits[static_cast<std::size_t>(i >> 3)] |=
                static_cast<std::uint8_t>(1u << (i & 7));
    return bits;
}

}  // namespace

namespace {
void add_new_bytes(const dftu_series* c,
                   dstd::HashSet<const void*, dstd::PtrHash>& seen,
                   std::size_t& total) {
    auto add = [&](const BufferView& b) {
        if (b.block() && seen.insert(b.data()).second) total += b.size();
    };
    add(c->data);
    add(c->offsets);
    add(c->validity);
    // A view column's data buffers are not counted: for a scan they are the
    // string table's blocks, shared by every part and freed by no spill, and
    // walking their list per part slows a collect.
    for (const auto& n : c->nested)
        if (n.series) add_new_bytes(n.series.get(), seen, total);
}
}  // namespace

std::size_t new_buffer_bytes(const dstd::Vector<Series>& cols,
                             dstd::HashSet<const void*, dstd::PtrHash>& seen) {
    std::size_t total = 0;
    for (const Series& c : cols)
        if (c.valid()) add_new_bytes(c.handle(), seen, total);
    return total;
}

std::size_t columns_bytes(const dstd::Vector<Series>& cols) {
    std::size_t total = 0;
    for (const Series& c : cols) {
        const std::int64_t n = c.length();
        const TypeId t = c.type();
        if (c.encoding() == Encoding::Dictionary ||
            c.encoding() == Encoding::View ||
            c.encoding() == Encoding::Chunked) {
            total +=
                static_cast<std::size_t>(dftu_series_buffer_bytes(c.handle()));
        } else if (is_wide_offset_type(t)) {  // LargeString / LargeBinary
            const std::int64_t* offs = c.offsets64();
            total +=
                static_cast<std::size_t>(n + 1) * sizeof(std::int64_t) +
                (n > 0 && offs != nullptr ? static_cast<std::size_t>(offs[n])
                                          : 0);
        } else if (t == TypeId::String || t == TypeId::Binary) {
            const std::int32_t* offs = dftu_series_offsets(c.handle());
            total +=
                static_cast<std::size_t>(n + 1) * sizeof(std::int32_t) +
                (n > 0 && offs != nullptr ? static_cast<std::size_t>(offs[n])
                                          : 0);
        } else if (t == TypeId::FixedSizeBinary) {
            total +=
                static_cast<std::size_t>(n) *
                static_cast<std::size_t>(dftu_series_fixed_size(c.handle()));
        } else {
            total += buffer_bytes(t, n);
        }
    }
    return total;
}

namespace {

// Cursor replaying a Spool: in-memory morsels first, then the spilled run.
class SpoolReader : public Cursor {
   public:
    SpoolReader(const dstd::Vector<Morsel>* mem, dstd::UniquePtr<Reader> disk)
        : mem_(mem), disk_(std::move(disk)) {}
    coro::CoroTask<MorselResult> next(std::int64_t max_rows) override {
        MorselResult mem;
        if (try_next(max_rows, mem)) co_return std::move(mem);
        if (disk_) co_return co_await disk_->next(max_rows);
        co_return dstd::nullopt;
    }

    // True only while replaying the in-memory prefix, which shares buffers and
    // never suspends. Once that is drained the run continues on disk, where
    // next() must do the awaiting.
    bool try_next(std::int64_t, MorselResult& out) override {
        if (pos_ >= mem_->size()) return false;
        const Morsel& m = (*mem_)[pos_++];
        Morsel o;
        o.rows = m.rows;
        o.columns.reserve(m.columns.size());
        for (const Series& c : m.columns) o.columns.push_back(c.share());
        out = std::move(o);
        return true;
    }

   private:
    const dstd::Vector<Morsel>* mem_;
    std::size_t pos_ = 0;
    dstd::UniquePtr<Reader> disk_;
};

// Cursor replaying a Spool backwards: the spilled morsels from the last, then
// the in-memory ones from the last.
class SpoolReverseReader : public Cursor {
   public:
    SpoolReverseReader(const dstd::Vector<Morsel>* mem,
                       dstd::UniquePtr<Reader> disk,
                       dstd::Vector<std::uint64_t> offsets)
        : mem_(mem),
          offsets_(std::move(offsets)),
          left_(mem->size()),
          disk_(std::move(disk)) {}
    coro::CoroTask<MorselResult> next(std::int64_t max_rows) override {
        if (!offsets_.empty()) {
            const auto sought = disk_->seek(offsets_.back());
            if (!sought) co_return sought.error();
            offsets_.pop_back();
            co_return co_await disk_->next(max_rows);
        }
        if (left_ == 0) co_return dstd::nullopt;
        const Morsel& m = (*mem_)[--left_];
        Morsel o;
        o.rows = m.rows;
        o.columns.reserve(m.columns.size());
        for (const Series& c : m.columns) o.columns.push_back(c.share());
        co_return std::move(o);
    }

   private:
    const dstd::Vector<Morsel>* mem_;
    dstd::Vector<std::uint64_t> offsets_;
    std::size_t left_;
    dstd::UniquePtr<Reader> disk_;
};

}  // namespace

namespace {

// The `count` values of `child` from `base`: `child` itself when they are
// all of it, else a gather (slice covers fixed-width types only).
dstd::Result<Series> list_values(Series child, std::int64_t base,
                                 std::int64_t count) {
    if (base == 0 && count == child.length()) return child;
    dstd::Vector<std::int64_t> rows(static_cast<std::size_t>(count));
    for (std::int64_t i = 0; i < count; ++i)
        rows[static_cast<std::size_t>(i)] = base + i;
    Series out = child.take(rows);
    if (!out.valid())
        return common::make_error<"spill: cannot gather a {} list child">(
            common::ErrorCode::InvalidArgument, type_name(child.type()));
    return out;
}

void put_series_trailer(dstd::String& out, const Series& s) {
    const dftu_series& h = *s.handle();
    put_pod<std::uint8_t>(out, h.json() ? 1 : 0);
    put_pod<std::int32_t>(out, static_cast<std::int32_t>(h.time_unit()));
    put_pod<std::uint32_t>(out,
                           static_cast<std::uint32_t>(h.timezone().size()));
    put_bytes(out, h.timezone().data(), h.timezone().size());
}

}  // namespace

dstd::Result<void> put_series(dstd::String& out, const Series& s_in) {
    Series s =
        s_in.encoding() == Encoding::Flat ? s_in.share() : s_in.materialize();
    const TypeId t = s.type();
    const std::int64_t n = s.length();
    const std::int64_t nulls = s.null_count();
    put_pod<std::int32_t>(out, static_cast<std::int32_t>(t));
    put_pod<std::int64_t>(out, n);
    put_pod<std::int64_t>(out, nulls);
    if (nulls > 0) {
        dstd::Vector<std::uint8_t> v = validity_bitmap(s);
        put_bytes(out, v.data(), v.size());
    }
    if (t == TypeId::FixedSizeList) {
        const std::int32_t width = s.handle()->fixed_size();
        put_pod<std::int32_t>(out, width);
        auto values = list_values(s.child(0), 0, n * width);
        if (!values) return values.error();
        auto put = put_series(out, *values);
        if (!put) return put;
        put_series_trailer(out, s);
        return {};
    }
    if (t == TypeId::List || t == TypeId::LargeList || t == TypeId::Map) {
        const bool wide = t == TypeId::LargeList;
        dstd::Vector<std::int64_t> offs(static_cast<std::size_t>(n + 1));
        for (std::int64_t i = 0; i <= n; ++i)
            offs[static_cast<std::size_t>(i)] =
                wide ? s.offsets64()[i] : s.offsets()[i];
        const std::int64_t base = offs.front();
        for (auto& o : offs) o -= base;
        for (const std::int64_t o : offs)
            if (wide)
                put_pod<std::int64_t>(out, o);
            else
                put_pod<std::int32_t>(out, static_cast<std::int32_t>(o));
        auto values = list_values(s.child(0), base, offs.back());
        if (!values) return values.error();
        auto put = put_series(out, *values);
        if (!put) return put;
        put_series_trailer(out, s);
        return {};
    }
    if (t == TypeId::Struct) {
        const std::int64_t fields = s.num_children();
        put_pod<std::int32_t>(out, static_cast<std::int32_t>(fields));
        for (std::int64_t i = 0; i < fields; ++i) {
            const dstd::String name = s.field_name(i);
            put_pod<std::uint32_t>(out,
                                   static_cast<std::uint32_t>(name.size()));
            put_bytes(out, name.data(), name.size());
            auto put = put_series(out, s.child(i));
            if (!put) return put;
        }
        put_series_trailer(out, s);
        return {};
    }
    if (t == TypeId::Decimal128 || t == TypeId::Decimal256) {
        // read_f64 divides by 10^decimal_scale, so it must round-trip too.
        const DataType dt = s.data_type();
        put_pod<std::int32_t>(out, dt.decimal_precision());
        put_pod<std::int32_t>(out, dt.decimal_scale());
        put_bytes(out, dftu_series_data(s.handle()), buffer_bytes(t, n));
    } else if (t == TypeId::FixedSizeBinary) {
        const std::int32_t width = dftu_series_fixed_size(s.handle());
        put_pod<std::int32_t>(out, width);
        put_bytes(
            out, dftu_series_data(s.handle()),
            static_cast<std::size_t>(n) * static_cast<std::size_t>(width));
    } else if (is_wide_offset_type(t)) {  // LargeString / LargeBinary
        const std::int64_t* offs = s.offsets64();
        const std::int64_t nbytes = n > 0 ? offs[n] : 0;
        put_pod<std::int64_t>(out, nbytes);
        put_bytes(out, offs,
                  static_cast<std::size_t>(n + 1) * sizeof(std::int64_t));
        put_bytes(out, dftu_series_data(s.handle()),
                  static_cast<std::size_t>(nbytes));
    } else if (!byte_width(t)) {  // String / Binary
        const std::int32_t* offs = dftu_series_offsets(s.handle());
        const std::int64_t nbytes = n > 0 ? offs[n] : 0;
        put_pod<std::int64_t>(out, nbytes);
        put_bytes(out, offs,
                  static_cast<std::size_t>(n + 1) * sizeof(std::int32_t));
        put_bytes(out, dftu_series_data(s.handle()),
                  static_cast<std::size_t>(nbytes));
    } else {
        const std::size_t bytes = buffer_bytes(t, n);
        put_bytes(out, dftu_series_data(s.handle()), bytes);
    }
    put_series_trailer(out, s);
    return {};
}

namespace {

dstd::Result<Series> get_series_data(In& in);

dstd::Result<Series> get_series_in(In& in) {
    auto data = get_series_data(in);
    if (!data) return data;
    Series s = std::move(*data);
    const bool json = get_pod<std::uint8_t>(in) != 0;
    const auto unit = static_cast<TimeUnit>(get_pod<std::int32_t>(in));
    const auto zone_bytes = get_pod<std::uint32_t>(in);
    const auto* zone = take_bytes(in, zone_bytes);
    if (in.bad) return truncated_read();
    dftu_series* h = s.handle();
    h->set_json(json);
    h->set_time_unit(unit);
    h->set_timezone(
        dstd::StringView(reinterpret_cast<const char*>(zone), zone_bytes));
    return s;
}

}  // namespace

dstd::Result<Series> get_series(const std::uint8_t*& p,
                                const std::uint8_t* end) {
    In in{p, end};
    return get_series_in(in);
}

namespace {

dstd::Result<Series> get_series_data(In& in) {
    const auto type = static_cast<TypeId>(get_pod<std::int32_t>(in));
    const std::int64_t n = get_pod<std::int64_t>(in);
    const std::int64_t nulls = get_pod<std::int64_t>(in);
    if (in.bad) return truncated_read();
    const std::uint8_t* validity = nullptr;
    if (nulls > 0)
        validity = take_bytes(in, static_cast<std::size_t>((n + 7) / 8));
    if (in.bad) return truncated_read();
    const auto dt = static_cast<dftu_dtype>(static_cast<std::int32_t>(type));
    if (type == TypeId::FixedSizeList) {
        const auto width = get_pod<std::int32_t>(in);
        if (in.bad) return truncated_read();
        auto child = get_series_in(in);
        if (!child) return child;
        auto* col = new dftu_series();
        col->type = type;
        col->encoding = Encoding::Flat;
        col->length = n;
        col->set_fixed_size(width);
        col->set_child(adopt_series(child->release()));
        if (validity != nullptr) {
            const std::size_t vbytes = static_cast<std::size_t>((n + 7) / 8);
            col->validity = new_buffer(vbytes);
            std::memcpy(writable(col->validity), validity, vbytes);
            col->null_count = nulls;
        }
        return Series{col};
    }
    if (type == TypeId::List || type == TypeId::LargeList ||
        type == TypeId::Map) {
        const bool wide = type == TypeId::LargeList;
        const std::size_t width =
            wide ? sizeof(std::int64_t) : sizeof(std::int32_t);
        const std::size_t off_bytes = static_cast<std::size_t>(n + 1) * width;
        const std::uint8_t* offs = take_bytes(in, off_bytes);
        if (in.bad) return truncated_read();
        auto child = get_series_in(in);
        if (!child) return child;
        auto* col = new dftu_series();
        col->type = type;
        col->encoding = Encoding::Flat;
        col->length = n;
        auto buffer = new_buffer(off_bytes);
        if (off_bytes != 0) std::memcpy(writable(buffer), offs, off_bytes);
        col->offsets = std::move(buffer);
        col->set_child(adopt_series(child->release()));
        if (validity != nullptr) {
            const std::size_t vbytes = static_cast<std::size_t>((n + 7) / 8);
            col->validity = new_buffer(vbytes);
            std::memcpy(writable(col->validity), validity, vbytes);
            col->null_count = nulls;
        }
        return Series{col};
    }
    if (type == TypeId::Struct) {
        const auto fields = get_pod<std::int32_t>(in);
        if (in.bad) return truncated_read();
        auto* col = new dftu_series();
        col->type = type;
        col->encoding = Encoding::Flat;
        col->length = n;
        // Frees the column on a failed read.
        Series owner{col};
        for (std::int32_t i = 0; i < fields; ++i) {
            const auto name_bytes = get_pod<std::uint32_t>(in);
            const auto* name = take_bytes(in, name_bytes);
            if (in.bad) return truncated_read();
            dstd::String field(reinterpret_cast<const char*>(name), name_bytes);
            auto child = get_series_in(in);
            if (!child) return child;
            col->nested.push_back(
                {adopt_series(child->release()), std::move(field)});
        }
        if (validity != nullptr) {
            const std::size_t vbytes = static_cast<std::size_t>((n + 7) / 8);
            col->validity = new_buffer(vbytes);
            std::memcpy(writable(col->validity), validity, vbytes);
            col->null_count = nulls;
        }
        return owner;
    }
    if (type == TypeId::Decimal128 || type == TypeId::Decimal256) {
        const std::int32_t precision = get_pod<std::int32_t>(in);
        const std::int32_t scale = get_pod<std::int32_t>(in);
        const std::size_t bytes = buffer_bytes(type, n);
        const std::uint8_t* data = take_bytes(in, bytes);
        if (in.bad) return truncated_read();
        auto* col = new dftu_series();
        col->type = type;
        col->encoding = Encoding::Flat;
        col->length = n;
        col->set_decimal(precision, scale);
        col->data = new_buffer(bytes);
        if (bytes != 0) std::memcpy(writable(col->data), data, bytes);
        if (validity != nullptr) {
            const std::size_t vbytes = static_cast<std::size_t>((n + 7) / 8);
            col->validity = new_buffer(vbytes);
            std::memcpy(writable(col->validity), validity, vbytes);
            col->null_count = nulls;
        }
        return Series{col};
    }
    if (type == TypeId::FixedSizeBinary) {
        const std::int32_t width = get_pod<std::int32_t>(in);
        const std::size_t bytes =
            static_cast<std::size_t>(n) * static_cast<std::size_t>(width);
        const std::uint8_t* data = take_bytes(in, bytes);
        if (in.bad) return truncated_read();
        auto* col = new dftu_series();
        col->type = type;
        col->encoding = Encoding::Flat;
        col->length = n;
        col->set_fixed_size(width);
        col->data = new_buffer(bytes);
        if (bytes != 0) std::memcpy(writable(col->data), data, bytes);
        if (validity != nullptr) {
            const std::size_t vbytes = static_cast<std::size_t>((n + 7) / 8);
            col->validity = new_buffer(vbytes);
            std::memcpy(writable(col->validity), validity, vbytes);
            col->null_count = nulls;
        }
        return Series{col};
    }
    if (is_wide_offset_type(type) || !byte_width(type)) {
        // String, Binary, LargeString and LargeBinary. The offsets sit at any
        // byte of the blob, so they are copied, never read in place.
        const std::size_t width = is_wide_offset_type(type)
                                      ? sizeof(std::int64_t)
                                      : sizeof(std::int32_t);
        const std::int64_t nbytes = get_pod<std::int64_t>(in);
        const std::size_t off_bytes = static_cast<std::size_t>(n + 1) * width;
        const std::uint8_t* offs = take_bytes(in, off_bytes);
        const std::uint8_t* data =
            take_bytes(in, static_cast<std::size_t>(nbytes));
        if (in.bad) return truncated_read();
        auto* col = new dftu_series();
        col->type = type;
        col->encoding = Encoding::Flat;
        col->length = n;
        col->offsets = new_buffer(off_bytes);
        std::memcpy(writable(col->offsets), offs, off_bytes);
        col->data = new_buffer(static_cast<std::size_t>(nbytes));
        if (nbytes != 0)
            std::memcpy(writable(col->data), data,
                        static_cast<std::size_t>(nbytes));
        if (validity != nullptr) {
            const std::size_t vbytes = static_cast<std::size_t>((n + 7) / 8);
            col->validity = new_buffer(vbytes);
            std::memcpy(writable(col->validity), validity, vbytes);
            col->null_count = nulls;
        }
        return Series{col};
    }
    const std::size_t bytes = buffer_bytes(type, n);
    const std::uint8_t* data = take_bytes(in, bytes);
    if (in.bad) return truncated_read();
    return Series{dftu_series_new_flat(dt, data, n, validity)};
}

}  // namespace

namespace {

constexpr std::size_t REGION_ALIGN = 64;

struct Mapping : dftracer::utils::dstd::HeapObject {
    void* addr;
    std::size_t bytes;
};

void unmap(void* context) {
    auto* m = static_cast<Mapping*>(context);
    ::munmap(m->addr, m->bytes);
    delete m;
}

bool is_nested(TypeId t) {
    return t == TypeId::Struct || t == TypeId::List || t == TypeId::LargeList ||
           t == TypeId::FixedSizeList || t == TypeId::Map;
}

struct Piece {
    const void* src = nullptr;
    std::size_t bytes = 0;
    std::size_t at = 0;
    dstd::Vector<std::uint8_t> owned;
};

struct ColumnPlan {
    Series flat;
    Piece validity, offsets, data;
};

}  // namespace

PartFile::PartFile(dstd::UniquePtr<common::SpillFile> file)
    : file_(std::move(file)) {}

dstd::Result<dstd::UniquePtr<PartFile>> PartFile::create() {
    auto file = common::SpillFile::create();
    if (!file) return file.error();
    return dstd::make_unique<PartFile>(std::move(*file));
}

PartFile::~PartFile() = default;

dstd::Result<dstd::Vector<Series>> PartFile::append(
    const dstd::Vector<Series>& cols) {
    dstd::Vector<ColumnPlan> plans(cols.size());
    std::size_t at = 0;
    const auto place = [&at](Piece& p, const void* src, std::size_t bytes) {
        p.src = src;
        p.bytes = bytes;
        p.at = at;
        at = dstd::bits::align_up(at + bytes, REGION_ALIGN);
    };
    for (std::size_t i = 0; i < cols.size(); ++i) {
        const Series& c = cols[i];
        ColumnPlan& cp = plans[i];
        if (is_nested(c.type()) || c.length() == 0) continue;
        cp.flat = c.encoding() == Encoding::Flat ? c.share() : c.materialize();
        const Series& s = cp.flat;
        const TypeId t = s.type();
        const std::int64_t n = s.length();
        if (s.null_count() > 0) {
            cp.validity.owned = validity_bitmap(s);
            place(cp.validity, cp.validity.owned.data(),
                  cp.validity.owned.size());
        }
        std::size_t nbytes;
        if (is_wide_offset_type(t)) {
            const std::int64_t* offs = s.offsets64();
            place(cp.offsets, offs,
                  static_cast<std::size_t>(n + 1) * sizeof(std::int64_t));
            nbytes = static_cast<std::size_t>(offs[n]);
        } else if (!byte_width(t) && t != TypeId::FixedSizeBinary &&
                   t != TypeId::Decimal128 && t != TypeId::Decimal256) {
            const std::int32_t* offs = dftu_series_offsets(s.handle());
            place(cp.offsets, offs,
                  static_cast<std::size_t>(n + 1) * sizeof(std::int32_t));
            nbytes = static_cast<std::size_t>(offs[n]);
        } else if (t == TypeId::FixedSizeBinary) {
            nbytes =
                static_cast<std::size_t>(n) *
                static_cast<std::size_t>(dftu_series_fixed_size(s.handle()));
        } else {
            nbytes = buffer_bytes(t, n);
        }
        place(cp.data, dftu_series_data(s.handle()), nbytes);
    }

    const long page_size = ::sysconf(_SC_PAGESIZE);
    const std::size_t page =
        page_size > 0 ? static_cast<std::size_t>(page_size) : 4096;
    const std::size_t region_bytes = dstd::bits::align_up(at, page);
    dstd::Vector<Series> out;
    out.reserve(cols.size());
    if (region_bytes == 0) {
        for (const Series& c : cols) out.push_back(c.share());
        return out;
    }
    const std::uint64_t base = file_->reserve(region_bytes, page);
    {
        coro::BlockingRegion blocking;
        for (const ColumnPlan& cp : plans)
            for (const Piece* p : {&cp.validity, &cp.offsets, &cp.data})
                if (p->bytes != 0) {
                    const auto wrote =
                        file_->write_at(base + p->at, p->src, p->bytes);
                    if (!wrote) return wrote.error();
                }
        // The last bytes may be padding: extend so the mapped pages exist.
        const auto resized = file_->file().resize(base + region_bytes);
        if (!resized)
            return common::make_error<
                "spill: cannot extend the spill file ({}); set {} to a "
                "directory with free space">(common::ErrorCode::Io,
                                             resized.error().format(),
                                             common::constants::SPILL_DIR_ENV);
    }
    void* map = ::mmap(nullptr, region_bytes, PROT_READ, MAP_SHARED,
                       file_->file().native_handle(), static_cast<off_t>(base));
    if (map == MAP_FAILED)
        return common::make_error<
            "spill: cannot map the spill file ({}); set {} to a usable "
            "directory">(common::ErrorCode::Io, std::strerror(errno),
                         common::constants::SPILL_DIR_ENV);
    Mapping* mapping = new Mapping{{}, map, region_bytes};
    memory::BlockRef region =
        memory::Block::borrow(map, region_bytes, unmap, mapping);
    const BufferView whole(std::move(region));
    const auto window = [&](const Piece& p) -> BufferView {
        if (p.bytes == 0) return new_buffer(0);
        return whole.slice(p.at, p.bytes);
    };
    for (std::size_t i = 0; i < cols.size(); ++i) {
        const ColumnPlan& cp = plans[i];
        if (!cp.flat.valid()) {
            out.push_back(cols[i].share());
            continue;
        }
        const dftu_series& h = *cp.flat.handle();
        auto* col = new dftu_series();
        col->type = h.type;
        col->encoding = Encoding::Flat;
        col->length = h.length;
        col->null_count = h.null_count;
        col->params = h.params;
        col->data = window(cp.data);
        if (cp.offsets.bytes != 0) col->offsets = window(cp.offsets);
        if (cp.validity.bytes != 0) col->validity = window(cp.validity);
        out.emplace_back(col);
    }
    return out;
}

Dir::Dir(common::ScopedSpillSubdir dir) : dir_(std::move(dir)) {}

dstd::Result<dstd::SharedPtr<Dir>> Dir::create() {
    auto dir = common::ScopedSpillSubdir::create("dftu_lazy");
    if (!dir) return dir.error();
    return dstd::make_shared<Dir>(std::move(*dir));
}

Dir::~Dir() = default;

int Dir::next_run() { return runs_.fetch_add(1); }

void Dir::remove_run(const dstd::String& path) const noexcept {
    const auto size = dstd::fs::file_size(dstd::PathView(path));
    const std::uint64_t bytes = size.has_value() ? *size : 0;
    const auto removed = dstd::fs::remove(dstd::PathView(path));
    if (removed.has_value() && *removed && bytes) dir_.charge().release(bytes);
}

dstd::String Dir::run_path(int id) const {
    return dstd::String(
        (dir_.path().view() / dstd::fmt<"run_{}.bin">(id)).str());
}

namespace {

const ZSTD_customMem TRACKED_MEM = {
    [](void*, std::size_t size) -> void* {
        return dftracer::utils::memory::detail::c_malloc(size);
    },
    [](void*, void* p) { dftracer::utils::memory::detail::c_free(p); },
    nullptr};

bool spill_compression_on(const dstd::String& path) {
    const auto v = common::Env::get(common::constants::SPILL_COMPRESS_ENV);
    const auto is = [&](dstd::StringView a, dstd::StringView b = {},
                        dstd::StringView c = {}) {
        return v && (::dftracer::utils::common::detail::iequals(*v, a) ||
                     (!b.empty() &&
                      ::dftracer::utils::common::detail::iequals(*v, b)) ||
                     (!c.empty() &&
                      ::dftracer::utils::common::detail::iequals(*v, c)));
    };
    if (is("off", "0")) return false;
    if (is("on", "1", "zstd")) return true;
    return common::is_network_filesystem(common::filesystem_kind(path));
}

// A morsel smaller than this is not worth a compression call.
constexpr std::size_t COMPRESS_MIN_BYTES = 4096;

std::FILE* open_file(const dstd::String& path, const char* mode) {
    coro::BlockingRegion blocking;
    return std::fopen(path.c_str(), mode);
}

}  // namespace

struct Writer::Codec {
    ZSTD_CCtx* ctx = ZSTD_createCCtx_advanced(TRACKED_MEM);
    dstd::String out;
    ~Codec() { ZSTD_freeCCtx(ctx); }
};
struct Reader::Codec {
    ZSTD_DCtx* ctx = ZSTD_createDCtx_advanced(TRACKED_MEM);
    ~Codec() { ZSTD_freeDCtx(ctx); }
};

Writer::Writer(dstd::ScopedFile os, dstd::SharedPtr<common::SpillCharge> charge)
    : os_(std::move(os)), charge_(std::move(charge)) {}

dstd::Result<Writer> Writer::open(const dstd::String& path) {
    dstd::ScopedFile os(open_file(path, "wb"));
    if (!os)
        return common::make_error<"spill: cannot open run file {}">(
            common::ErrorCode::Io, path);
    Writer w(std::move(os),
             common::ScopedSpillSubdir::charge_for(dstd::PathView(path)));
    if (spill_compression_on(path)) w.codec_ = dstd::make_unique<Codec>();
    return w;
}

Writer::Writer(Writer&&) noexcept = default;
Writer& Writer::operator=(Writer&&) noexcept = default;
Writer::~Writer() = default;

// Each morsel is [stored bytes][raw bytes][payload]; the payload is zstd when
// the two differ.
dstd::Result<void> Writer::write(const dstd::Vector<Series>& cols,
                                 std::int64_t rows) {
    blob_.clear();
    put_pod<std::int64_t>(blob_, rows);
    put_pod<std::int32_t>(blob_, static_cast<std::int32_t>(cols.size()));
    for (const Series& c : cols) {
        auto put = put_series(blob_, c);
        if (!put) return put;
    }
    const char* payload = blob_.data();
    std::int64_t stored = static_cast<std::int64_t>(blob_.size());
    const std::int64_t raw = stored;
    if (codec_ && blob_.size() >= COMPRESS_MIN_BYTES) {
        codec_->out.resize(ZSTD_compressBound(blob_.size()));
        const std::size_t n = ZSTD_compressCCtx(codec_->ctx, codec_->out.data(),
                                                codec_->out.size(),
                                                blob_.data(), blob_.size(), 1);
        if (!ZSTD_isError(n) && n < blob_.size()) {
            payload = codec_->out.data();
            stored = static_cast<std::int64_t>(n);
        }
    }
    const std::size_t payload_bytes = static_cast<std::size_t>(stored);
    coro::BlockingRegion blocking;
    if (std::fwrite(&stored, sizeof(stored), 1, os_.get()) != 1 ||
        std::fwrite(&raw, sizeof(raw), 1, os_.get()) != 1 ||
        std::fwrite(payload, 1, payload_bytes, os_.get()) != payload_bytes)
        return common::make_error<
            "spill: cannot write a run file (disk full?)">(
            common::ErrorCode::Io);
    const std::uint64_t n =
        sizeof(stored) + sizeof(raw) + static_cast<std::uint64_t>(stored);
    written_ += n;
    if (charge_) charge_->add(n);
    return {};
}

dstd::Result<void> Writer::close() {
    coro::BlockingRegion blocking;
    std::FILE* f = os_.release();
    if (!f || std::fclose(f) != 0)
        return common::make_error<
            "spill: cannot finish a run file (disk full?)">(
            common::ErrorCode::Io);
    return {};
}

dstd::Result<std::uint64_t> write_run(Writer& w,
                                      const dstd::Vector<Series>& cols,
                                      std::int64_t rows,
                                      std::int64_t rows_per_morsel) {
    std::uint64_t largest = 0;
    rows_per_morsel = std::max<std::int64_t>(rows_per_morsel, 1);
    for (std::int64_t off = 0; off < rows; off += rows_per_morsel) {
        const std::int64_t len = std::min(rows_per_morsel, rows - off);
        dstd::Vector<Series> piece;
        piece.reserve(cols.size());
        for (const Series& c : cols)
            piece.push_back(c.slice(off, len).materialize());
        largest = std::max<std::uint64_t>(largest, columns_bytes(piece));
        auto wrote = w.write(piece, len);
        if (!wrote) return wrote.error();
    }
    return largest;
}

HashPartitions::HashPartitions(std::size_t parts)
    : bytes_(parts, 0), rows_(parts, 0) {
    paths_.reserve(parts);
    writers_.reserve(parts);
}

dstd::Result<HashPartitions> HashPartitions::make(Dir& dir, std::size_t parts) {
    HashPartitions out(parts);
    for (std::size_t p = 0; p < parts; ++p) {
        out.paths_.push_back(dir.run_path(dir.next_run()));
        auto writer = Writer::open(out.paths_.back());
        if (!writer) return writer.error();
        out.writers_.push_back(std::move(*writer));
    }
    return out;
}

dstd::Result<void> HashPartitions::write(std::size_t p,
                                         const dstd::Vector<Series>& cols,
                                         std::int64_t rows) {
    bytes_[p] += columns_bytes(cols);
    rows_[p] += rows;
    return writers_[p].write(cols, rows);
}

dstd::Result<void> HashPartitions::close() {
    for (Writer& w : writers_) {
        auto closed = w.close();
        if (!closed) return closed;
    }
    return {};
}

Reader::Reader(dstd::ScopedFile is, std::uint64_t size)
    : is_(std::move(is)), size_(size) {}

dstd::Result<dstd::UniquePtr<Reader>> Reader::open(const dstd::String& path) {
    dstd::ScopedFile is(open_file(path, "rb"));
    if (!is)
        return common::make_error<"spill: cannot open run file {}">(
            common::ErrorCode::Io, path);
    const auto sized = dstd::fs::file_size(dstd::PathView(path));
    if (!sized)
        return common::make_error<"spill: cannot stat run file {}">(
            common::ErrorCode::Io, path);
    return dstd::make_unique<Reader>(std::move(is), *sized);
}

Reader::~Reader() = default;

dstd::Result<void> Reader::seek(std::uint64_t pos) {
    if (::fseeko(is_.get(), static_cast<off_t>(pos), SEEK_SET) != 0)
        return common::make_error<"spill: cannot seek in a run file">(
            common::ErrorCode::Io);
    return {};
}

coro::CoroTask<MorselResult> Reader::next(std::int64_t /*max_rows*/) {
    std::int64_t sizes[2] = {0, 0};
    std::size_t got_sizes = 0;
    {
        coro::BlockingRegion blocking;
        got_sizes = std::fread(sizes, 1, sizeof(sizes), is_.get());
    }
    if (got_sizes == 0) co_return dstd::nullopt;
    if (got_sizes != sizeof(sizes))
        co_return common::make_error<"spill: short run read">(
            common::ErrorCode::Io);
    const auto at = static_cast<std::uint64_t>(::ftello(is_.get()));
    if (sizes[0] < 0 || sizes[1] < 0 ||
        static_cast<std::uint64_t>(sizes[0]) > size_ - at)
        co_return common::make_error<
            "spill: corrupt run file (bad morsel size)">(
            common::ErrorCode::Parse);
    const auto stored = static_cast<std::size_t>(sizes[0]);
    const auto raw = static_cast<std::size_t>(sizes[1]);
    const auto grow = [](dstd::UniquePtr<char[]>& buf, std::size_t& cap,
                         std::size_t need) {
        if (need <= cap) return;
        buf = dstd::make_unique_for_overwrite<char[]>(need);
        cap = need;
    };
    std::size_t got = 0;
    if (stored == raw) {
        grow(raw_, raw_cap_, raw);
        coro::BlockingRegion blocking;
        got = std::fread(raw_.get(), 1, raw, is_.get());
    } else {
        if (!codec_) codec_ = dstd::make_unique<Codec>();
        grow(stored_, stored_cap_, stored);
        {
            coro::BlockingRegion blocking;
            got = std::fread(stored_.get(), 1, stored, is_.get());
        }
        if (got != stored)
            co_return common::make_error<"spill: short run read">(
                common::ErrorCode::Io);
        if (ZSTD_getFrameContentSize(stored_.get(), stored) != raw)
            co_return common::make_error<
                "spill: corrupt run file (bad compressed size)">(
                common::ErrorCode::Parse);
        grow(raw_, raw_cap_, raw);
        if (ZSTD_decompressDCtx(codec_->ctx, raw_.get(), raw, stored_.get(),
                                stored) != raw)
            co_return common::make_error<"spill: corrupt compressed morsel">(
                common::ErrorCode::Parse);
    }
    if (got != stored)
        co_return common::make_error<"spill: short run read">(
            common::ErrorCode::Io);
    const auto* p = reinterpret_cast<const std::uint8_t*>(raw_.get());
    const std::uint8_t* pend = p + raw;
    In in{p, pend};
    Morsel m;
    m.rows = get_pod<std::int64_t>(in);
    const std::int32_t ncols = get_pod<std::int32_t>(in);
    if (in.bad || ncols < 0 || static_cast<std::size_t>(ncols) > raw)
        co_return truncated_read();
    m.columns.reserve(static_cast<std::size_t>(ncols));
    for (std::int32_t c = 0; c < ncols; ++c) {
        auto column = get_series(p, pend);
        if (!column) co_return column.error();
        m.columns.push_back(std::move(*column));
    }
    co_return std::move(m);
}

Spool::Spool(std::uint64_t budget, dstd::SharedPtr<Dir> dir)
    : budget_(budget), dir_(std::move(dir)) {}

Spool::~Spool() {
    writer_.reset();
    if (!run_.empty()) dir_->remove_run(run_);
}

dstd::Result<void> Spool::add(dstd::Vector<Series> columns, std::int64_t rows) {
    if (read_) dstd::fatal("spool: a morsel was added after a reader");
    if (!spilling_) {
        const std::size_t bytes = columns_bytes(columns);
        bytes_ += bytes;
        total_bytes_ += bytes;
        Morsel m;
        m.rows = rows;
        m.columns = std::move(columns);
        mem_.push_back(std::move(m));
        if (bytes_ > budget_) return open_run();  // later morsels go to disk
        return {};
    }
    total_bytes_ += columns_bytes(columns);
    offsets_.push_back(writer_->bytes());
    return writer_->write(columns, rows);
}

dstd::Result<void> Spool::open_run() {
    if (!dir_) {
        auto dir = Dir::create();
        if (!dir) return dir.error();
        dir_ = std::move(*dir);
    }
    run_ = dir_->run_path(dir_->next_run());
    auto writer = Writer::open(run_);
    if (!writer) return writer.error();
    writer_ = dstd::make_unique<Writer>(std::move(*writer));
    spilling_ = true;
    return {};
}

dstd::Result<std::uint64_t> Spool::spill() {
    if (read_ || spilling_) return std::uint64_t{0};
    auto opened = open_run();
    if (!opened) return opened.error();
    for (const Morsel& m : mem_) {
        offsets_.push_back(writer_->bytes());
        auto wrote = writer_->write(m.columns, m.rows);
        if (!wrote) return wrote.error();
    }
    mem_.clear();
    const std::uint64_t freed = bytes_;
    bytes_ = 0;
    return freed;
}

dstd::Result<dstd::UniquePtr<Reader>> Spool::open_disk() {
    // The run is closed by the first reader; any number of readers may follow.
    read_ = true;
    if (writer_) {
        auto closed = writer_->close();
        if (!closed) return closed.error();
        writer_.reset();
    }
    if (run_.empty()) return dstd::UniquePtr<Reader>();
    return Reader::open(run_);
}

dstd::Result<dstd::UniquePtr<Cursor>> Spool::reader() {
    auto disk = open_disk();
    if (!disk) return disk.error();
    return dstd::UniquePtr<Cursor>(
        dstd::make_unique<SpoolReader>(&mem_, std::move(*disk)));
}

dstd::Result<dstd::UniquePtr<Cursor>> Spool::reverse_reader() {
    auto disk = open_disk();
    if (!disk) return disk.error();
    return dstd::UniquePtr<Cursor>(dstd::make_unique<SpoolReverseReader>(
        &mem_, std::move(*disk), offsets_));
}

dstd::Result<void> write_agg_run(AggState& state, const dstd::String& path) {
    agg_sort_groups(state);
    dstd::ScopedFile os(open_file(path, "wb"));
    if (!os)
        return common::make_error<"agg spill: cannot open run {}">(
            common::ErrorCode::Io, path);
    const dstd::SharedPtr<common::SpillCharge> charge =
        common::ScopedSpillSubdir::charge_for(dstd::PathView(path));
    const std::int64_t ng = agg_group_count(state);
    for (std::int64_t g = 0; g < ng; ++g) {
        const dstd::String blob =
            agg_serialize(*agg_extract_group(state, g), true);
        if (blob.size() > std::numeric_limits<std::uint32_t>::max())
            dstd::fatal("agg spill: a group state is over 4 GiB");
        const std::uint32_t len = static_cast<std::uint32_t>(blob.size());
        if (std::fwrite(&len, sizeof(len), 1, os.get()) != 1 ||
            std::fwrite(blob.data(), 1, blob.size(), os.get()) != blob.size())
            return common::make_error<"agg spill: cannot write run {}">(
                common::ErrorCode::Io, path);
        if (charge) charge->add(sizeof(len) + blob.size());
    }
    coro::BlockingRegion blocking;
    if (std::fclose(os.release()) != 0)
        return common::make_error<"agg spill: cannot write run {}">(
            common::ErrorCode::Io, path);
    return {};
}

AggRunReader::AggRunReader(dstd::ScopedFile is, std::uint64_t size)
    : is_(std::move(is)), size_(size) {}

dstd::Result<dstd::UniquePtr<AggRunReader>> AggRunReader::open(
    const dstd::String& path) {
    dstd::ScopedFile is(open_file(path, "rb"));
    if (!is)
        return common::make_error<"agg spill: cannot open run {}">(
            common::ErrorCode::Io, path);
    const auto sized = dstd::fs::file_size(dstd::PathView(path));
    if (!sized)
        return common::make_error<"agg spill: cannot stat run {}">(
            common::ErrorCode::Io, path);
    auto reader = dstd::make_unique<AggRunReader>(std::move(is), *sized);
    auto first = reader->advance();
    if (!first) return first.error();
    return reader;
}

dstd::Result<void> AggRunReader::advance() {
    std::uint32_t len = 0;
    const std::size_t got_size = std::fread(&len, 1, sizeof(len), is_.get());
    if (got_size == 0) {
        valid_ = false;
        return {};
    }
    if (got_size != sizeof(len))
        return common::make_error<"agg spill: truncated run">(
            common::ErrorCode::Io);
    pos_ += sizeof(len);
    if (len > size_ - pos_)
        return common::make_error<"agg spill: corrupt run (bad state size)">(
            common::ErrorCode::Parse);
    pos_ += len;
    dstd::String blob(len, '\0');
    if (std::fread(blob.data(), 1, len, is_.get()) != len)
        return common::make_error<"agg spill: truncated run">(
            common::ErrorCode::Io);
    auto state = agg_deserialize(blob, true);
    if (!state) return state.error();
    cur_ = std::move(*state);
    valid_ = true;
    return {};
}

}  // namespace dftracer::utils::dataframe::spill
