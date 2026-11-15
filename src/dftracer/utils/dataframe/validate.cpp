// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/validate.h>
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace dftracer::utils::dataframe {
namespace {

using common::ErrorCode;
using common::make_error;
using dstd::String;
using dstd::StringView;

constexpr std::size_t VIEW_BYTES = 16;
constexpr std::int32_t VIEW_INLINE_MAX = 12;
/// A column nests a handful of levels in practice; the bound only stops a
/// self-referencing child from recursing without end.
constexpr int MAX_DEPTH = 32;

dstd::Result<void> broken(const String& label, const String& rule) {
    return make_error(ErrorCode::InvalidArgument,
                      dstd::fmt<"{}: {}">(label, rule));
}

// Refuses more rows than a buffer of `width` bytes per row can size, so a
// length no buffer could back fails here instead of overflowing a `length *
// width` and then reading past a buffer the row count promised.
dstd::Result<void> check_rows_fit(const String& label, std::uint64_t rows,
                                  std::uint64_t width) {
    if (width == 0) return {};
    if (rows > std::numeric_limits<std::size_t>::max() / width)
        return broken(label,
                      dstd::fmt<"{} rows of {} bytes do not fit a buffer; the "
                                "length is too large">(rows, width));
    return {};
}

String label_of(StringView name) {
    return name.empty() ? String("column (unnamed)")
                        : dstd::fmt<"column '{}'">(name);
}

bool is_variable_width(TypeId t) noexcept {
    switch (t) {
        case TypeId::String:
        case TypeId::Binary:
        case TypeId::LargeString:
        case TypeId::LargeBinary:
            return true;
        default:
            return false;
    }
}

bool is_list_like(TypeId t) noexcept {
    switch (t) {
        case TypeId::List:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
        case TypeId::Map:
            return true;
        default:
            return false;
    }
}

// Bytes one row of a fixed-width type occupies, or nullopt when the type is
// not fixed width (a variable-width one, or a nested one).
dstd::Optional<std::size_t> row_bytes(const dftu_series& c) noexcept {
    if (is_variable_width(c.type) || is_list_like(c.type)) return dstd::nullopt;
    if (c.type == TypeId::Struct) return dstd::nullopt;
    return byte_width(c.type, c.fixed_size());
}

bool own_null(const dftu_series& c, std::int64_t i) noexcept {
    return c.validity.data() != nullptr &&
           ((c.validity.data()[i >> 3] >> (i & 7)) & 1) == 0;
}

dstd::Result<void> check_validity(const dftu_series& c, const String& label) {
    if (c.length < 0)
        return broken(label, dstd::fmt<"its length {} is negative">(c.length));
    if (c.null_count < 0)
        return broken(label,
                      dstd::fmt<"its null count {} is negative">(c.null_count));
    const std::int64_t need_bits = c.length;
    // A bitmap of near-INT64_MAX rows would overflow the byte count below.
    if (need_bits > std::numeric_limits<std::int64_t>::max() - 8)
        return broken(label,
                      dstd::fmt<"it claims {} rows; the length is too "
                                "large for a validity bitmap">(need_bits));
    const std::int64_t need_bytes = (need_bits + 7) / 8;
    if (c.validity.data() == nullptr) {
        // A CHUNKED column counts the nulls of its chunks and keeps no bitmap
        // of its own; a SELECTION counts its rows through the base.
        if (c.null_count != 0 && !c.is_chunked() &&
            c.encoding != Encoding::Selection)
            return broken(label,
                          dstd::fmt<"it has {} nulls but no validity bitmap">(
                              c.null_count));
        return {};
    }
    if (static_cast<std::int64_t>(c.validity.size()) < need_bytes)
        return broken(
            label,
            dstd::fmt<"its validity bitmap holds {} bytes, but {} rows need "
                      "{}">(c.validity.size(), need_bits, need_bytes));
    if (c.is_chunked() || c.encoding == Encoding::Selection) return {};
    std::int64_t nulls = 0;
    for (std::int64_t i = 0; i < need_bits; ++i) nulls += own_null(c, i);
    if (nulls != c.null_count)
        return broken(
            label,
            dstd::fmt<"its null count is {} but its validity bitmap clears {} "
                      "of {} bits">(c.null_count, nulls, need_bits));
    return {};
}

template <class Off>
dstd::Result<void> check_offsets(const dftu_series& c, const String& label,
                                 StringView where, std::size_t limit) {
    if (c.offsets.data() == nullptr) {
        // Zero rows need no offsets at all: a row's ends are the pair around
        // it, and there is no row.
        if (c.length == 0) return {};
        return broken(label, dstd::fmt<"a {} column has no offset buffer">(
                                 type_name(c.type)));
    }
    const std::size_t entries = static_cast<std::size_t>(c.length) + 1;
    if (auto ok = check_rows_fit(label, entries, sizeof(Off)); !ok) return ok;
    const std::size_t need = entries * sizeof(Off);
    if (c.offsets.size() < need)
        return broken(
            label,
            dstd::fmt<"its offset buffer holds {} bytes, but {} rows need {} "
                      "entries of {} bytes">(c.offsets.size(), c.length,
                                             entries, sizeof(Off)));
    const auto* off = reinterpret_cast<const Off*>(c.offsets.data());
    if (static_cast<std::int64_t>(off[0]) < 0)
        return broken(label, dstd::fmt<"its first offset is {}">(
                                 static_cast<std::int64_t>(off[0])));
    for (std::size_t i = 0; i < entries; ++i) {
        const auto at = static_cast<std::int64_t>(off[i]);
        if (at < 0) return broken(label, dstd::fmt<"offset {} is {}">(i, at));
        if (i > 0 && at < static_cast<std::int64_t>(off[i - 1]))
            return broken(
                label, dstd::fmt<"offset {} is {} but offset {} before it is "
                                 "{}">(i, at, i - 1,
                                       static_cast<std::int64_t>(off[i - 1])));
    }
    const auto last = static_cast<std::size_t>(off[entries - 1]);
    if (last > limit)
        return broken(label,
                      dstd::fmt<"its last offset is {} but its {} holds {}">(
                          last, where, limit));
    return {};
}

dstd::Result<void> check_var_width_data(const dftu_series& c,
                                        const String& label) {
    if (c.wide_offsets())
        return check_offsets<std::int64_t>(c, label, "data", c.data.size());
    return check_offsets<std::int32_t>(c, label, "data", c.data.size());
}

dstd::Result<void> check_views(const dftu_series& c, const String& label) {
    if (auto ok = check_rows_fit(label, static_cast<std::uint64_t>(c.length),
                                 VIEW_BYTES);
        !ok)
        return ok;
    const std::size_t need = static_cast<std::size_t>(c.length) * VIEW_BYTES;
    if (c.data.size() < need)
        return broken(
            label,
            dstd::fmt<"its view buffer holds {} bytes, but {} rows need {}">(
                c.data.size(), c.length, need));
    const std::uint8_t* views = c.data.data();
    const std::size_t buffers = c.blobs ? c.blobs->size() : 0;
    for (std::int64_t i = 0; i < c.length; ++i) {
        if ((c.validity.data() != nullptr) && own_null(c, i)) continue;
        const std::uint8_t* view =
            views + static_cast<std::size_t>(i) * VIEW_BYTES;
        std::int32_t size = 0;
        std::memcpy(&size, view, sizeof(size));
        if (size < 0)
            return broken(
                label, dstd::fmt<"the view of row {} has length {}">(i, size));
        if (size <= VIEW_INLINE_MAX) continue;
        std::int32_t index = 0;
        std::int32_t offset = 0;
        std::memcpy(&index, view + 8, sizeof(index));
        std::memcpy(&offset, view + 12, sizeof(offset));
        if (index < 0 || static_cast<std::size_t>(index) >= buffers)
            return broken(
                label,
                dstd::fmt<"the view of row {} names buffer {}, and it has "
                          "{}">(i, index, buffers));
        const std::size_t have =
            (*c.blobs)[static_cast<std::size_t>(index)].size();
        if (offset < 0 || static_cast<std::size_t>(offset) > have ||
            static_cast<std::size_t>(size) >
                have - static_cast<std::size_t>(offset))
            return broken(
                label,
                dstd::fmt<"the view of row {} reads {} bytes at {} of a {} "
                          "byte buffer">(i, size, offset, have));
    }
    return {};
}

dstd::Result<void> check_column(const dftu_series& c, const String& label,
                                int depth);

dstd::Result<void> check_nested(const dftu_series& c, const String& label,
                                int depth) {
    if (depth >= MAX_DEPTH)
        return broken(label, String("its nesting is deeper than 32 levels"));
    const bool fields = c.has_fields();
    for (const dftu_nested& n : c.nested) {
        if (!n.series)
            return broken(label, fields ? String("it has an empty field")
                                        : String("it has an empty child"));
    }
    if (fields) {
        for (const dftu_nested& n : c.nested) {
            const String child =
                n.name.empty() ? label
                               : dstd::fmt<"{} field '{}'">(label, n.name);
            if (n.series->length != c.length)
                return broken(
                    child,
                    dstd::fmt<"its length is {} but the struct holding "
                              "it has {} rows">(n.series->length, c.length));
            if (auto ok = check_column(*n.series, child, depth + 1); !ok)
                return ok;
        }
        return {};
    }
    if (is_list_like(c.type)) {
        if (!c.child())
            return broken(label, dstd::fmt<"a {} column has no values column">(
                                     type_name(c.type)));
        const dftu_series& values = *c.child();
        const String child = dstd::fmt<"{} values">(label);
        if (c.type == TypeId::FixedSizeList) {
            if (auto ok =
                    check_rows_fit(label, static_cast<std::uint64_t>(c.length),
                                   static_cast<std::uint64_t>(c.fixed_size()));
                !ok)
                return ok;
            const std::uint64_t want =
                static_cast<std::uint64_t>(c.length) *
                static_cast<std::uint64_t>(c.fixed_size());
            if (values.length < 0 ||
                static_cast<std::uint64_t>(values.length) != want)
                return broken(
                    child, dstd::fmt<"its length is {} but {} rows of fixed "
                                     "size {} need {}">(values.length, c.length,
                                                        c.fixed_size(), want));
        } else {
            const auto limit = static_cast<std::size_t>(values.length);
            auto ok =
                c.wide_offsets()
                    ? check_offsets<std::int64_t>(c, label, "values", limit)
                    : check_offsets<std::int32_t>(c, label, "values", limit);
            if (!ok) return ok;
        }
        return check_column(values, child, depth + 1);
    }
    for (const dftu_nested& n : c.nested)
        if (auto ok = check_column(*n.series, label, depth + 1); !ok) return ok;
    return {};
}

dstd::Result<void> check_column(const dftu_series& c, const String& label,
                                int depth) {
    if (c.type == TypeId::Unknown)
        return broken(label, String("its type is unknown"));
    if (auto ok = check_validity(c, label); !ok) return ok;
    // The widest layout any encoding needs is 16 bytes per row (a VIEW); a
    // length above that ceiling no buffer could back, and every size below is
    // a product or a loop over it.
    if (auto ok =
            check_rows_fit(label, static_cast<std::uint64_t>(c.length), 16);
        !ok)
        return ok;
    switch (c.encoding) {
        case Encoding::Flat:
            if (c.type == TypeId::Struct || is_list_like(c.type))
                return check_nested(c, label, depth);
            if (is_variable_width(c.type))
                return check_var_width_data(c, label);
            if (const auto width = row_bytes(c)) {
                if (auto ok = check_rows_fit(
                        label, static_cast<std::uint64_t>(c.length), *width);
                    !ok)
                    return ok;
                const auto need =
                    c.type == TypeId::Bool
                        ? static_cast<std::size_t>((c.length + 7) / 8)
                        : static_cast<std::size_t>(c.length) * (*width);
                if (c.data.size() < need)
                    return broken(
                        label,
                        dstd::fmt<"its data buffer holds {} bytes, but {} rows "
                                  "of {} need {}">(c.data.size(), c.length,
                                                   type_name(c.type), need));
            }
            return {};
        case Encoding::Constant: {
            const auto width = row_bytes(c);
            if (width && c.data.size() < *width)
                return broken(
                    label,
                    dstd::fmt<"its constant buffer holds {} bytes, but one {} "
                              "needs {}">(c.data.size(), type_name(c.type),
                                          *width));
            return {};
        }
        case Encoding::View:
            return check_views(c, label);
        case Encoding::Dictionary: {
            if (!c.child())
                return broken(label,
                              dstd::fmt<"a dictionary column of {} has no "
                                        "values column">(type_name(c.type)));
            const std::size_t need =
                static_cast<std::size_t>(c.length) * sizeof(std::int32_t);
            if (auto ok =
                    check_rows_fit(label, static_cast<std::uint64_t>(c.length),
                                   sizeof(std::int32_t));
                !ok)
                return ok;
            if (c.data.size() < need)
                return broken(
                    label,
                    dstd::fmt<"its code buffer holds {} bytes, but {} rows "
                              "need {}">(c.data.size(), c.length, need));
            const auto* codes =
                reinterpret_cast<const std::int32_t*>(c.data.data());
            const std::int64_t values = c.child()->length;
            for (std::int64_t i = 0; i < c.length; ++i) {
                if (own_null(c, i)) continue;
                const std::int32_t code = codes[i];
                if (code < 0 || code >= values)
                    return broken(
                        label, dstd::fmt<"code {} of row {} is outside its {} "
                                         "values">(code, i, values));
            }
            return check_column(*c.child(), dstd::fmt<"{} values">(label),
                                depth + 1);
        }
        case Encoding::Selection: {
            if (!c.child())
                return broken(
                    label,
                    dstd::fmt<"a selection column of {} has no base column">(
                        type_name(c.type)));
            const std::size_t need =
                static_cast<std::size_t>(c.length) * sizeof(std::int64_t);
            if (auto ok =
                    check_rows_fit(label, static_cast<std::uint64_t>(c.length),
                                   sizeof(std::int64_t));
                !ok)
                return ok;
            if (c.data.size() < need)
                return broken(
                    label,
                    dstd::fmt<"its index buffer holds {} bytes, but {} rows "
                              "need {}">(c.data.size(), c.length, need));
            const auto* index =
                reinterpret_cast<const std::int64_t*>(c.data.data());
            const std::int64_t base = c.child()->length;
            for (std::int64_t i = 0; i < c.length; ++i) {
                // A negative index is the outer-fill sentinel, not an offset.
                if (index[i] < 0) continue;
                if (index[i] >= base)
                    return broken(
                        label,
                        dstd::fmt<"index {} of row {} is outside its {} base "
                                  "rows">(index[i], i, base));
            }
            return check_column(*c.child(), dstd::fmt<"{} base">(label),
                                depth + 1);
        }
        case Encoding::Chunked: {
            if (c.nested.empty())
                return broken(label, String("a chunked column has no chunks"));
            const std::size_t entries = c.nested.size() + 1;
            if (auto ok = check_rows_fit(label, entries, sizeof(std::int64_t));
                !ok)
                return ok;
            const std::size_t need = entries * sizeof(std::int64_t);
            if (c.data.size() < need)
                return broken(
                    label,
                    dstd::fmt<"its chunk start buffer holds {} bytes, but {} "
                              "chunks need {}">(c.data.size(), c.nested.size(),
                                                need));
            const auto* starts =
                reinterpret_cast<const std::int64_t*>(c.data.data());
            if (starts[0] != 0)
                return broken(label, dstd::fmt<"its first chunk starts at {}">(
                                         starts[0]));
            std::int64_t total = 0;
            for (std::size_t k = 0; k < c.nested.size(); ++k) {
                if (!c.nested[k].series)
                    return broken(dstd::fmt<"{} chunk {}">(label, k),
                                  String("it is empty"));
                const dftu_series& chunk = *c.nested[k].series;
                const String child = dstd::fmt<"{} chunk {}">(label, k);
                if (starts[k + 1] < starts[k])
                    return broken(
                        child,
                        dstd::fmt<
                            "it starts at {} but the chunk before it ends "
                            "at {}">(starts[k + 1], starts[k]));
                if (starts[k + 1] - starts[k] != chunk.length)
                    return broken(
                        child,
                        dstd::fmt<"it has {} rows but its start says {}">(
                            chunk.length, starts[k + 1] - starts[k]));
                if (auto ok = check_column(chunk, child, depth + 1); !ok)
                    return ok;
                total += chunk.length;
            }
            if (total != c.length)
                return broken(
                    label,
                    dstd::fmt<"its chunks hold {} rows in all, but its length "
                              "is {}">(total, c.length));
            return {};
        }
    }
    return broken(label, String("it has an unknown encoding"));
}

}  // namespace

bool plugin_check_enabled() {
    static const bool enabled = [] {
        const auto raw = common::Env::get<dstd::StringView>(
            common::constants::plugins::CHECK_ENV);
        if (!raw) return false;
        return !(*raw == "0" || dstd::ascii_iequals(*raw, "false") ||
                 dstd::ascii_iequals(*raw, "off") ||
                 dstd::ascii_iequals(*raw, "no"));
    }();
    return enabled;
}

dstd::Result<void> validate(const dftu_series& col) {
    return check_column(col, label_of(col.name.view()), 0);
}

dstd::Result<void> validate(const Series& col) {
    if (!col.valid())
        return make_error(ErrorCode::InvalidArgument,
                          StringView("column (unnamed): it is null"));
    return validate(*col.handle());
}

dstd::Result<void> validate(const DataFrame& frame) {
    if (frame.names.size() != frame.columns.size())
        return broken(String("frame"),
                      dstd::fmt<"it has {} names for {} columns">(
                          frame.names.size(), frame.columns.size()));
    for (std::size_t i = 0; i < frame.names.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j)
            if (frame.names[i].view() == frame.names[j].view())
                return broken(
                    label_of(frame.names[i].view()),
                    String("two columns of the frame carry this name"));
    }
    for (std::size_t i = 0; i < frame.columns.size(); ++i) {
        const String label = label_of(frame.names[i].view());
        if (!frame.columns[i].valid())
            return broken(label, String("the column is null"));
        const dftu_series& col = *frame.columns[i].handle();
        if (auto ok = check_column(col, label, 0); !ok) return ok;
        if (col.length != frame.num_rows())
            return broken(label,
                          dstd::fmt<"it has {} rows but the frame has {}">(
                              col.length, frame.num_rows()));
    }
    return {};
}

dstd::Result<void> validate_if_checking(const dftu_series& col) {
    if (!plugin_check_enabled()) return {};
    return validate(col);
}

dstd::Result<void> validate_if_checking(const Series& col) {
    if (!plugin_check_enabled()) return {};
    return validate(col);
}

dstd::Result<void> validate_if_checking(const DataFrame& frame) {
    if (!plugin_check_enabled()) return {};
    return validate(frame);
}

namespace {

thread_local String t_op_check_error;

}  // namespace

StringView op_check_error() { return StringView(t_op_check_error); }

void record_op_check_error(StringView why) { t_op_check_error.assign(why); }

}  // namespace dftracer::utils::dataframe
