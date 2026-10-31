#include <dftracer/utils/dstd/varint.h>
#include <dftracer/utils/index/schemas/dft/agg/chunk_columns.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace dftracer::utils::index::schemas::dft::agg {

namespace {

constexpr std::uint8_t BLOCK_FORMAT = 0;
constexpr std::uint8_t MAX_EXPONENT = 18;
constexpr std::size_t SAMPLE_VALUES = 256;
constexpr double MAX_SCALED = 9007199254740992.0;
constexpr double POW10[MAX_EXPONENT + 1] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8, 1e9,
    1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18};

/// False when v * 10^e is outside the encoder's range, NaN and infinities
/// included.
bool scale(double v, std::uint8_t e, double& scaled) {
    scaled = v * POW10[e];
    return scaled > -MAX_SCALED && scaled < MAX_SCALED;
}

/// Whether v at exponent e decodes back to the very same double.
bool encodes_as(double v, std::uint8_t e, std::int64_t& i) {
    double scaled = 0;
    if (!scale(v, e, scaled)) return false;
    i = std::llround(scaled);
    return double_bits(static_cast<double>(i) / POW10[e]) == double_bits(v);
}

std::uint8_t best_exponent(const dstd::Vector<std::uint64_t>& bits) {
    const std::size_t sample = std::min(bits.size(), SAMPLE_VALUES);
    std::array<std::uint32_t, MAX_EXPONENT + 1> encoded{};
    for (std::size_t k = 0; k < sample; ++k) {
        const double v = bits_double(bits[k]);
        double scaled = 0;
        for (std::uint8_t e = 0; e <= MAX_EXPONENT; ++e) {
            if (!scale(v, e, scaled)) break;
            const auto i = std::llround(scaled);
            if (double_bits(static_cast<double>(i) / POW10[e]) ==
                double_bits(v)) {
                ++encoded[e];
            }
        }
    }
    std::uint8_t best = 0;
    for (std::uint8_t e = 1; e <= MAX_EXPONENT; ++e)
        if (encoded[e] > encoded[best]) best = e;
    return best;
}

template <class T, class Put>
void put_rle(dstd::Vector<std::uint8_t>& out, const dstd::Vector<T>& values,
             Put&& put) {
    std::size_t runs = 0;
    for (std::size_t i = 0; i < values.size(); ++i)
        if (i == 0 || values[i] != values[i - 1]) ++runs;
    append_varint(out, runs);
    std::size_t i = 0;
    while (i < values.size()) {
        std::size_t j = i + 1;
        while (j < values.size() && values[j] == values[i]) ++j;
        append_varint(out, j - i);
        put(values[i]);
        i = j;
    }
}

void put_deltas(dstd::Vector<std::uint8_t>& out,
                const dstd::Vector<std::uint64_t>& values) {
    std::uint64_t prev = 0;
    for (const std::uint64_t v : values) {
        append_varint(out, zigzag_encode(static_cast<std::int64_t>(v - prev)));
        prev = v;
    }
}

void put_gaps(dstd::Vector<std::uint8_t>& out,
              const dstd::Vector<std::uint64_t>& rows) {
    std::uint64_t prev = 0;
    for (const std::uint64_t row : rows) {
        append_varint(out, row - prev);
        prev = row;
    }
}

void put_core(dstd::Vector<std::uint8_t>& out, const ChunkColumns& block) {
    std::uint64_t ts_prev = 0;
    std::uint64_t step_prev = 0;
    for (const std::uint64_t ts : block.ts) {
        const std::uint64_t step = ts - ts_prev;
        append_varint(
            out, zigzag_encode(static_cast<std::int64_t>(step - step_prev)));
        step_prev = step;
        ts_prev = ts;
    }
    std::uint64_t missing = 0;
    for (const std::uint8_t has : block.has_dur) missing += has != 0 ? 0 : 1;
    append_varint(out, missing);
    std::uint64_t prev_row = 0;
    for (std::size_t r = 0; r < block.has_dur.size(); ++r) {
        if (block.has_dur[r] != 0) continue;
        append_varint(out, r - prev_row);
        prev_row = r;
    }
    std::uint64_t dur_prev = 0;
    for (std::size_t r = 0; r < block.has_dur.size(); ++r) {
        if (block.has_dur[r] == 0) continue;
        append_varint(
            out,
            zigzag_encode(static_cast<std::int64_t>(block.dur[r] - dur_prev)));
        dur_prev = block.dur[r];
    }
    put_deltas(out, block.pid);
    put_deltas(out, block.tid);
    put_rle(out, block.phase, [&out](std::uint8_t v) { out.push_back(v); });
    const auto put_local = [&out](std::uint32_t v) { append_varint(out, v); };
    put_rle(out, block.cat, put_local);
    put_rle(out, block.name, put_local);
    put_rle(out, block.host, put_local);
    put_rle(out, block.file, put_local);
}

void put_column(dstd::Vector<std::uint8_t>& out, const ChunkColumn& column) {
    append_varint(out, column.path);
    out.push_back(static_cast<std::uint8_t>(column.kind));
    append_varint(out, column.rows.size());
    put_gaps(out, column.rows);
    if (column.kind != ColumnKind::Double) {
        std::uint64_t prev = 0;
        for (const std::uint64_t bits : column.bits) {
            append_varint(
                out, zigzag_encode(static_cast<std::int64_t>(bits - prev)));
            prev = bits;
        }
        return;
    }
    const std::uint8_t exponent = best_exponent(column.bits);
    out.push_back(exponent);
    std::size_t exceptions = 0;
    for (const std::uint64_t bits : column.bits) {
        std::int64_t i = 0;
        if (!encodes_as(bits_double(bits), exponent, i)) ++exceptions;
    }
    append_varint(out, exceptions);
    std::uint64_t prev_row = 0;
    for (std::size_t k = 0; k < column.bits.size(); ++k) {
        std::int64_t i = 0;
        if (encodes_as(bits_double(column.bits[k]), exponent, i)) continue;
        append_varint(out, column.rows[k] - prev_row);
        prev_row = column.rows[k];
        append_u64_le(out, column.bits[k]);
    }
    std::uint64_t prev = 0;
    for (const std::uint64_t bits : column.bits) {
        std::int64_t i = 0;
        if (!encodes_as(bits_double(bits), exponent, i)) continue;
        const auto value = static_cast<std::uint64_t>(i);
        append_varint(out,
                      zigzag_encode(static_cast<std::int64_t>(value - prev)));
        prev = value;
    }
}

void append_block(const ChunkColumns& block, std::size_t column_count,
                  dstd::Vector<std::uint8_t>& out) {
    out.push_back(BLOCK_FORMAT);
    append_varint(out, block.rows);
    append_varint(out, block.ids.size());
    for (const std::uint32_t id : block.ids) append_varint(out, id);
    put_core(out, block);
    append_varint(out, column_count);
    for (std::size_t k = 0; k < column_count; ++k)
        put_column(out, block.columns[k]);
}

/// Every element takes at least one byte, so a count above the bytes left is
/// malformed and never reserves a large buffer.
bool get_count(LeReader& r, std::uint64_t limit, std::uint64_t& n) {
    return read_varint(r, n) && n <= limit && n <= r.remaining();
}

bool get_gaps(LeReader& r, std::uint64_t rows, std::size_t n,
              dstd::Vector<std::uint64_t>& out) {
    out.resize(n);
    std::uint64_t prev = 0;
    for (std::size_t k = 0; k < n; ++k) {
        std::uint64_t gap = 0;
        if (!read_varint(r, gap)) return false;
        if (k == 0) {
            if (gap >= rows) return false;
            prev = gap;
        } else {
            if (gap == 0 || gap > rows - 1 - prev) return false;
            prev += gap;
        }
        out[k] = prev;
    }
    return true;
}

bool get_deltas(LeReader& r, std::size_t n, dstd::Vector<std::uint64_t>& out) {
    out.resize(n);
    std::uint64_t prev = 0;
    for (std::size_t k = 0; k < n; ++k) {
        std::uint64_t z = 0;
        if (!read_varint(r, z)) return false;
        prev += static_cast<std::uint64_t>(zigzag_decode(z));
        out[k] = prev;
    }
    return true;
}

bool get_rle(LeReader& r, std::uint64_t rows, dstd::Vector<std::uint8_t>& out) {
    std::uint64_t runs = 0;
    if (!get_count(r, rows, runs)) return false;
    out.resize(rows);
    std::size_t at = 0;
    for (std::uint64_t i = 0; i < runs; ++i) {
        std::uint64_t length = 0;
        std::uint8_t value = 0;
        if (!read_varint(r, length) || !r.u8(value) || length == 0 ||
            length > rows - at) {
            return false;
        }
        for (std::uint64_t k = 0; k < length; ++k) out[at++] = value;
    }
    return at == rows;
}

bool get_rle(LeReader& r, std::uint64_t rows, std::size_t id_count,
             dstd::Vector<std::uint32_t>& out) {
    std::uint64_t runs = 0;
    if (!get_count(r, rows, runs)) return false;
    out.resize(rows);
    std::size_t at = 0;
    for (std::uint64_t i = 0; i < runs; ++i) {
        std::uint64_t length = 0;
        std::uint64_t value = 0;
        if (!read_varint(r, length) || !read_varint(r, value) || length == 0 ||
            length > rows - at || value >= id_count) {
            return false;
        }
        for (std::uint64_t k = 0; k < length; ++k)
            out[at++] = static_cast<std::uint32_t>(value);
    }
    return at == rows;
}

bool get_core(LeReader& r, ChunkColumns& block, std::size_t id_count) {
    const std::uint64_t rows = block.rows;
    dstd::Vector<std::uint64_t> steps;
    if (!get_deltas(r, static_cast<std::size_t>(rows), steps)) return false;
    block.ts.resize(steps.size());
    std::uint64_t ts = 0;
    for (std::size_t k = 0; k < steps.size(); ++k) {
        ts += steps[k];
        block.ts[k] = ts;
    }
    std::uint64_t missing = 0;
    if (!get_count(r, rows, missing)) return false;
    dstd::Vector<std::uint64_t> gaps;
    if (!get_gaps(r, rows, static_cast<std::size_t>(missing), gaps)) {
        return false;
    }
    block.dur.resize(steps.size());
    block.has_dur.resize(steps.size(), 1);
    for (const std::uint64_t row : gaps) block.has_dur[row] = 0;
    std::uint64_t dur_prev = 0;
    for (std::size_t k = 0; k < steps.size(); ++k) {
        if (block.has_dur[k] == 0) continue;
        std::uint64_t z = 0;
        if (!read_varint(r, z)) return false;
        dur_prev += static_cast<std::uint64_t>(zigzag_decode(z));
        block.dur[k] = dur_prev;
    }
    return get_deltas(r, steps.size(), block.pid) &&
           get_deltas(r, steps.size(), block.tid) &&
           get_rle(r, rows, block.phase) &&
           get_rle(r, rows, id_count, block.cat) &&
           get_rle(r, rows, id_count, block.name) &&
           get_rle(r, rows, id_count, block.host) &&
           get_rle(r, rows, id_count, block.file);
}

bool get_column(LeReader& r, std::uint64_t rows, std::size_t id_count,
                ChunkColumn& column) {
    std::uint64_t path = 0;
    std::uint8_t kind = 0;
    if (!read_varint(r, path) || path >= id_count || !r.u8(kind) ||
        kind > static_cast<std::uint8_t>(ColumnKind::Double)) {
        return false;
    }
    column.path = static_cast<std::uint32_t>(path);
    column.kind = static_cast<ColumnKind>(kind);
    std::uint64_t n = 0;
    if (!get_count(r, rows, n) || n == 0) return false;
    if (!get_gaps(r, rows, static_cast<std::size_t>(n), column.rows)) {
        return false;
    }
    if (column.kind != ColumnKind::Double) {
        return get_deltas(r, column.rows.size(), column.bits);
    }
    std::uint8_t exponent = 0;
    std::uint64_t exceptions = 0;
    if (!r.u8(exponent) || exponent > MAX_EXPONENT ||
        !get_count(r, n, exceptions)) {
        return false;
    }
    dstd::Vector<std::uint64_t> rows_without;
    dstd::Vector<std::uint64_t> bits;
    rows_without.resize(static_cast<std::size_t>(exceptions));
    bits.resize(static_cast<std::size_t>(exceptions));
    std::uint64_t prev = 0;
    for (std::size_t k = 0; k < bits.size(); ++k) {
        std::uint64_t gap = 0;
        if (!read_varint(r, gap)) return false;
        if (k == 0) {
            if (gap >= rows) return false;
            prev = gap;
        } else {
            if (gap == 0 || gap > rows - 1 - prev) return false;
            prev += gap;
        }
        rows_without[k] = prev;
        if (!r.u64(bits[k])) return false;
    }
    column.bits.resize(column.rows.size());
    std::size_t at = 0;
    std::uint64_t value_prev = 0;
    for (std::size_t k = 0; k < column.rows.size(); ++k) {
        if (at < rows_without.size() && rows_without[at] == column.rows[k]) {
            column.bits[k] = bits[at++];
            continue;
        }
        std::uint64_t z = 0;
        if (!read_varint(r, z)) return false;
        value_prev += static_cast<std::uint64_t>(zigzag_decode(z));
        const std::int64_t i = static_cast<std::int64_t>(value_prev);
        column.bits[k] = double_bits(static_cast<double>(i) / POW10[exponent]);
    }
    return at == rows_without.size();
}

}  // namespace

void ChunkColumns::clear() {
    rows = 0;
    ids.clear();
    texts.clear();
    cat.clear();
    name.clear();
    host.clear();
    file.clear();
    ts.clear();
    dur.clear();
    has_dur.clear();
    pid.clear();
    tid.clear();
    phase.clear();
    for (ChunkColumn& column : columns) {
        column.rows.clear();
        column.bits.clear();
    }
}

std::uint32_t ChunkColumnBuilder::local_id(std::uint32_t id) {
    const auto at = local_of_.find(id);
    if (at != local_of_.end()) return at->second;
    const auto local = static_cast<std::uint32_t>(block_.ids.size());
    block_.ids.push_back(id);
    local_of_.emplace(id, local);
    return local;
}

ChunkColumn& ChunkColumnBuilder::column_of(std::uint32_t path,
                                           ColumnKind kind) {
    const std::uint64_t key = (static_cast<std::uint64_t>(path) << 2U) |
                              static_cast<std::uint64_t>(kind);
    const auto at = column_of_.find(key);
    if (at != column_of_.end()) return block_.columns[at->second];
    const std::uint32_t live = live_columns_++;
    if (live == block_.columns.size()) block_.columns.emplace_back();
    ChunkColumn& column = block_.columns[live];
    column.path = local_id(path);
    column.kind = kind;
    column_of_.emplace(key, live);
    return column;
}

std::uint32_t ChunkColumnBuilder::row_local_id(std::uint32_t id,
                                               std::uint32_t& last,
                                               std::uint32_t& last_local) {
    if (id == last) return last_local;
    last_local = local_id(id);
    last = id;
    return last_local;
}

bool ChunkColumnBuilder::first_in_row(std::uint32_t path) {
    const std::uint32_t local = local_id(path);
    if (local >= path_row_.size()) path_row_.resize(local + 1, 0);
    if (path_row_[local] == block_.rows) return false;
    path_row_[local] = block_.rows;
    return true;
}

void ChunkColumnBuilder::clear() {
    block_.clear();
    path_row_.clear();
    local_of_.clear();
    column_of_.clear();
    live_columns_ = 0;
    last_cat_ = NO_DICTIONARY_ID;
    last_name_ = NO_DICTIONARY_ID;
    last_host_ = NO_DICTIONARY_ID;
    last_file_ = NO_DICTIONARY_ID;
}

void ChunkColumnBuilder::add_row(std::uint64_t ts, std::uint64_t dur,
                                 bool has_dur, std::uint64_t pid,
                                 std::uint64_t tid, std::uint8_t phase,
                                 std::uint32_t cat, std::uint32_t name,
                                 std::uint32_t host, std::uint32_t file) {
    block_.ts.push_back(ts);
    block_.dur.push_back(dur);
    block_.has_dur.push_back(has_dur ? 1 : 0);
    block_.pid.push_back(pid);
    block_.tid.push_back(tid);
    block_.phase.push_back(phase);
    block_.cat.push_back(row_local_id(cat, last_cat_, last_cat_local_));
    block_.name.push_back(row_local_id(name, last_name_, last_name_local_));
    block_.host.push_back(row_local_id(host, last_host_, last_host_local_));
    block_.file.push_back(row_local_id(file, last_file_, last_file_local_));
    ++block_.rows;
}

void ChunkColumnBuilder::add_int64(std::uint32_t path, std::int64_t v) {
    if (!first_in_row(path)) return;
    ChunkColumn& column = column_of(path, ColumnKind::Int64);
    column.rows.push_back(block_.rows - 1);
    column.bits.push_back(static_cast<std::uint64_t>(v));
}

void ChunkColumnBuilder::add_uint64(std::uint32_t path, std::uint64_t v) {
    if (!first_in_row(path)) return;
    ChunkColumn& column = column_of(path, ColumnKind::Uint64);
    column.rows.push_back(block_.rows - 1);
    column.bits.push_back(v);
}

void ChunkColumnBuilder::add_double(std::uint32_t path, double v) {
    if (!first_in_row(path)) return;
    ChunkColumn& column = column_of(path, ColumnKind::Double);
    column.rows.push_back(block_.rows - 1);
    column.bits.push_back(double_bits(v));
}

bool ChunkColumnBuilder::encode_into(dstd::Vector<std::uint8_t>& out) {
    if (block_.rows == 0) return false;
    encode_chunk_columns(block_, live_columns_, out);
    return true;
}

void encode_chunk_columns(const ChunkColumns& block, std::size_t column_count,
                          dstd::Vector<std::uint8_t>& out) {
    append_block(block, column_count, out);
}

dstd::Expected<ChunkColumns, ErrorCode> decode_chunk_columns(
    dstd::Span<const std::uint8_t> bytes) {
    LeReader r(bytes.data(), bytes.size());
    const auto bad = [] { return dstd::Unexpected(ErrorCode::Parse); };
    std::uint8_t format = 0;
    if (!r.u8(format) || format != BLOCK_FORMAT) return bad();
    ChunkColumns block;
    std::uint64_t rows = 0;
    std::uint64_t ids = 0;
    if (!get_count(r, r.remaining(), rows) ||
        !get_count(r, r.remaining(), ids)) {
        return bad();
    }
    block.rows = static_cast<std::size_t>(rows);
    block.ids.resize(static_cast<std::size_t>(ids));
    for (std::size_t k = 0; k < block.ids.size(); ++k) {
        std::uint64_t id = 0;
        if (!read_varint(r, id) || id > UINT32_MAX) return bad();
        block.ids[k] = static_cast<std::uint32_t>(id);
    }
    if (!get_core(r, block, block.ids.size())) return bad();
    std::uint64_t columns = 0;
    if (!get_count(r, r.remaining(), columns)) return bad();
    block.columns.resize(static_cast<std::size_t>(columns));
    for (ChunkColumn& column : block.columns) {
        if (!get_column(r, rows, block.ids.size(), column)) return bad();
    }
    if (r.remaining() != 0) return bad();
    return block;
}

}  // namespace dftracer::utils::index::schemas::dft::agg
