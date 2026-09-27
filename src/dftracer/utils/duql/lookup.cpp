#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/duql/vectorize.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace dftracer::utils::duql {

namespace {

namespace df = dftracer::utils::dataframe;

[[noreturn]] void fail(const std::string& why) {
    throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT, "View::duql " + why);
}

bool bit(const df::Series& s, std::int64_t r) {
    const auto* bits = s.data<std::uint8_t>();
    return (bits[r >> 3] >> (r & 7)) & 1;
}

template <class T>
bool int_key(std::string& out, const df::Series& s, std::int64_t r) {
    const T v = s.data<T>()[r];
    if constexpr (std::is_signed_v<T>) {
        if (v < 0) {
            append_int_key(
                out, true,
                std::uint64_t{0} -
                    static_cast<std::uint64_t>(static_cast<std::int64_t>(v)));
            return true;
        }
    }
    append_int_key(out, false, static_cast<std::uint64_t>(v));
    return true;
}

// Row `r` of a FLAT column as the row evaluator reads it.
std::optional<Cell> cell_of(const df::Series& s, std::int64_t r) {
    if (s.is_null(r)) return Cell::null();
    switch (s.type()) {
        case df::TypeId::Int8:
        case df::TypeId::Int16:
        case df::TypeId::Int32:
        case df::TypeId::Int64:
        case df::TypeId::Uint8:
        case df::TypeId::Uint16:
        case df::TypeId::Uint32:
            return Cell{
                s.slice(r, 1).cast(df::TypeId::Int64).data<std::int64_t>()[0]};
        case df::TypeId::Uint64:
            return Cell{s.data<std::uint64_t>()[r]};
        case df::TypeId::Float32:
            return Cell{static_cast<double>(s.data<float>()[r])};
        case df::TypeId::Float64:
            return Cell{s.data<double>()[r]};
        case df::TypeId::Bool:
            return Cell{bit(s, r)};
        case df::TypeId::String:
        case df::TypeId::LargeString:
            return Cell{std::string(s.string_at(r))};
        default:
            return std::nullopt;
    }
}

}  // namespace

bool append_cell_key(std::string& out, const df::Series& s, std::int64_t r) {
    if (s.is_null(r)) return false;
    switch (s.type()) {
        case df::TypeId::Int8:
            return int_key<std::int8_t>(out, s, r);
        case df::TypeId::Int16:
            return int_key<std::int16_t>(out, s, r);
        case df::TypeId::Int32:
            return int_key<std::int32_t>(out, s, r);
        case df::TypeId::Int64:
            return int_key<std::int64_t>(out, s, r);
        case df::TypeId::Uint8:
            return int_key<std::uint8_t>(out, s, r);
        case df::TypeId::Uint16:
            return int_key<std::uint16_t>(out, s, r);
        case df::TypeId::Uint32:
            return int_key<std::uint32_t>(out, s, r);
        case df::TypeId::Uint64:
            return int_key<std::uint64_t>(out, s, r);
        case df::TypeId::Float32:
            return append_double_key(out,
                                     static_cast<double>(s.data<float>()[r]));
        case df::TypeId::Float64:
            return append_double_key(out, s.data<double>()[r]);
        case df::TypeId::Bool:
            append_bool_key(out, bit(s, r));
            return true;
        case df::TypeId::String:
        case df::TypeId::LargeString:
            append_string_key(out, s.string_at(r));
            return true;
        default:
            return false;
    }
}

df::DataFrame flat_frame(df::DataFrame f) {
    for (auto& c : f.columns)
        if (!c.is_flat()) c = c.materialize();
    return f;
}

namespace {

std::uint64_t series_bytes(const df::Series& s) {
    const std::int64_t n = s.length();
    std::uint64_t bytes =
        s.null_count() > 0 ? static_cast<std::uint64_t>((n + 7) / 8) : 0;
    switch (s.type()) {
        case df::TypeId::String:
        case df::TypeId::LargeString:
            bytes += static_cast<std::uint64_t>(n + 1) *
                     (s.type() == df::TypeId::String ? 4 : 8);
            for (std::int64_t r = 0; r < n; ++r) bytes += s.string_at(r).size();
            return bytes;
        case df::TypeId::List:
        case df::TypeId::LargeList:
            bytes += static_cast<std::uint64_t>(n + 1) * 4;
            [[fallthrough]];
        case df::TypeId::Struct:
            for (std::int64_t i = 0; i < s.num_children(); ++i)
                bytes += series_bytes(s.child(i));
            return bytes;
        default:
            return bytes + df::buffer_bytes(s.type(), n);
    }
}

}  // namespace

std::uint64_t frame_bytes(const df::DataFrame& f) {
    std::uint64_t bytes = 0;
    for (const auto& c : f.columns) bytes += series_bytes(c);
    return bytes;
}

std::optional<std::size_t> column_of(const df::DataFrame& f,
                                     std::string_view name) {
    for (std::size_t i = 0; i < f.names.size(); ++i)
        if (f.names[i] == name) return i;
    for (std::size_t i = 0; i < f.names.size(); ++i)
        if (f.names[i].size() == name.size() + 5 &&
            std::string_view(f.names[i]).starts_with("args.") &&
            std::string_view(f.names[i]).substr(5) == name)
            return i;
    return std::nullopt;
}

std::shared_ptr<const LookupTable> make_lookup_table(
    std::string name, std::shared_ptr<const df::DataFrame> frame,
    const std::vector<std::size_t>& columns) {
    auto t = std::make_shared<LookupTable>();
    t->name = std::move(name);
    const std::int64_t n = frame->num_rows();
    std::string key;
    for (std::int64_t r = 0; r < n; ++r) {
        key.clear();
        bool keyed = true;
        for (const auto c : columns)
            if (!append_cell_key(key, frame->columns[c], r)) {
                keyed = false;
                break;
            }
        if (keyed) t->rows[key].push_back(r);
    }
    t->frame = std::move(frame);
    t->key_columns = columns;
    return t;
}

std::shared_ptr<const LookupTable> make_lookup_table(
    const TLookup& l, std::shared_ptr<const df::DataFrame> frame) {
    const df::DataFrame& f = *frame;
    std::vector<std::size_t> columns;
    switch (l.kind) {
        case LookupKind::IN:
            if (f.columns.size() != l.keys.size())
                fail("'" + l.name + "' gives " +
                     std::to_string(f.columns.size()) +
                     " columns; 'in' compares " +
                     std::to_string(l.keys.size()));
            for (std::size_t i = 0; i < f.columns.size(); ++i)
                columns.push_back(i);
            break;
        case LookupKind::ARROW:
            for (const auto& target : l.target) {
                const auto c = column_of(f, target);
                if (!c)
                    fail("'" + l.name + "' has no key column '" + target +
                         "' for '->'");
                columns.push_back(*c);
            }
            break;
        case LookupKind::SCALAR:
            if (f.columns.size() != 1 || f.num_rows() > 1)
                fail("the sub-query '" + l.name + "' gives " +
                     std::to_string(f.num_rows()) + " rows and " +
                     std::to_string(f.columns.size()) +
                     " columns; a sub-query in an expression gives one "
                     "row and one column");
            break;
    }
    auto t = std::const_pointer_cast<LookupTable>(
        make_lookup_table(l.name, std::move(frame), columns));
    if (l.kind == LookupKind::ARROW) t->value = column_of(*t->frame, l.column);
    if (l.kind == LookupKind::SCALAR && !t->frame->columns.empty())
        t->value = 0;
    if (t->value) {
        const df::Series& v = t->frame->columns[*t->value];
        t->cells.reserve(static_cast<std::size_t>(v.length()));
        for (std::int64_t r = 0; r < v.length(); ++r)
            t->cells.push_back(cell_of(v, r));
    }
    return t;
}

std::string row_key(const std::vector<const df::Series*>& keys,
                    std::int64_t r) {
    std::string key;
    for (const auto* k : keys) append_cell_key(key, *k, r);
    return key;
}

Matches match(const LookupTable& table,
              const std::vector<const df::Series*>& keys, std::int64_t n) {
    Matches m;
    m.offsets.reserve(static_cast<std::size_t>(n) + 1);
    m.offsets.push_back(0);
    m.keyed.assign(static_cast<std::size_t>(n), 0);
    std::string key;
    for (std::int64_t r = 0; r < n; ++r) {
        key.clear();
        bool keyed = true;
        for (const auto* k : keys)
            if (!append_cell_key(key, *k, r)) {
                keyed = false;
                break;
            }
        if (keyed) {
            m.keyed[static_cast<std::size_t>(r)] = 1;
            if (const auto* hits = table.find(key))
                m.rows.insert(m.rows.end(), hits->begin(), hits->end());
        }
        m.offsets.push_back(static_cast<std::int64_t>(m.rows.size()));
    }
    return m;
}

bool same_cell(const df::Series& x, std::int64_t a, const df::Series& y,
               std::int64_t b) {
    std::string ka;
    std::string kb;
    const bool ha = append_cell_key(ka, x, a);
    const bool hb = append_cell_key(kb, y, b);
    if (!ha || !hb) return ha == hb && x.is_null(a) == y.is_null(b);
    return ka == kb;
}

df::Series lookup_column(const TLookup& t,
                         const std::vector<const df::Series*>& keys,
                         std::int64_t n) {
    const LookupTable* table = t.slot ? t.slot->table.get() : nullptr;
    if (!table) fail("'" + t.name + "' was read before its rows were bound");
    const df::DataFrame& f = *table->frame;
    if (t.kind == LookupKind::SCALAR) {
        if (f.num_rows() == 0 || f.columns.empty())
            return df::Series::nulls(df::TypeId::Bool, n);
        return f.columns[0].take(
            std::vector<std::int64_t>(static_cast<std::size_t>(n), 0));
    }
    const Matches m = match(*table, keys, n);
    if (t.kind == LookupKind::IN) {
        std::vector<std::uint8_t> out((static_cast<std::size_t>(n) + 7) / 8, 0);
        std::vector<std::uint8_t> valid(out.size(), 0);
        for (std::int64_t r = 0; r < n; ++r) {
            const auto i = static_cast<std::size_t>(r);
            if (!m.keyed[i]) continue;
            const bool hit = m.offsets[i + 1] > m.offsets[i];
            const auto b = static_cast<std::uint8_t>(1u << (r & 7));
            if (hit != t.negated) out[i >> 3] |= b;
            valid[i >> 3] |= b;
        }
        return df::Series::flat(df::TypeId::Bool, out.data(), n, valid.data());
    }
    if (!table->value) return df::Series::nulls(df::TypeId::Bool, n);
    const df::Series& value = f.columns[*table->value];
    std::vector<std::int64_t> pick(static_cast<std::size_t>(n), -1);
    for (std::int64_t r = 0; r < n; ++r) {
        const auto i = static_cast<std::size_t>(r);
        const std::int64_t lo = m.offsets[i];
        const std::int64_t hi = m.offsets[i + 1];
        if (lo == hi) continue;
        const std::int64_t first = m.rows[static_cast<std::size_t>(lo)];
        for (std::int64_t h = lo + 1; h < hi; ++h)
            if (!same_cell(value, first, value,
                           m.rows[static_cast<std::size_t>(h)]))
                conflict(*table, t.column, row_key(keys, r));
        pick[i] = first;
    }
    return value.take(pick);
}

}  // namespace dftracer::utils::duql
