// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/internal/buffers.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/decimal_arith.h>
#include <dftracer/utils/dataframe/internal/frame_buffers.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/bits.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#include <cstring>
#include <limits>

namespace dftracer::utils::dataframe {

namespace {

constexpr char MAGIC[4] = {'D', 'F', 'T', 'B'};
constexpr std::uint32_t VERSION = 0;
constexpr std::uint32_t NO_BUFFER = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint8_t FLAG_JSON = 1;
constexpr std::uint8_t FLAG_NAMED = 2;

template <class T>
void put(dstd::String& out, T v) {
    out.append(reinterpret_cast<const char*>(&v), sizeof v);
}

void put_text(dstd::String& out, dstd::StringView text) {
    put<std::uint32_t>(out, static_cast<std::uint32_t>(text.size()));
    out += text;
}

bool is_list_like(TypeId t) {
    return t == TypeId::List || t == TypeId::LargeList || t == TypeId::Map;
}

bool is_text(TypeId t) {
    return t == TypeId::String || t == TypeId::Binary ||
           t == TypeId::LargeString || t == TypeId::LargeBinary;
}

bool has_params_width(TypeId t) {
    return t == TypeId::FixedSizeBinary || t == TypeId::FixedSizeList;
}

std::size_t value_bytes(TypeId t, std::int64_t n, std::int32_t width) {
    if (t == TypeId::FixedSizeBinary)
        return static_cast<std::size_t>(n) * static_cast<std::size_t>(width);
    return buffer_bytes(t, n);
}

std::size_t value_align(TypeId t) {
    if (t == TypeId::FixedSizeBinary || t == TypeId::Bool) return 1;
    const std::size_t w = byte_width(t).value_or(1);
    return w > 16 ? 16 : w;
}

class Writer {
   public:
    dstd::Result<void> column(const Series& in) {
        if (!in.valid())
            return common::make_error(common::ErrorCode::InvalidArgument,
                                      "frame buffers: a column has no data");
        Series s =
            in.encoding() == Encoding::Flat ? in.share() : in.materialize();
        if (!s.valid())
            return common::make_error<
                "frame buffers: cannot flatten a {} column">(
                common::ErrorCode::InvalidArgument, type_name(in.type()));
        const dftu_series& h = *s.handle();
        const TypeId t = s.type();
        const std::int64_t n = s.length();
        const std::int64_t nulls = s.null_count();
        put<std::int32_t>(out_.header, static_cast<std::int32_t>(t));
        put<std::int64_t>(out_.header, n);
        put<std::int64_t>(out_.header, nulls);
        const dstd::StringView name = s.name();
        put<std::uint8_t>(out_.header, static_cast<std::uint8_t>(
                                           (h.json() ? FLAG_JSON : 0) |
                                           (name.empty() ? 0 : FLAG_NAMED)));
        put<std::int32_t>(out_.header,
                          static_cast<std::int32_t>(h.time_unit()));
        put_text(out_.header, h.timezone());
        if (!name.empty()) put_text(out_.header, name);
        if (t == TypeId::Decimal128 || t == TypeId::Decimal256) {
            put<std::int32_t>(out_.header, h.decimal_precision());
            put<std::int32_t>(out_.header, h.decimal_scale());
        }
        if (has_params_width(t)) put<std::int32_t>(out_.header, h.fixed_size());

        const std::size_t vbytes = static_cast<std::size_t>((n + 7) / 8);
        if (nulls > 0) {
            if (h.validity.size() < vbytes) return short_buffer(t);
            add(h.validity.slice(0, vbytes));
        } else {
            put<std::uint32_t>(out_.header, NO_BUFFER);
        }

        if (t == TypeId::FixedSizeList) {
            auto child = range_of(s.child(0), 0, n * h.fixed_size());
            if (!child) return child.error();
            return column(*child);
        }
        if (t == TypeId::Struct) {
            const std::int64_t fields = s.num_children();
            put<std::uint32_t>(out_.header, static_cast<std::uint32_t>(fields));
            for (std::int64_t i = 0; i < fields; ++i) {
                put_text(out_.header, s.field_name(i));
                auto put_field = column(s.child(i));
                if (!put_field) return put_field;
            }
            return {};
        }
        if (is_list_like(t) || is_text(t)) {
            const bool wide = h.wide_offsets();
            std::int64_t base = 0;
            std::int64_t last = 0;
            const std::size_t width =
                wide ? sizeof(std::int64_t) : sizeof(std::int32_t);
            if (n > 0) {
                if (h.offsets.size() < static_cast<std::size_t>(n + 1) * width)
                    return short_buffer(t);
                base = wide ? s.offsets64()[0] : s.offsets()[0];
                last = wide ? s.offsets64()[n] : s.offsets()[n];
            }
            if (wide)
                add_offsets(h.offsets, s.offsets64(), n, base);
            else
                add_offsets(h.offsets, s.offsets(), n, base);
            const std::int64_t count = last - base;
            if (is_text(t)) {
                if (h.data.size() < static_cast<std::size_t>(last))
                    return short_buffer(t);
                add(count == 0 ? BufferView{}
                               : h.data.slice(static_cast<std::size_t>(base),
                                              static_cast<std::size_t>(count)));
                return {};
            }
            auto child = range_of(s.child(0), base, count);
            if (!child) return child.error();
            return column(*child);
        }
        const std::size_t bytes = value_bytes(t, n, h.fixed_size());
        if (h.data.size() < bytes) return short_buffer(t);
        add(h.data.slice(0, bytes));
        return {};
    }

    FrameBuffers& out() { return out_; }

   private:
    void add(BufferView view) {
        put<std::uint32_t>(out_.header,
                           static_cast<std::uint32_t>(out_.buffers.size()));
        out_.buffers.push_back(std::move(view));
    }

    template <class Off>
    void add_offsets(const BufferView& offsets, const Off* offs, std::int64_t n,
                     std::int64_t base) {
        const std::size_t bytes = static_cast<std::size_t>(n + 1) * sizeof(Off);
        // An empty slice can keep one offset that is not 0.
        if (n > 0 && base == 0 && offsets.size() >= bytes) {
            add(offsets.slice(0, bytes));
            return;
        }
        BufferView rebased = new_buffer(bytes);
        auto* dst = reinterpret_cast<Off*>(writable(rebased));
        for (std::int64_t i = 0; i <= n; ++i)
            dst[i] = n > 0 ? static_cast<Off>(offs[i] - base) : Off{0};
        add(std::move(rebased));
    }

    // The `count` values of `child` from `base`, sharing its buffers where a
    // slice can.
    static dstd::Result<Series> range_of(Series child, std::int64_t base,
                                         std::int64_t count) {
        if (base == 0 && count == child.length()) return child;
        if (Series cut = child.slice(base, count); cut.valid()) return cut;
        dstd::Vector<std::int64_t> rows(static_cast<std::size_t>(count));
        for (std::int64_t i = 0; i < count; ++i)
            rows[static_cast<std::size_t>(i)] = base + i;
        Series out = child.take(rows);
        if (!out.valid())
            return common::make_error<
                "frame buffers: cannot gather a {} list "
                "child">(common::ErrorCode::InvalidArgument,
                         type_name(child.type()));
        return out;
    }

    static dstd::Unexpected<dstd::Error> short_buffer(TypeId t) {
        return common::make_error<
            "frame buffers: a {} column holds fewer "
            "bytes than its rows">(common::ErrorCode::InvalidArgument,
                                   type_name(t));
    }

    FrameBuffers out_;
};

class Reader {
   public:
    Reader(dstd::StringView header, dstd::Span<const ReceivedBuffer> buffers)
        : p_(reinterpret_cast<const std::uint8_t*>(header.data())),
          end_(p_ + header.size()),
          buffers_(buffers) {}

    template <class T>
    bool take(T& v) {
        if (static_cast<std::size_t>(end_ - p_) < sizeof v) return false;
        std::memcpy(&v, p_, sizeof v);
        p_ += sizeof v;
        return true;
    }

    bool text(dstd::StringView& v) {
        std::uint32_t bytes = 0;
        if (!take(bytes) || static_cast<std::size_t>(end_ - p_) < bytes)
            return false;
        v = dstd::StringView(reinterpret_cast<const char*>(p_), bytes);
        p_ += bytes;
        return true;
    }

    bool at_end() const { return p_ == end_; }

    dstd::Optional<Series> column(int depth = 0) {
        if (depth > MAX_DEPTH) return dstd::nullopt;
        std::int32_t type = 0;
        std::int64_t n = 0;
        std::int64_t nulls = 0;
        std::uint8_t flags = 0;
        std::int32_t unit = 0;
        dstd::StringView zone;
        dstd::StringView name;
        if (!take(type) || !take(n) || !take(nulls) || !take(flags) ||
            !take(unit) || !text(zone))
            return dstd::nullopt;
        if ((flags & FLAG_NAMED) != 0 && !text(name)) return dstd::nullopt;
        if (type <= static_cast<std::int32_t>(TypeId::Unknown) ||
            type > static_cast<std::int32_t>(TypeId::Map) || n < 0 ||
            n > MAX_LENGTH || nulls < 0 || nulls > n)
            return dstd::nullopt;
        const auto t = static_cast<TypeId>(type);
        auto* col = new dftu_series();
        Series owner{col};
        col->type = t;
        col->encoding = Encoding::Flat;
        col->length = n;
        if (unit < static_cast<std::int32_t>(TimeUnit::Second) ||
            unit > static_cast<std::int32_t>(TimeUnit::Nano))
            return dstd::nullopt;
        const auto time_unit = static_cast<TimeUnit>(unit);
        const bool coarse =
            time_unit == TimeUnit::Second || time_unit == TimeUnit::Milli;
        if ((t == TypeId::Time32 && !coarse) || (t == TypeId::Time64 && coarse))
            return dstd::nullopt;
        if (t == TypeId::Decimal128 || t == TypeId::Decimal256) {
            std::int32_t precision = 0;
            std::int32_t scale = 0;
            if (!take(precision) || !take(scale)) return dstd::nullopt;
            const std::int32_t most = t == TypeId::Decimal128
                                          ? DECIMAL128_MAX_PRECISION
                                          : DECIMAL256_MAX_PRECISION;
            if (precision < 1 || precision > most || scale < -most ||
                scale > most)
                return dstd::nullopt;
            col->set_decimal(precision, scale);
        }
        std::int32_t width = 0;
        if (has_params_width(t)) {
            if (!take(width) || width < 0 ||
                (width > 0 && n > MAX_LENGTH / width))
                return dstd::nullopt;
            col->set_fixed_size(width);
        }
        col->set_json((flags & FLAG_JSON) != 0);
        col->set_time_unit(time_unit);
        col->set_timezone(zone);
        if (!name.empty()) col->name = dstd::SharedString{name};

        std::uint32_t validity = NO_BUFFER;
        if (!take(validity)) return dstd::nullopt;
        if ((validity == NO_BUFFER) != (nulls == 0)) return dstd::nullopt;
        if (validity != NO_BUFFER) {
            auto bits = buffer(validity, static_cast<std::size_t>((n + 7) / 8),
                               1, true);
            if (!bits || set_bits(*bits, n) != n - nulls) return dstd::nullopt;
            col->validity = std::move(*bits);
            col->null_count = nulls;
        }

        if (t == TypeId::FixedSizeList) {
            auto child = column(depth + 1);
            if (!child || child->length() != n * width) return dstd::nullopt;
            col->set_child(adopt_series(child->release()));
            return owner;
        }
        if (t == TypeId::Struct) {
            std::uint32_t fields = 0;
            if (!take(fields)) return dstd::nullopt;
            for (std::uint32_t i = 0; i < fields; ++i) {
                dstd::StringView field;
                if (!text(field)) return dstd::nullopt;
                auto child = column(depth + 1);
                if (!child || child->length() != n) return dstd::nullopt;
                col->nested.push_back(
                    {adopt_series(child->release()), dstd::String(field)});
            }
            return owner;
        }
        if (is_list_like(t) || is_text(t)) {
            const bool wide = t == TypeId::LargeList ||
                              t == TypeId::LargeString ||
                              t == TypeId::LargeBinary;
            std::uint32_t at = 0;
            if (!take(at)) return dstd::nullopt;
            const std::size_t width_bytes =
                wide ? sizeof(std::int64_t) : sizeof(std::int32_t);
            auto offsets =
                buffer(at, static_cast<std::size_t>(n + 1) * width_bytes,
                       width_bytes, true);
            if (!offsets) return dstd::nullopt;
            col->offsets = std::move(*offsets);
            std::int64_t last = 0;
            const bool ordered =
                wide ? ascending(reinterpret_cast<const std::int64_t*>(
                                     col->offsets.data()),
                                 n, last)
                     : ascending(reinterpret_cast<const std::int32_t*>(
                                     col->offsets.data()),
                                 n, last);
            if (!ordered) return dstd::nullopt;
            if (is_text(t)) {
                std::uint32_t data_at = 0;
                if (!take(data_at)) return dstd::nullopt;
                auto data = buffer(data_at, static_cast<std::size_t>(last), 1);
                if (!data) return dstd::nullopt;
                col->data = std::move(*data);
                return owner;
            }
            auto child = column(depth + 1);
            if (!child || child->length() != last) return dstd::nullopt;
            if (t == TypeId::Map &&
                (child->type() != TypeId::Struct || child->num_children() != 2))
                return dstd::nullopt;
            col->set_child(adopt_series(child->release()));
            return owner;
        }
        if (!byte_width(t, width)) return dstd::nullopt;
        std::uint32_t at = 0;
        if (!take(at)) return dstd::nullopt;
        auto data = buffer(at, value_bytes(t, n, width), value_align(t));
        if (!data) return dstd::nullopt;
        col->data = std::move(*data);
        return owner;
    }

   private:
    static constexpr int MAX_DEPTH = 64;
    // Keeps every buffer size product below 2^64 (n * 32 bytes, n * width).
    static constexpr std::int64_t MAX_LENGTH = std::int64_t{1} << 48;

    // Offsets that start at 0 and never go down; `last` is the final one.
    template <class Off>
    static bool ascending(const Off* offs, std::int64_t n, std::int64_t& last) {
        if (offs[0] != 0) return false;
        for (std::int64_t i = 0; i < n; ++i)
            if (offs[i + 1] < offs[i]) return false;
        last = static_cast<std::int64_t>(offs[n]);
        return true;
    }

    // The set bits among the first `n` of `bits`.
    static std::int64_t set_bits(const BufferView& bits, std::int64_t n) {
        const std::size_t whole = static_cast<std::size_t>(n / 8);
        auto count = static_cast<std::int64_t>(
            dstd::bits::popcount_bytes(bits.data(), whole));
        if (const auto tail = static_cast<unsigned>(n % 8); tail != 0)
            count += dstd::bits::popcount_u64(bits.data()[whole] &
                                              ((1U << tail) - 1U));
        return count;
    }

    // `structural` marks the bytes that bound later reads (offsets, validity):
    // those are copied unless the buffer is read-only.
    dstd::Optional<BufferView> buffer(std::uint32_t at, std::size_t size,
                                      std::size_t align,
                                      bool structural = false) {
        if (at >= buffers_.size()) return dstd::nullopt;
        const ReceivedBuffer& b = buffers_[at];
        if (b.size != size) return dstd::nullopt;
        if (size == 0) return new_buffer(0);
        if (b.data == nullptr) return dstd::nullopt;
        if (reinterpret_cast<std::uintptr_t>(b.data) % align == 0 &&
            (b.read_only || !structural))
            return borrow_buffer(b.data, size, b.owner);
        BufferView copy = new_buffer(size);
        std::memcpy(writable(copy), b.data, size);
        return copy;
    }

    const std::uint8_t* p_;
    const std::uint8_t* end_;
    dstd::Span<const ReceivedBuffer> buffers_;
};

}  // namespace

dstd::Result<FrameBuffers> frame_to_buffers(const DataFrame& f) {
    Writer w;
    dstd::String& h = w.out().header;
    h.append(MAGIC, sizeof MAGIC);
    put<std::uint32_t>(h, VERSION);
    put<std::int64_t>(h, f.num_rows());
    put<std::uint32_t>(h, static_cast<std::uint32_t>(f.columns.size()));
    for (std::size_t c = 0; c < f.columns.size(); ++c) {
        put_text(h, f.names[c]);
        auto put_column = w.column(f.columns[c]);
        if (!put_column) return put_column.error();
    }
    const dstd::Span<const dstd::SharedString> index = f.index_names();
    put<std::uint32_t>(h, static_cast<std::uint32_t>(index.size()));
    for (const dstd::SharedString& name : index)
        put_text(h, dstd::StringView(name));
    return std::move(w.out());
}

dstd::Optional<DataFrame> frame_from_buffers(
    dstd::StringView header,
    dstd::Span<const ReceivedBuffer> buffers) noexcept {
    if (header.size() < sizeof MAGIC ||
        std::memcmp(header.data(), MAGIC, sizeof MAGIC) != 0)
        return dstd::nullopt;
    Reader r(header.substr(sizeof MAGIC), buffers);
    std::uint32_t version = 0;
    std::int64_t rows = 0;
    std::uint32_t columns = 0;
    if (!r.take(version) || version != VERSION || !r.take(rows) || rows < 0 ||
        !r.take(columns))
        return dstd::nullopt;
    DataFrame f;
    for (std::uint32_t c = 0; c < columns; ++c) {
        dstd::StringView name;
        if (!r.text(name)) return dstd::nullopt;
        auto column = r.column();
        if (!column || column->length() != rows) return dstd::nullopt;
        f.names.emplace_back(name);
        f.columns.push_back(std::move(*column));
    }
    std::uint32_t index_count = 0;
    if (!r.take(index_count)) return dstd::nullopt;
    dstd::Vector<dstd::StringView> index;
    for (std::uint32_t i = 0; i < index_count; ++i) {
        dstd::StringView name;
        if (!r.text(name)) return dstd::nullopt;
        index.push_back(name);
    }
    if (!r.at_end()) return dstd::nullopt;
    if (index.empty()) return f;
    auto indexed = f.with_index(index);
    if (!indexed) return dstd::nullopt;
    return dstd::Optional<DataFrame>(std::move(*indexed));
}

}  // namespace dftracer::utils::dataframe
