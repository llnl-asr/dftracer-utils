// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/table/table_decode.h>
#include <dftracer/utils/trace/table/table_header.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace dftracer::utils::trace::table::detail {

namespace {

using common::ErrorCode;
using dataframe::Series;
using dataframe::TypeId;
using detail::Field;
using detail::Kind;
using fileio::compress::GzipDecode;
using fileio::compress::GzipMemberDecompressor;

constexpr std::size_t MIN_PART_BYTES = std::size_t{256} << 10;
// Deflate cannot expand a byte past about 1032 bytes; a member that claims more
// is corrupt or forged.
constexpr std::size_t MAX_RATIO = 1032;
constexpr std::size_t RATIO_SLACK = std::size_t{1} << 20;

}  // namespace

Buffer Buffer::with_capacity(std::size_t n) {
    Buffer b;
    b.data = dstd::make_unique_for_overwrite<unsigned char[]>(n + PADDING);
    b.capacity = n;
    return b;
}

void Buffer::reserve(std::size_t n) {
    if (n <= capacity) return;
    Buffer bigger = with_capacity(n);
    if (size > 0) std::memcpy(bigger.data.get(), data.get(), size);
    bigger.size = size;
    *this = std::move(bigger);
}

dstd::Result<Buffer> read_whole(dstd::StringView path) {
    auto file = dstd::File::open(dstd::PathView(path), {});
    if (!file) return file.error();
    auto size = file->size();
    if (!size) return size.error();
    Buffer b = Buffer::with_capacity(static_cast<std::size_t>(*size));
    if (*size > 0) {
        auto read = file->read_exact_at(
            0, dstd::Span<std::uint8_t>(b.data.get(), b.capacity));
        if (!read) return read.error();
    }
    b.size = b.capacity;
    return b;
}

bool has_gzip_magic(const unsigned char* bytes, std::size_t size) {
    return size >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b;
}

namespace {

std::uint32_t trailer_size(const unsigned char* end) {
    return static_cast<std::uint32_t>(end[-4]) |
           static_cast<std::uint32_t>(end[-3]) << 8 |
           static_cast<std::uint32_t>(end[-2]) << 16 |
           static_cast<std::uint32_t>(end[-1]) << 24;
}

dstd::Error corrupt() {
    return dstd::Error(ErrorCode::Compression,
                       "gzip data is corrupt or cut short");
}

// Members decode one after another into a buffer that grows.
dstd::Result<Buffer> inflate_in_order(const Buffer& comp, InflateStats& stats,
                                      std::size_t max_bytes) {
    const std::size_t limit =
        std::min(comp.size * MAX_RATIO + RATIO_SLACK, max_bytes);
    GzipMemberDecompressor dec;
    if (!dec.valid())
        return common::make_error(ErrorCode::Compression,
                                  "failed to allocate a gzip decompressor");
    Buffer out = Buffer::with_capacity(std::min(
        std::max<std::size_t>(comp.size * 4, std::size_t{1} << 20), limit));
    std::size_t at = 0;
    while (at < comp.size) {
        fileio::compress::DecompressResult res{};
        for (;;) {
            const GzipDecode status = dec.decompress_status(
                comp.data.get() + at, comp.size - at, out.data.get() + out.size,
                out.capacity - out.size, res);
            if (status == GzipDecode::Ok) break;
            if (status != GzipDecode::InsufficientSpace) return corrupt();
            if (out.capacity >= limit) {
                if (limit == max_bytes) {
                    stats.exceeded = true;
                    return Buffer{};
                }
                return common::make_error(
                    ErrorCode::Compression,
                    "gzip data inflates past the deflate maximum ratio");
            }
            Buffer bigger =
                Buffer::with_capacity(std::min(out.capacity * 2, limit));
            std::memcpy(bigger.data.get(), out.data.get(), out.size);
            bigger.size = out.size;
            out = std::move(bigger);
        }
        out.size += res.out_bytes;
        at += res.in_bytes;
    }
    return out;
}

dstd::Result<Buffer> in_order(const Buffer& comp, InflateStats& stats,
                              std::size_t max_bytes) {
    stats.inflated_in_order = true;
    return inflate_in_order(comp, stats, max_bytes);
}

}  // namespace

// A writer that cuts members at line ends leaves a gzip header at each member
// start, and the member's size in its trailer. The members decode in parallel
// from those marks; any mark that is not a member, or a member that ends
// elsewhere, sends the file to the in-order decode.
dstd::Result<Buffer> inflate_gzip(const Buffer& comp, InflateStats& stats,
                                  std::size_t max_bytes) {
    dstd::Vector<std::size_t> starts;
    starts.push_back(0);
    const unsigned char* base = comp.data.get();
    for (std::size_t i = 1; i + 4 <= comp.size;) {
        const void* hit = std::memchr(base + i, 0x1f, comp.size - 3 - i);
        if (hit == nullptr) break;
        i = static_cast<std::size_t>(static_cast<const unsigned char*>(hit) -
                                     base);
        if (base[i + 1] == 0x8b && base[i + 2] == 0x08 && base[i + 3] == 0)
            starts.push_back(i);
        ++i;
    }
    const std::size_t members = starts.size();
    dstd::Vector<std::size_t> in_end(members);
    dstd::Vector<std::size_t> out_at(members + 1);
    for (std::size_t m = 0; m < members; ++m) {
        in_end[m] = m + 1 < members ? starts[m + 1] : comp.size;
        const std::size_t in_size = in_end[m] - starts[m];
        const std::size_t claimed = trailer_size(base + in_end[m]);
        if (in_size < 18 || claimed > in_size * MAX_RATIO + 64)
            return in_order(comp, stats, max_bytes);
        out_at[m + 1] = out_at[m] + claimed;
        if (out_at[m + 1] > max_bytes) {
            stats.exceeded = true;
            return Buffer{};
        }
    }
    Buffer out = Buffer::with_capacity(out_at[members]);
    out.size = out_at[members];
    dstd::Vector<std::uint8_t> good(members, 0);
    auto ran = default_runtime().parallel_for(
        static_cast<std::int64_t>(members), 1,
        [&](std::int64_t first, std::int64_t last) {
            GzipMemberDecompressor dec;
            if (!dec.valid()) return;
            for (std::int64_t m = first; m < last; ++m) {
                const auto k = static_cast<std::size_t>(m);
                const std::size_t want = out_at[k + 1] - out_at[k];
                fileio::compress::DecompressResult res{};
                const GzipDecode status = dec.decompress_status(
                    base + starts[k], in_end[k] - starts[k],
                    out.data.get() + out_at[k], want, res);
                good[k] = status == GzipDecode::Ok && res.out_bytes == want &&
                          res.in_bytes == in_end[k] - starts[k];
            }
        });
    if (!ran) return ran.error();
    for (const std::uint8_t g : good)
        if (!g) return in_order(comp, stats, max_bytes);
    stats.members_in_parallel = members;
    return out;
}

namespace {

bool is_blank(dstd::StringView line) {
    for (const char c : line)
        if (c != ' ' && c != '\t' && c != '\r') return false;
    return true;
}

bool parse_u64(dstd::StringView text, std::uint64_t& out) {
    if (text.empty()) return false;
    std::uint64_t n = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (n > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            return false;
        n = n * 10 + digit;
    }
    out = n;
    return true;
}

// Calls fn(line) for each non-blank line of `range`, each cut at '\n'. Stops
// at the first fn that returns false.
template <class Fn>
bool for_each_line(dstd::StringView range, Fn&& fn) {
    while (!range.empty()) {
        const std::size_t nl = range.find('\n');
        const dstd::StringView line = range.substr(0, nl);
        if (!is_blank(line) && !fn(line)) return false;
        range = nl == dstd::StringView::npos ? dstd::StringView()
                                             : range.substr(nl + 1);
    }
    return true;
}

struct Column {
    dstd::Vector<std::uint64_t> words;  ///< one per row, for the numeric kinds
    dstd::Vector<std::uint8_t> valid;   ///< one per row, 1 for a present cell
};

// A cell's size is known only once it is parsed, so each part collects its
// cells here and the column buffer is sized and filled from the parts after.
struct StringPart {
    dstd::String bytes;
    dstd::Vector<std::uint32_t> sizes;
};

struct Chunk {
    dstd::StringView range;
    std::size_t first_row = 0;
    std::size_t rows = 0;
    dstd::Optional<dstd::Error> error;
    dstd::Vector<StringPart> parts;  ///< one per String or Raw wanted field
};

constexpr double NAN_VALUE = std::bit_cast<double>(0x7ff8000000000000ull);
constexpr std::size_t NOT_WANTED = static_cast<std::size_t>(-1);

class RowParser {
   public:
    RowParser(const Header& header, dstd::Span<const std::size_t> wanted,
              dstd::Span<const std::size_t> position,
              dstd::Vector<Column>& columns,
              dstd::Span<const std::size_t> part_of, dstd::StringView padded)
        : header_(header),
          wanted_(wanted),
          position_(position),
          columns_(columns),
          part_of_(part_of),
          limit_(padded.data() + padded.size()),
          stamp_(header.fields.size(), 0) {
        for (std::size_t i = 0; i < header.fields.size(); ++i)
            order_.push_back(i);
        std::sort(order_.begin(), order_.end(),
                  [&](std::size_t a, std::size_t b) {
                      return dstd::StringView(header_.fields[a].name) <
                             dstd::StringView(header_.fields[b].name);
                  });
    }

    void run(Chunk& chunk) {
        chunk_ = &chunk;
        for (std::size_t p = 0; p < chunk.parts.size(); ++p)
            chunk.parts[p].sizes.reserve(chunk.rows);
        std::size_t row = chunk.first_row;
        const std::size_t stop = chunk.first_row + chunk.rows;
        for_each_line(chunk.range, [&](dstd::StringView line) {
            if (row == stop) {
                chunk.error.emplace(ErrorCode::Parse,
                                    "more rows than were counted");
                return false;
            }
            if (!parse_row(line, row)) return false;
            ++row;
            return true;
        });
    }

   private:
    bool fail(std::size_t row, dstd::StringView why,
              dstd::StringView field = {}) {
        if (field.empty())
            chunk_->error = dstd::Error::from_format<"row {}: {}">(
                ErrorCode::Parse, row, why);
        else
            chunk_->error = dstd::Error::from_format<"row {}, field {}: {}">(
                ErrorCode::Parse, row, field, why);
        return false;
    }

    std::size_t find_field(dstd::StringView key) const {
        const auto it = std::lower_bound(
            order_.begin(), order_.end(), key,
            [&](std::size_t a, dstd::StringView k) {
                return dstd::StringView(header_.fields[a].name) < k;
            });
        if (it == order_.end() ||
            dstd::StringView(header_.fields[*it].name) != key)
            return header_.fields.size();
        return *it;
    }

    void put_part(std::size_t pos, std::size_t row, dstd::StringView text) {
        StringPart& part = chunk_->parts[part_of_[pos]];
        part.bytes.append(text.data(), text.size());
        part.sizes.push_back(static_cast<std::uint32_t>(text.size()));
        columns_[pos].valid[row] = 1;
    }

    void put_null(std::size_t pos, std::size_t row) {
        if (part_of_[pos] != NOT_WANTED)
            chunk_->parts[part_of_[pos]].sizes.push_back(0);
        else
            columns_[pos].words[row] = 0;
        columns_[pos].valid[row] = 0;
    }

    bool read_cell(std::size_t col, std::size_t pos, std::size_t row,
                   json::Value& v) {
        const Field& f = header_.fields[col];
        const dstd::StringView name(f.name);
        const auto type = v.type();
        if (type == json::Value::Type::Null) {
            // type() also reports Null for text that is not JSON.
            if (v.raw() != "null") return fail(row, "not a JSON value", name);
            if (!f.nulls_ok)
                return fail(row, "null in a dtype with no nulls", name);
            put_null(pos, row);
            return true;
        }
        Column& c = columns_[pos];
        switch (f.kind) {
            case Kind::Int: {
                const auto n = v.as_int();
                if (!n) return fail(row, "not an integer", name);
                if (f.bits < 64) {
                    const std::int64_t lim = std::int64_t{1} << (f.bits - 1);
                    if (*n < -lim || *n >= lim)
                        return fail(row, "outside the integer dtype", name);
                }
                c.words[row] = static_cast<std::uint64_t>(*n);
                break;
            }
            case Kind::UInt: {
                const dstd::StringView text = v.raw();
                std::uint64_t n = 0;
                if (!parse_u64(text, n))
                    return fail(row, "not an unsigned integer", name);
                if (f.bits < 64 && n >> f.bits != 0)
                    return fail(row, "outside the integer dtype", name);
                c.words[row] = n;
                break;
            }
            case Kind::Float: {
                double d = 0;
                if (type == json::Value::Type::String) {
                    const auto s = v.as_string();
                    if (s && *s == "NaN")
                        d = NAN_VALUE;
                    else if (s && *s == "Infinity")
                        d = std::numeric_limits<double>::infinity();
                    else if (s && *s == "-Infinity")
                        d = -std::numeric_limits<double>::infinity();
                    else
                        return fail(row, "not a float", name);
                } else {
                    const auto n = v.as_double();
                    if (!n) return fail(row, "not a float", name);
                    d = *n;
                }
                c.words[row] = std::bit_cast<std::uint64_t>(d);
                break;
            }
            case Kind::Bool: {
                const auto b = v.as_bool();
                if (!b) return fail(row, "not a boolean", name);
                c.words[row] = *b ? 1 : 0;
                break;
            }
            case Kind::String: {
                const auto s = v.as_string();
                if (!s) return fail(row, "not a string", name);
                put_part(pos, row, *s);
                return true;
            }
            case Kind::Raw:
                put_part(pos, row, v.raw());
                return true;
        }
        c.valid[row] = 1;
        return true;
    }

    bool parse_row(dstd::StringView line, std::size_t row) {
        auto root = doc_.parse_in_place(
            line, static_cast<std::size_t>(limit_ - line.data()));
        if (!root) return fail(row, "not valid JSON");
        auto args = root->find("args");
        const std::size_t nfields = header_.fields.size();
        if (!args) return fail(row, "no args");
        const auto stamp = static_cast<std::uint32_t>(++tick_);
        std::size_t hits = 0;
        std::size_t expect = 0;
        bool ok = true;
        args->for_each_member([&](dstd::StringView key, json::Value v) {
            if (!ok) return;
            std::size_t col = expect;
            if (col >= nfields ||
                dstd::StringView(header_.fields[col].name) != key)
                col = find_field(key);
            if (col >= nfields) return;
            expect = col + 1;
            const std::size_t pos = position_[col];
            if (pos == NOT_WANTED) return;
            if (stamp_[col] == stamp) return;
            stamp_[col] = stamp;
            ++hits;
            ok = read_cell(col, pos, row, v);
        });
        if (!ok) return false;
        if (hits != wanted_.size()) {
            for (const std::size_t i : wanted_)
                if (stamp_[i] != stamp)
                    return fail(row, "the row misses the field",
                                dstd::StringView(header_.fields[i].name));
        }
        return true;
    }

    const Header& header_;
    dstd::Span<const std::size_t> wanted_;
    dstd::Span<const std::size_t> position_;
    dstd::Vector<Column>& columns_;
    dstd::Span<const std::size_t> part_of_;
    const char* limit_;
    json::Document doc_;
    dstd::Vector<std::uint32_t> stamp_;
    dstd::Vector<std::size_t> order_;
    std::uint32_t tick_ = 0;
    Chunk* chunk_ = nullptr;
};

void pack_bits(const std::uint8_t* flags, std::size_t n, std::uint8_t* out) {
    std::memset(out, 0, (n + 7) / 8);
    for (std::size_t i = 0; i < n; ++i)
        if (flags[i]) out[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
}

TypeId type_of(Kind kind) {
    switch (kind) {
        case Kind::Int:
            return TypeId::Int64;
        case Kind::UInt:
            return TypeId::Uint64;
        case Kind::Float:
            return TypeId::Float64;
        case Kind::Bool:
            return TypeId::Bool;
        default:
            return TypeId::String;
    }
}

}  // namespace

dstd::Result<Decoded> decode_lines(const Header& header,
                                   dstd::Span<const std::size_t> wanted,
                                   dstd::StringView range, std::size_t parts) {
    const std::size_t nfields = header.fields.size();
    const std::size_t ncols = wanted.size();
    dstd::Vector<std::size_t> position(nfields, NOT_WANTED);
    for (std::size_t k = 0; k < ncols; ++k) {
        if (wanted[k] >= nfields)
            return common::make_error(ErrorCode::InvalidArgument,
                                      "a wanted field is not in the header");
        position[wanted[k]] = k;
    }

    // Parts end at line ends, and are parsed in parallel.
    const char* body = range.data();
    const char* end = body + range.size();
    const std::size_t body_size = range.size();
    Runtime& rt = default_runtime();
    std::size_t nparts = parts;
    if (nparts == 0)
        nparts = std::clamp<std::size_t>(rt.threads() * 4, 1,
                                         body_size / MIN_PART_BYTES + 1);
    dstd::Vector<Chunk> chunks(nparts);
    {
        const char* at = body;
        for (std::size_t i = 0; i < nparts; ++i) {
            const char* next = end;
            if (i + 1 < nparts) {
                const char* guess = body + body_size / nparts * (i + 1);
                if (guess < at) guess = at;
                if (guess > body && guess[-1] != '\n') {
                    const void* nl = std::memchr(
                        guess, '\n', static_cast<std::size_t>(end - guess));
                    guess = nl ? static_cast<const char*>(nl) + 1 : end;
                }
                next = guess;
            }
            chunks[i].range =
                dstd::StringView(at, static_cast<std::size_t>(next - at));
            at = next;
        }
    }
    auto counted =
        rt.parallel_for(static_cast<std::int64_t>(nparts), 1,
                        [&](std::int64_t first, std::int64_t last) {
                            for (std::int64_t i = first; i < last; ++i) {
                                Chunk& c = chunks[static_cast<std::size_t>(i)];
                                std::size_t n = 0;
                                for_each_line(c.range, [&](dstd::StringView) {
                                    ++n;
                                    return true;
                                });
                                c.rows = n;
                            }
                        });
    if (!counted) return counted.error();
    std::size_t total = 0;
    for (Chunk& c : chunks) {
        c.first_row = total;
        total += c.rows;
    }
    const std::size_t nrows = total;

    dstd::Vector<Column> columns(ncols);
    dstd::Vector<std::size_t> part_of(ncols, NOT_WANTED);
    std::size_t nparts_strings = 0;
    for (std::size_t k = 0; k < ncols; ++k) {
        columns[k].valid.resize(nrows);
        if (type_of(header.fields[wanted[k]].kind) == TypeId::String)
            part_of[k] = nparts_strings++;
        else
            columns[k].words.resize(nrows);
    }
    for (Chunk& c : chunks) c.parts.resize(nparts_strings);

    const dstd::StringView padded(body, body_size + PADDING);
    auto parsed =
        rt.parallel_for(static_cast<std::int64_t>(nparts), 1,
                        [&](std::int64_t first, std::int64_t last) {
                            RowParser parser(header, wanted, position, columns,
                                             part_of, padded);
                            for (std::int64_t i = first; i < last; ++i)
                                parser.run(chunks[static_cast<std::size_t>(i)]);
                        });
    if (!parsed) return parsed.error();
    for (Chunk& c : chunks)
        if (c.error) return *c.error;

    Decoded out;
    out.rows = nrows;
    out.columns.resize(ncols);
    dstd::Vector<dstd::Optional<dstd::Error>> errors(ncols);
    auto built = rt.parallel_for(
        static_cast<std::int64_t>(ncols), 1,
        [&](std::int64_t first, std::int64_t last) {
            for (std::int64_t k = first; k < last; ++k) {
                const auto i = static_cast<std::size_t>(k);
                Column& c = columns[i];
                const TypeId type = type_of(header.fields[wanted[i]].kind);
                dstd::Vector<std::uint8_t> bits((nrows + 7) / 8);
                bool any_null = false;
                for (std::size_t r = 0; r < nrows && !any_null; ++r)
                    any_null = c.valid[r] == 0;
                const std::uint8_t* validity = nullptr;
                if (any_null) {
                    pack_bits(c.valid.data(), nrows, bits.data());
                    validity = bits.data();
                }
                if (type == TypeId::Bool) {
                    for (std::size_t r = 0; r < nrows; ++r)
                        c.valid[r] = c.words[r] != 0;
                    dstd::Vector<std::uint8_t> packed((nrows + 7) / 8);
                    pack_bits(c.valid.data(), nrows, packed.data());
                    out.columns[i] = Series::flat(
                        type, packed.data(), static_cast<std::int64_t>(nrows),
                        validity);
                } else if (type != TypeId::String) {
                    out.columns[i] = Series::flat(
                        type, c.words.data(), static_cast<std::int64_t>(nrows),
                        validity);
                } else {
                    const std::size_t p = part_of[i];
                    std::size_t bytes = 0;
                    for (const Chunk& ch : chunks)
                        bytes += ch.parts[p].bytes.size();
                    if (bytes > static_cast<std::size_t>(INT32_MAX)) {
                        errors[i].emplace(
                            ErrorCode::InvalidArgument,
                            "a string column holds more than 2 GiB");
                        continue;
                    }
                    dstd::Vector<std::int32_t> offsets(nrows + 1);
                    dstd::Vector<char> data(bytes);
                    std::size_t row = 0;
                    std::size_t at = 0;
                    for (const Chunk& ch : chunks) {
                        const StringPart& part = ch.parts[p];
                        if (!part.bytes.empty())
                            std::memcpy(data.data() + at, part.bytes.data(),
                                        part.bytes.size());
                        for (const std::uint32_t size : part.sizes) {
                            offsets[row] = static_cast<std::int32_t>(at);
                            at += size;
                            ++row;
                        }
                    }
                    offsets[nrows] = static_cast<std::int32_t>(at);
                    out.columns[i] = Series(dftu_series_new_string(
                        static_cast<dftu_dtype>(TypeId::String), offsets.data(),
                        data.data(), static_cast<std::int64_t>(nrows),
                        validity));
                }
                if (!out.columns[i].valid())
                    errors[i].emplace(ErrorCode::Internal,
                                      "failed to build a table column");
            }
        });
    if (!built) return built.error();
    for (const auto& e : errors)
        if (e) return *e;
    return out;
}

}  // namespace dftracer::utils::trace::table::detail
