// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/compress/gzip_member_reader.h>
#include <dftracer/utils/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/gzip/gzip_member_scanner.h>
#include <dftracer/utils/trace/table/table_decode.h>
#include <dftracer/utils/trace/table/table_header.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
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
// The fewest compressed bytes a slab reader holds before it gives a member
// whose end it cannot find to the in-order inflate.
constexpr std::size_t HOLD_MIN = std::size_t{64} << 20;

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

void Buffer::seal() { std::memset(data.get() + size, 0, PADDING); }

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
    b.seal();
    return b;
}

bool has_gzip_magic(const unsigned char* bytes, std::size_t size) {
    return size >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b;
}

namespace {

std::uint32_t trailer_size(const unsigned char* end) {
    return dstd::read_u32_le(end - 4);
}

dstd::Error corrupt() {
    return dstd::Error(ErrorCode::Compression,
                       "gzip data is corrupt or cut short");
}

// The most compressed bytes or member text a slab reader holds before it
// streams the rest of the file in order.
std::size_t hold_bytes(std::size_t slab) {
    return std::max(4 * slab, HOLD_MIN);
}

// Inflates the member at `in` after the `out.size` bytes of `out`, doubling
// `out` up to `limit` while it is too small.
GzipDecode inflate_growing(GzipMemberDecompressor& dec, const unsigned char* in,
                           std::size_t in_size, Buffer& out, std::size_t limit,
                           fileio::compress::DecompressResult& res) {
    for (;;) {
        const GzipDecode status =
            dec.decompress_status(in, in_size, out.data.get() + out.size,
                                  out.capacity - out.size, res);
        if (status != GzipDecode::InsufficientSpace || out.capacity >= limit)
            return status;
        out.reserve(std::min(out.capacity * 2, limit));
    }
}

// Members decode one after another into a buffer that grows.
dstd::Result<Buffer> inflate_in_order(const unsigned char* comp,
                                      std::size_t comp_size,
                                      InflateStats& stats) {
    stats.inflated_in_order = true;
    const std::size_t limit = comp_size * MAX_RATIO + RATIO_SLACK;
    GzipMemberDecompressor dec;
    if (!dec.valid())
        return common::make_error(ErrorCode::Compression,
                                  "failed to allocate a gzip decompressor");
    Buffer out = Buffer::with_capacity(std::min(
        std::max<std::size_t>(comp_size * 4, std::size_t{1} << 20), limit));
    std::size_t at = 0;
    while (at < comp_size) {
        fileio::compress::DecompressResult res{};
        const GzipDecode status =
            inflate_growing(dec, comp + at, comp_size - at, out, limit, res);
        if (status == GzipDecode::InsufficientSpace)
            return common::make_error(
                ErrorCode::Compression,
                "gzip data inflates past the deflate maximum ratio");
        if (status != GzipDecode::Ok) return corrupt();
        out.size += res.out_bytes;
        at += res.in_bytes;
    }
    out.seal();
    return out;
}

}  // namespace

namespace {

// Appends to `starts` each gzip member mark in bytes [from, size) of `base`: a
// whole header that looks valid. Returns where the next scan starts.
std::size_t find_marks(const unsigned char* base, std::size_t size,
                       std::size_t from, dstd::Vector<std::size_t>& starts) {
    constexpr std::size_t HEADER = 10;
    std::size_t i = std::max<std::size_t>(from, 1);
    while (i + HEADER <= size) {
        const void* hit = std::memchr(base + i, 0x1f, size - (HEADER - 1) - i);
        if (hit == nullptr) break;
        i = static_cast<std::size_t>(static_cast<const unsigned char*>(hit) -
                                     base);
        if (index::gzip::gzip_header_looks_valid(base + i)) starts.push_back(i);
        ++i;
    }
    return size >= HEADER ? std::max<std::size_t>(size - (HEADER - 1), 1) : 1;
}

// The output offset of each of members [0, count) of `base` from their
// trailers, the member m spanning [starts[m], ends(m)). Returns how many lead
// members are sane: a member that is too short or claims more than deflate can
// give ends the count, as a false mark at its end gives.
std::size_t claimed_sizes(const unsigned char* base,
                          dstd::Span<const std::size_t> starts,
                          std::size_t count, std::size_t end_of_last,
                          dstd::Vector<std::size_t>& out_at) {
    out_at.assign(count + 1, 0);
    for (std::size_t m = 0; m < count; ++m) {
        const std::size_t end =
            m + 1 < starts.size() ? starts[m + 1] : end_of_last;
        const std::size_t in_size = end - starts[m];
        if (in_size < 18) return m;
        const std::size_t claimed = trailer_size(base + end);
        if (claimed > in_size * MAX_RATIO + 64) return m;
        out_at[m + 1] = out_at[m] + claimed;
    }
    return count;
}

// Inflates members [0, count) in parallel into `out`, which holds
// out_at[count] bytes. Returns the first member that is not a whole member of
// its claimed size, as a false mark gives, or `count` when all are.
dstd::Result<std::size_t> inflate_run(const unsigned char* base,
                                      dstd::Span<const std::size_t> starts,
                                      std::size_t count,
                                      std::size_t end_of_last,
                                      dstd::Span<const std::size_t> out_at,
                                      unsigned char* out) {
    dstd::Vector<std::uint8_t> good(count, 0);
    auto ran = default_runtime().parallel_for(
        static_cast<std::int64_t>(count), 1,
        [&](std::int64_t lo, std::int64_t hi) {
            GzipMemberDecompressor dec;
            if (!dec.valid()) return;
            for (std::int64_t m = lo; m < hi; ++m) {
                const auto k = static_cast<std::size_t>(m);
                const std::size_t end =
                    k + 1 < starts.size() ? starts[k + 1] : end_of_last;
                const std::size_t want = out_at[k + 1] - out_at[k];
                fileio::compress::DecompressResult res{};
                const GzipDecode status =
                    dec.decompress_status(base + starts[k], end - starts[k],
                                          out + out_at[k], want, res);
                good[k] = status == GzipDecode::Ok && res.out_bytes == want &&
                          res.in_bytes == end - starts[k];
            }
        });
    if (!ran) return ran.error();
    std::size_t first_bad = 0;
    while (first_bad < count && good[first_bad]) ++first_bad;
    return first_bad;
}

}  // namespace

// A writer that cuts members at line ends leaves a gzip header at each member
// start, and the member's size in its trailer. The members decode in parallel
// from those marks; any mark that is not a member, or a member that ends
// elsewhere, sends the file to the in-order decode.
dstd::Result<Buffer> inflate_gzip(const Buffer& comp, InflateStats& stats) {
    const unsigned char* base = comp.data.get();
    dstd::Vector<std::size_t> starts;
    starts.push_back(0);
    find_marks(base, comp.size, 1, starts);
    dstd::Vector<std::size_t> out_at;
    if (claimed_sizes(base, starts, starts.size(), comp.size, out_at) !=
        starts.size())
        return inflate_in_order(base, comp.size, stats);
    const std::size_t bytes = out_at.back();
    Buffer out = Buffer::with_capacity(bytes);
    out.size = bytes;
    auto ran = inflate_run(base, starts, starts.size(), comp.size, out_at,
                           out.data.get());
    if (!ran.has_value()) return ran.error();
    if (*ran != starts.size()) return inflate_in_order(base, comp.size, stats);
    stats.members_in_parallel += starts.size();
    out.seal();
    return out;
}

SlabReader::SlabReader(const dstd::File& file, std::uint64_t file_size,
                       bool gzip, std::size_t slab_bytes)
    : file_(file),
      file_size_(file_size),
      gzip_(gzip),
      slab_(std::max<std::size_t>(slab_bytes, 1)) {
    starts_.push_back(0);
}

dstd::Result<void> SlabReader::read_more(std::size_t want) {
    want = static_cast<std::size_t>(
        std::min<std::uint64_t>(want, file_size_ - read_at_));
    if (comp_.size + want > comp_.capacity)
        comp_.reserve(std::max(comp_.size + want, comp_.capacity * 2));
    auto read = file_.read_exact_at(
        read_at_,
        dstd::Span<std::uint8_t>(comp_.data.get() + comp_.size, want));
    if (!read) return read.error();
    comp_.size += want;
    read_at_ += want;
    return {};
}

void SlabReader::scan() {
    scanned_ = find_marks(comp_.data.get(), comp_.size, scanned_, starts_);
}

void SlabReader::start_in_order() {
    stats_.inflated_in_order = true;
    in_order_.emplace(fileio::compress::decode_gzip_members(
        file_.native_handle(), file_size_, slab_, false, held_at_));
    comp_ = Buffer{};
    starts_.clear();
}

// Drops the first `used` bytes held, which end at a member end.
void SlabReader::drop(std::size_t used) {
    std::memmove(comp_.data.get(), comp_.data.get() + used, comp_.size - used);
    comp_.size -= used;
    held_at_ += used;
    const auto kept = std::lower_bound(starts_.begin(), starts_.end(), used);
    starts_.erase(starts_.begin(), kept);
    for (std::size_t& at : starts_) at -= used;
    if (starts_.empty() || starts_.front() != 0)
        starts_.insert(starts_.begin(), 0);
    scanned_ = scanned_ > used ? scanned_ - used : 1;
}

// The first member held, inflated alone to find where it truly ends: a false
// mark inside it, or a header the scan does not mark, leaves its end unknown.
// False when it is not held whole yet; past the hold limit the rest of the
// file inflates in order.
dstd::Result<bool> SlabReader::inflate_first(Buffer& out) {
    GzipMemberDecompressor dec;
    if (!dec.valid())
        return dstd::Error(ErrorCode::Compression,
                           "failed to allocate a gzip decompressor");
    const std::size_t hold = hold_bytes(slab_);
    const std::size_t limit =
        std::min(comp_.size * MAX_RATIO + RATIO_SLACK, hold);
    out = Buffer::with_capacity(
        std::min(std::max(slab_, std::size_t{1} << 20), limit));
    fileio::compress::DecompressResult res{};
    const GzipDecode status =
        inflate_growing(dec, comp_.data.get(), comp_.size, out, limit, res);
    if (status == GzipDecode::Ok) {
        out.size = res.out_bytes;
        drop(res.in_bytes);
        ++stats_.members_alone;
        out.seal();
        return true;
    }
    // A member whose text passes the hold streams in order.
    if (status == GzipDecode::InsufficientSpace && limit == hold) {
        out = Buffer{};
        start_in_order();
        return false;
    }
    out = Buffer{};
    if (read_at_ >= file_size_) return corrupt();
    if (comp_.size >= hold) start_in_order();
    return false;
}

// Inflates the whole members held, up to about one slab, into `out`. False
// when more bytes must be read first or the rest of the file is left to the
// in-order inflate.
dstd::Result<bool> SlabReader::inflate_ready(Buffer& out) {
    const bool all = read_at_ >= file_size_;
    // Every member but the last held ends where the next starts; the last
    // ends at the end of the file once it is read.
    const std::size_t whole = all ? starts_.size() : starts_.size() - 1;
    const unsigned char* base = comp_.data.get();
    dstd::Vector<std::size_t> out_at;
    const std::size_t sane =
        claimed_sizes(base, starts_, whole, comp_.size, out_at);
    // A member whose text passes the hold, or whose trailer claims so, is not
    // inflated whole.
    const std::size_t hold = hold_bytes(slab_);
    std::size_t count = 0;
    while (count < sane && out_at[count] < slab_ &&
           out_at[count + 1] - out_at[count] <= hold)
        ++count;
    if (count == 0 && sane > 0) return inflate_first(out);
    if (sane == whole && !all && out_at[count] < slab_ && comp_.size < slab_)
        return false;
    if (count == 0) {
        if (whole == 0 && !all && comp_.size < slab_) return false;
        return inflate_first(out);
    }
    out = Buffer::with_capacity(out_at[count]);
    auto ran =
        inflate_run(base, starts_, count, comp_.size, out_at, out.data.get());
    if (!ran.has_value()) return ran.error();
    if (*ran == 0) return inflate_first(out);
    count = *ran;
    out.size = out_at[count];
    stats_.members_in_parallel += count;
    drop(count < starts_.size() ? starts_[count] : comp_.size);
    out.seal();
    return true;
}

coro::CoroTask<dstd::Result<Buffer>> SlabReader::next() {
    if (!gzip_) {
        const std::size_t n = static_cast<std::size_t>(
            std::min<std::uint64_t>(slab_, file_size_ - read_at_));
        Buffer out = Buffer::with_capacity(n);
        auto read = file_.read_exact_at(
            read_at_, dstd::Span<std::uint8_t>(out.data.get(), n));
        if (!read) co_return read.error();
        out.size = n;
        read_at_ += n;
        out.seal();
        co_return out;
    }
    while (!in_order_) {
        if (comp_.size == 0 && read_at_ >= file_size_) {
            Buffer none = Buffer::with_capacity(0);
            none.seal();
            co_return none;
        }
        Buffer out;
        auto ready = inflate_ready(out);
        if (!ready.has_value()) co_return ready.error();
        // An empty member gives no text, which must not read as the end.
        if (*ready) {
            if (out.size != 0) co_return out;
            continue;
        }
        if (in_order_) break;
        if (read_at_ >= file_size_) co_return corrupt();
        auto read = read_more(std::max(slab_ / 4, std::size_t{1} << 20));
        if (!read) co_return read.error();
        scan();
    }
    Buffer out = Buffer::with_capacity(slab_);
    while (out.size < slab_) {
        auto item = co_await in_order_->next();
        if (!item) break;
        if (!*item) co_return item->error();
        const dstd::StringView bytes = **item;
        if (out.size + bytes.size() > out.capacity)
            out.reserve(std::max(out.size + bytes.size(), out.capacity * 2));
        std::memcpy(out.data.get() + out.size, bytes.data(), bytes.size());
        out.size += bytes.size();
    }
    out.seal();
    co_return out;
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
    std::size_t row_base = 0;        ///< added to a row an error names
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
                ErrorCode::Parse, chunk_->row_base + row, why);
        else
            chunk_->error = dstd::Error::from_format<"row {}, field {}: {}">(
                ErrorCode::Parse, chunk_->row_base + row, field, why);
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

template <class T>
Series narrow_column(TypeId type, const std::uint64_t* words, std::size_t n,
                     const std::uint8_t* validity) {
    dstd::Vector<T> values(n);
    for (std::size_t i = 0; i < n; ++i) {
        if constexpr (std::is_same_v<T, float>)
            values[i] = static_cast<float>(std::bit_cast<double>(words[i]));
        else
            values[i] = static_cast<T>(words[i]);
    }
    return Series::flat(type, values.data(), static_cast<std::int64_t>(n),
                        validity);
}

Series number_column(TypeId type, const std::uint64_t* words, std::size_t n,
                     const std::uint8_t* validity) {
    switch (type) {
        case TypeId::Int8:
            return narrow_column<std::int8_t>(type, words, n, validity);
        case TypeId::Int16:
            return narrow_column<std::int16_t>(type, words, n, validity);
        case TypeId::Int32:
            return narrow_column<std::int32_t>(type, words, n, validity);
        case TypeId::Uint8:
            return narrow_column<std::uint8_t>(type, words, n, validity);
        case TypeId::Uint16:
            return narrow_column<std::uint16_t>(type, words, n, validity);
        case TypeId::Uint32:
            return narrow_column<std::uint32_t>(type, words, n, validity);
        case TypeId::Float32:
            return narrow_column<float>(type, words, n, validity);
        default:
            return Series::flat(type, words, static_cast<std::int64_t>(n),
                                validity);
    }
}

}  // namespace

TypeId column_type(const Field& field) {
    switch (field.kind) {
        case Kind::Int:
            return field.bits == 8    ? TypeId::Int8
                   : field.bits == 16 ? TypeId::Int16
                   : field.bits == 32 ? TypeId::Int32
                                      : TypeId::Int64;
        case Kind::UInt:
            return field.bits == 8    ? TypeId::Uint8
                   : field.bits == 16 ? TypeId::Uint16
                   : field.bits == 32 ? TypeId::Uint32
                                      : TypeId::Uint64;
        case Kind::Float:
            return field.bits == 32 ? TypeId::Float32 : TypeId::Float64;
        case Kind::Bool:
            return TypeId::Bool;
        default:
            return TypeId::String;
    }
}

dstd::Result<Decoded> decode_lines(const Header& header,
                                   dstd::Span<const std::size_t> wanted,
                                   dstd::StringView range, std::size_t parts,
                                   std::size_t row_base) {
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
        c.row_base = row_base;
        c.first_row = total;
        total += c.rows;
    }
    const std::size_t nrows = total;

    dstd::Vector<Column> columns(ncols);
    dstd::Vector<std::size_t> part_of(ncols, NOT_WANTED);
    std::size_t nparts_strings = 0;
    for (std::size_t k = 0; k < ncols; ++k) {
        columns[k].valid.resize(nrows);
        if (column_type(header.fields[wanted[k]]) == TypeId::String)
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
                const TypeId type = column_type(header.fields[wanted[i]]);
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
                    out.columns[i] =
                        number_column(type, c.words.data(), nrows, validity);
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
