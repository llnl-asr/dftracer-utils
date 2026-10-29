#include <dftracer/utils/dstd/varint.h>
#include <dftracer/utils/index/schemas/dft/agg/chunk_columns.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace dftracer::utils::index::schemas::dft::agg {

namespace {

constexpr std::uint8_t BLOCK_FORMAT = 0;
constexpr std::uint8_t MAX_EXPONENT = 18;
constexpr std::size_t SAMPLE_VALUES = 256;
constexpr double MAX_SCALED = 9007199254740992.0;
constexpr double POW10[MAX_EXPONENT + 1] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8, 1e9,
    1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18};

std::size_t varint_bytes(std::uint64_t v) {
    std::size_t bytes = 1;
    while (v >= 0x80U) {
        v >>= 7U;
        ++bytes;
    }
    return bytes;
}

/// Zigzag of the two's complement bit pattern of a signed value, so a
/// difference that wrapped still round-trips exactly.
std::uint64_t zigzag_bits(std::uint64_t bits) {
    return (bits << 1U) ^ (0U - (bits >> 63U));
}

std::uint64_t unzigzag_bits(std::uint64_t z) {
    return (z >> 1U) ^ (0U - (z & 1U));
}

/// v * 10^e when that is inside the encoder's range; false for a value that
/// scaling cannot bring into it, NaN and the infinities included.
bool scale(double v, std::uint8_t e, double& scaled) {
    scaled = v * POW10[e];
    return scaled > -MAX_SCALED && scaled < MAX_SCALED;
}

/// Whether v encodes at exponent e as an integer i that decodes back to the
/// very same double.
bool encodes_as(double v, std::uint8_t e, std::int64_t& i) {
    double scaled = 0;
    if (!scale(v, e, scaled)) return false;
    i = std::llround(scaled);
    return double_bits(static_cast<double>(i) / POW10[e]) == double_bits(v);
}

/// The exponent with the fewest exceptions over a sample of the column, ties
/// to the smaller exponent.
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
        append_varint(out, zigzag_bits(v - prev));
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
        append_varint(out, zigzag_bits(step - step_prev));
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
        append_varint(out, zigzag_bits(block.dur[r] - dur_prev));
        dur_prev = block.dur[r];
    }
    put_deltas(out, block.pid);
    put_deltas(out, block.tid);
    put_rle(out, block.phase, [&out](std::uint8_t v) { out.push_back(v); });
    const auto put_local = [&out](std::uint32_t v) { append_varint(out, v); };
    put_rle(out, block.cat, put_local);
    put_rle(out, block.name, put_local);
    put_rle(out, block.host, put_local);
}

void put_column(dstd::Vector<std::uint8_t>& out, const ChunkColumn& column) {
    append_varint(out, column.path);
    out.push_back(static_cast<std::uint8_t>(column.kind));
    append_varint(out, column.rows.size());
    put_gaps(out, column.rows);
    if (column.kind != ColumnKind::Double) {
        std::uint64_t prev = 0;
        for (const std::uint64_t bits : column.bits) {
            append_varint(out, zigzag_bits(bits - prev));
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
        append_varint(out, zigzag_bits(value - prev));
        prev = value;
    }
}

struct ColumnSpan {
    std::uint32_t path;
    std::uint64_t rows;
    std::size_t size;
};

struct BlockLayout {
    /// Where the column count sits in the output.
    std::size_t columns_at = 0;
    dstd::Vector<ColumnSpan> columns;
};

bool lists(const dstd::Vector<std::uint32_t>& list, std::uint32_t value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

void append_block(const ChunkColumns& block, std::size_t column_count,
                  const dstd::Vector<std::uint32_t>& dropped,
                  dstd::Vector<std::uint8_t>& out, BlockLayout* layout) {
    out.push_back(BLOCK_FORMAT);
    append_varint(out, block.rows);
    append_varint(out, block.ids.size());
    for (const std::uint32_t id : block.ids) append_varint(out, id);
    put_core(out, block);
    const std::size_t columns_at = out.size();
    std::size_t kept = 0;
    for (std::size_t k = 0; k < column_count; ++k)
        if (!lists(dropped, block.columns[k].path)) ++kept;
    append_varint(out, kept);
    for (std::size_t k = 0; k < column_count; ++k) {
        const ChunkColumn& column = block.columns[k];
        if (lists(dropped, column.path)) continue;
        const std::size_t at = out.size();
        put_column(out, column);
        if (layout != nullptr) {
            layout->columns.push_back(
                {column.path, column.rows.size(), out.size() - at});
        }
    }
    append_varint(out, dropped.size());
    for (const std::uint32_t path : dropped) append_varint(out, path);
    if (layout != nullptr) layout->columns_at = columns_at;
}

std::size_t dropped_bytes(const dstd::Vector<std::uint32_t>& dropped) {
    std::size_t bytes = varint_bytes(dropped.size());
    for (const std::uint32_t path : dropped) bytes += varint_bytes(path);
    return bytes;
}

bool get_varint(LeReader& r, std::uint64_t& out) {
    const std::uint8_t* p = r.position();
    if (!read_varint(p, p + r.remaining(), out)) return false;
    r.take(static_cast<std::size_t>(p - r.position()));
    return true;
}

/// A count whose elements take at least one byte each, so a count above the
/// bytes left is malformed and never reserves a large buffer.
bool get_count(LeReader& r, std::uint64_t limit, std::uint64_t& n) {
    return get_varint(r, n) && n <= limit && n <= r.remaining();
}

/// `n` strictly ascending row ordinals below `rows`, stored as gaps.
bool get_gaps(LeReader& r, std::uint64_t rows, std::size_t n,
              dstd::Vector<std::uint64_t>& out) {
    out.resize(n);
    std::uint64_t prev = 0;
    for (std::size_t k = 0; k < n; ++k) {
        std::uint64_t gap = 0;
        if (!get_varint(r, gap)) return false;
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
        if (!get_varint(r, z)) return false;
        prev += unzigzag_bits(z);
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
        if (!get_varint(r, length) || !r.u8(value) || length == 0 ||
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
        if (!get_varint(r, length) || !get_varint(r, value) || length == 0 ||
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
        if (!get_varint(r, z)) return false;
        dur_prev += unzigzag_bits(z);
        block.dur[k] = dur_prev;
    }
    return get_deltas(r, steps.size(), block.pid) &&
           get_deltas(r, steps.size(), block.tid) &&
           get_rle(r, rows, block.phase) &&
           get_rle(r, rows, id_count, block.cat) &&
           get_rle(r, rows, id_count, block.name) &&
           get_rle(r, rows, id_count, block.host);
}

bool get_column(LeReader& r, std::uint64_t rows, std::size_t id_count,
                ChunkColumn& column) {
    std::uint64_t path = 0;
    std::uint8_t kind = 0;
    if (!get_varint(r, path) || path >= id_count || !r.u8(kind) ||
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
        if (!get_varint(r, gap)) return false;
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
        if (!get_varint(r, z)) return false;
        value_prev += unzigzag_bits(z);
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
    ts.clear();
    dur.clear();
    has_dur.clear();
    pid.clear();
    tid.clear();
    phase.clear();
    dropped.clear();
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

void ChunkColumnBuilder::clear() {
    block_.clear();
    local_of_.clear();
    column_of_.clear();
    live_columns_ = 0;
    last_cat_ = NO_DICTIONARY_ID;
    last_name_ = NO_DICTIONARY_ID;
    last_host_ = NO_DICTIONARY_ID;
}

void ChunkColumnBuilder::add_row(std::uint64_t ts, std::uint64_t dur,
                                 bool has_dur, std::uint64_t pid,
                                 std::uint64_t tid, std::uint8_t phase,
                                 std::uint32_t cat, std::uint32_t name,
                                 std::uint32_t host) {
    block_.ts.push_back(ts);
    block_.dur.push_back(dur);
    block_.has_dur.push_back(has_dur ? 1 : 0);
    block_.pid.push_back(pid);
    block_.tid.push_back(tid);
    block_.phase.push_back(phase);
    block_.cat.push_back(row_local_id(cat, last_cat_, last_cat_local_));
    block_.name.push_back(row_local_id(name, last_name_, last_name_local_));
    block_.host.push_back(row_local_id(host, last_host_, last_host_local_));
    ++block_.rows;
}

void ChunkColumnBuilder::add_int64(std::uint32_t path, std::int64_t v) {
    ChunkColumn& column = column_of(path, ColumnKind::Int64);
    column.rows.push_back(block_.rows - 1);
    column.bits.push_back(static_cast<std::uint64_t>(v));
}

void ChunkColumnBuilder::add_uint64(std::uint32_t path, std::uint64_t v) {
    ChunkColumn& column = column_of(path, ColumnKind::Uint64);
    column.rows.push_back(block_.rows - 1);
    column.bits.push_back(v);
}

void ChunkColumnBuilder::add_double(std::uint32_t path, double v) {
    ChunkColumn& column = column_of(path, ColumnKind::Double);
    column.rows.push_back(block_.rows - 1);
    column.bits.push_back(double_bits(v));
}

bool ChunkColumnBuilder::encode_into(dstd::Vector<std::uint8_t>& out,
                                     std::size_t budget) {
    if (block_.rows == 0) return false;
    encode_chunk_columns(block_, live_columns_, out, budget);
    return true;
}

void encode_chunk_columns(const ChunkColumns& block, std::size_t column_count,
                          dstd::Vector<std::uint8_t>& out, std::size_t budget) {
    const std::size_t start = out.size();
    dstd::Vector<std::uint32_t> dropped(block.dropped.begin(),
                                        block.dropped.end());
    BlockLayout layout;
    append_block(block, column_count, dropped, out, &layout);
    if (budget == NO_COLUMN_BUDGET || out.size() - start <= budget) return;

    // Over budget: drop whole arg paths, those with the fewest rows first, and
    // list them. The estimates are exact, so one more encode is the last.
    const std::size_t prefix = layout.columns_at - start;
    std::size_t kept_bytes = 0;
    std::size_t kept_columns = 0;
    for (const ColumnSpan& span : layout.columns) {
        kept_bytes += span.size;
        ++kept_columns;
    }
    while (true) {
        const std::size_t size = prefix + varint_bytes(kept_columns) +
                                 kept_bytes + dropped_bytes(dropped);
        if (size <= budget) break;
        bool found = false;
        std::uint32_t next = 0;
        std::uint64_t next_rows = 0;
        for (const ColumnSpan& span : layout.columns) {
            if (lists(dropped, span.path)) continue;
            std::uint64_t rows = 0;
            for (const ColumnSpan& other : layout.columns)
                if (other.path == span.path) rows += other.rows;
            if (found && (rows > next_rows ||
                          (rows == next_rows && span.path >= next))) {
                continue;
            }
            found = true;
            next = span.path;
            next_rows = rows;
        }
        if (!found) break;
        dropped.push_back(next);
        for (const ColumnSpan& span : layout.columns) {
            if (span.path != next) continue;
            kept_bytes -= span.size;
            --kept_columns;
        }
    }
    out.resize(start);
    append_block(block, column_count, dropped, out, nullptr);
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
        if (!get_varint(r, id) || id > UINT32_MAX) return bad();
        block.ids[k] = static_cast<std::uint32_t>(id);
    }
    if (!get_core(r, block, block.ids.size())) return bad();
    std::uint64_t columns = 0;
    if (!get_count(r, r.remaining(), columns)) return bad();
    block.columns.resize(static_cast<std::size_t>(columns));
    for (ChunkColumn& column : block.columns) {
        if (!get_column(r, rows, block.ids.size(), column)) return bad();
    }
    std::uint64_t dropped = 0;
    if (!get_count(r, r.remaining(), dropped)) return bad();
    block.dropped.resize(static_cast<std::size_t>(dropped));
    for (std::size_t k = 0; k < block.dropped.size(); ++k) {
        std::uint64_t path = 0;
        if (!get_varint(r, path) || path >= block.ids.size()) return bad();
        block.dropped[k] = static_cast<std::uint32_t>(path);
    }
    if (r.remaining() != 0) return bad();
    return block;
}

}  // namespace dftracer::utils::index::schemas::dft::agg
