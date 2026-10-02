#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/duql/decorrelate.h>
#include <dftracer/utils/duql/vectorize.h>
#include <dftracer/utils/json/record_parser.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
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

// The key of a JSON column's cell: its parsed scalar, as a typed column of
// that value would key it. Containers have none.
bool append_json_key(std::string& out, std::string_view text) {
    thread_local dftracer::utils::json::RecordParser parser;
    simdjson::dom::element el;
    if (parser.parse(text.data(), text.size()).get(el) != simdjson::SUCCESS)
        return false;
    switch (el.type()) {
        case simdjson::dom::element_type::INT64: {
            const std::int64_t v = el.get_int64();
            append_int_key(
                out, v < 0,
                v < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(v)
                      : static_cast<std::uint64_t>(v));
            return true;
        }
        case simdjson::dom::element_type::UINT64:
            append_int_key(out, false, el.get_uint64());
            return true;
        case simdjson::dom::element_type::DOUBLE:
            return append_double_key(out, el.get_double());
        case simdjson::dom::element_type::STRING:
            append_string_key(out, el.get_string().value());
            return true;
        case simdjson::dom::element_type::BOOL:
            append_bool_key(out, el.get_bool());
            return true;
        default:
            return false;
    }
}

// Row `r` of a FLAT column as duql orders it; nullopt for null, NaN and
// containers. A JSON cell's string is kept in `owned`.
std::optional<Ordered> ordered_of(const df::Series& s, std::int64_t r,
                                  std::deque<std::string>& owned) {
    if (s.is_null(r)) return std::nullopt;
    auto real = [](double d) -> std::optional<Ordered> {
        if (std::isnan(d)) return std::nullopt;
        return Ordered{d};
    };
    auto whole = [&](auto v) { return Ordered{static_cast<std::int64_t>(v)}; };
    switch (s.type()) {
        case df::TypeId::Int8:
            return whole(s.data<std::int8_t>()[r]);
        case df::TypeId::Int16:
            return whole(s.data<std::int16_t>()[r]);
        case df::TypeId::Int32:
            return whole(s.data<std::int32_t>()[r]);
        case df::TypeId::Int64:
            return whole(s.data<std::int64_t>()[r]);
        case df::TypeId::Uint8:
            return whole(s.data<std::uint8_t>()[r]);
        case df::TypeId::Uint16:
            return whole(s.data<std::uint16_t>()[r]);
        case df::TypeId::Uint32:
            return whole(s.data<std::uint32_t>()[r]);
        case df::TypeId::Uint64:
            return Ordered{s.data<std::uint64_t>()[r]};
        case df::TypeId::Float32:
            return real(static_cast<double>(s.data<float>()[r]));
        case df::TypeId::Float64:
            return real(s.data<double>()[r]);
        case df::TypeId::Bool:
            return Ordered{bit(s, r)};
        case df::TypeId::String:
        case df::TypeId::LargeString:
            break;
        default:
            return std::nullopt;
    }
    if (!s.is_json()) return Ordered{s.string_at(r)};
    thread_local dftracer::utils::json::RecordParser parser;
    const std::string_view text = s.string_at(r);
    simdjson::dom::element el;
    if (parser.parse(text.data(), text.size()).get(el) != simdjson::SUCCESS)
        return std::nullopt;
    switch (el.type()) {
        case simdjson::dom::element_type::INT64:
            return Ordered{el.get_int64().value()};
        case simdjson::dom::element_type::UINT64:
            return Ordered{el.get_uint64().value()};
        case simdjson::dom::element_type::DOUBLE:
            return real(el.get_double().value());
        case simdjson::dom::element_type::STRING:
            owned.emplace_back(el.get_string().value());
            return Ordered{std::string_view(owned.back())};
        case simdjson::dom::element_type::BOOL:
            return Ordered{el.get_bool().value()};
        default:
            return std::nullopt;
    }
}

bool integral(df::TypeId t) {
    switch (t) {
        case df::TypeId::Int8:
        case df::TypeId::Int16:
        case df::TypeId::Int32:
        case df::TypeId::Int64:
        case df::TypeId::Uint8:
        case df::TypeId::Uint16:
        case df::TypeId::Uint32:
        case df::TypeId::Uint64:
        case df::TypeId::Bool:
            return true;
        default:
            return false;
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
            if (s.is_json()) return append_json_key(out, s.string_at(r));
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
    if (std::any_of(frame->columns.begin(), frame->columns.end(),
                    [](const df::Series& c) { return !c.is_flat(); })) {
        df::DataFrame flat;
        flat.names = frame->names;
        for (const df::Series& c : frame->columns)
            flat.columns.push_back(c.is_flat() ? c.share() : c.materialize());
        frame = std::make_shared<const df::DataFrame>(std::move(flat));
    }
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

namespace {

// The range index of `t`, whose `rows` are its keys' rows, over the range
// column `range` and the value column `value`; it takes over `t.rows`.
void index_range(LookupTable& t, RangeRead read, std::size_t range,
                 std::optional<std::size_t> value) {
    auto ix = std::make_shared<RangeIndex>();
    const df::DataFrame& f = *t.frame;
    const df::Series& by = f.columns[range];
    const df::Series* arg = value ? &f.columns[*value] : nullptr;
    const bool tree = read != RangeRead::ROWS && read != RangeRead::COUNT;
    if ((read == RangeRead::SUM || read == RangeRead::MEAN) && arg) {
        const df::TypeId type = arg->type();
        const bool number = integral(type) || type == df::TypeId::Float32 ||
                            type == df::TypeId::Float64 ||
                            arg->null_count() == arg->length();
        if (!number || arg->is_json())
            fail("'" + t.name +
                 "' sums a column that is not a typed number; cast it with "
                 "int() or float() in the sub-query");
        ix->integral = integral(type);
    }
    std::vector<std::pair<Ordered, std::int64_t>> sorted;
    std::vector<RangeIndex::Node> leaves;
    for (auto& [key, rows] : t.rows) {
        sorted.clear();
        for (const std::int64_t r : rows)
            if (auto v = ordered_of(by, r, ix->owned))
                sorted.emplace_back(std::move(*v), r);
        if (sorted.empty()) continue;
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const auto& a, const auto& b) {
                             return compare_ordered(a.first, b.first) < 0;
                         });
        const auto first = static_cast<std::int64_t>(ix->rows.size());
        for (auto& [v, r] : sorted) {
            const auto at = static_cast<std::int64_t>(ix->rows.size());
            ix->rows.push_back(r);
            ix->values.push_back(v);
            if (!tree) continue;
            RangeIndex::Node leaf;
            if (arg && !arg->is_null(r)) {
                switch (read) {
                    case RangeRead::COUNT_VALUES:
                        leaf.count = 1;
                        break;
                    case RangeRead::COUNT_IF: {
                        const auto b = ordered_of(*arg, r, ix->owned);
                        const auto* yes = b ? std::get_if<bool>(&*b) : nullptr;
                        leaf.count = yes && *yes ? 1 : 0;
                        break;
                    }
                    case RangeRead::SUM:
                    case RangeRead::MEAN:
                        leaf.count = 1;
                        if (ix->integral)
                            leaf.isum = std::visit(
                                [](const auto& x) -> RangeWide {
                                    using T = std::decay_t<decltype(x)>;
                                    if constexpr (std::is_same_v<T, bool>)
                                        return x ? 1 : 0;
                                    else if constexpr (std::is_same_v<
                                                           T, std::int64_t> ||
                                                       std::is_same_v<
                                                           T, std::uint64_t>)
                                        return static_cast<RangeWide>(x);
                                    else
                                        return 0;
                                },
                                *ordered_of(*arg, r, ix->owned));
                        else if (auto d = ordered_of(*arg, r, ix->owned))
                            leaf.fsum = std::get<double>(*d);
                        else
                            leaf.fsum = std::nan("");
                        break;
                    case RangeRead::MIN:
                    case RangeRead::MAX:
                        ix->args.resize(ix->rows.size());
                        ix->args.back() = ordered_of(*arg, r, ix->owned);
                        if (ix->args.back()) leaf.best = at;
                        break;
                    case RangeRead::NONE:
                    case RangeRead::ROWS:
                    case RangeRead::COUNT:
                        break;
                }
            }
            leaves.push_back(leaf);
        }
        ix->keys.emplace(
            key,
            std::make_pair(first, static_cast<std::int64_t>(ix->rows.size())));
    }
    ix->args.resize(ix->rows.size());
    if (tree) build_range_tree(*ix, read, leaves);
    t.rows.clear();
    t.range = std::move(ix);
}

}  // namespace

std::shared_ptr<const LookupTable> make_lookup_table(
    const TLookup& l, std::shared_ptr<const df::DataFrame> frame) {
    if (joined(l))
        fail("'" + l.name + "' reads its rows through a join, not a table");
    const df::DataFrame& f = *frame;
    std::vector<std::size_t> columns;
    std::optional<std::size_t> range;
    if (l.range != RangeRead::NONE) {
        range = column_of(f, CORRELATED_RANGE);
        if (!range)
            fail("'" + l.name + "' has no column '" + CORRELATED_RANGE + "'");
    }
    switch (l.kind) {
        case LookupKind::IN:
            if (!range && f.columns.size() != l.keys.size())
                fail("'" + l.name + "' gives " +
                     std::to_string(f.columns.size()) +
                     " columns; 'in' compares " +
                     std::to_string(l.keys.size() - l.correlated));
            for (std::size_t i = 0; i < f.columns.size(); ++i)
                if (!range || i != *range) columns.push_back(i);
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
            if (!l.keys.empty()) {
                for (const auto& target : l.target) {
                    const auto c = column_of(f, target);
                    if (!c)
                        fail("'" + l.name + "' has no key column '" + target +
                             "'");
                    columns.push_back(*c);
                }
            } else if (f.columns.size() != 1 || f.num_rows() > 1)
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
    if (l.kind == LookupKind::SCALAR && l.keys.empty() &&
        !t->frame->columns.empty())
        t->value = 0;
    if (l.kind == LookupKind::SCALAR && !l.keys.empty())
        t->value = column_of(*t->frame, l.column);
    if (range) index_range(*t, l.range, *range, t->value);
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

namespace {

df::Series range_column(const TLookup& t, const LookupTable& table,
                        const std::vector<const df::Series*>& keys,
                        std::int64_t n) {
    const std::size_t bounds = (t.low ? 1 : 0) + (t.high ? 1 : 0);
    const std::size_t keyed = keys.size() - bounds;
    const std::size_t subject = keys.size() - t.correlated;
    const bool in = t.kind == LookupKind::IN;
    const bool picks =
        !in && (t.range == RangeRead::ROWS || t.range == RangeRead::MIN ||
                t.range == RangeRead::MAX);
    const bool reals = t.range == RangeRead::MEAN ||
                       (t.range == RangeRead::SUM && !table.range->integral);
    const auto rows = static_cast<std::size_t>(n);
    std::vector<std::uint8_t> bits((rows + 7) / 8, 0);
    std::vector<std::uint8_t> valid(bits.size(), 0);
    std::vector<std::int64_t> pick(picks ? rows : 0, -1);
    std::vector<std::int64_t> ints(!in && !picks && !reals ? rows : 0, 0);
    std::vector<double> doubles(reals ? rows : 0, 0);
    std::string key;
    std::deque<std::string> owned;
    for (std::int64_t r = 0; r < n; ++r) {
        const auto i = static_cast<std::size_t>(r);
        key.clear();
        owned.clear();
        std::size_t known = 0;
        while (known < keyed && append_cell_key(key, *keys[known], r)) ++known;
        if (in && known < subject) continue;
        std::optional<Ordered> low;
        std::optional<Ordered> high;
        std::size_t b = keyed;
        if (t.low) low = ordered_of(*keys[b++], r, owned);
        if (t.high) high = ordered_of(*keys[b], r, owned);
        RangeAnswer a;
        if (known == keyed) {
            a = range_answer(t, table, key, low, high);
        } else if (t.range == RangeRead::COUNT ||
                   t.range == RangeRead::COUNT_VALUES ||
                   t.range == RangeRead::COUNT_IF) {
            a.number = Number{std::int64_t{0}};
        }
        const auto bit = static_cast<std::uint8_t>(1u << (r & 7));
        if (in) {
            if ((a.rows > 0) != t.negated) bits[i >> 3] |= bit;
            valid[i >> 3] |= bit;
        } else if (picks) {
            pick[i] = a.row;
        } else if (a.number) {
            if (reals)
                doubles[i] = std::get<double>(*a.number);
            else
                ints[i] = std::get<std::int64_t>(*a.number);
            valid[i >> 3] |= bit;
        }
    }
    if (in)
        return df::Series::flat(df::TypeId::Bool, bits.data(), n, valid.data());
    if (picks) {
        if (!table.value) return df::Series::nulls(df::TypeId::Bool, n);
        return table.frame->columns[*table.value].take(pick);
    }
    if (reals) return df::Series::flat_f64(doubles.data(), n, valid.data());
    return df::Series::flat_i64(ints.data(), n, valid.data());
}

}  // namespace

df::Series lookup_column(const TLookup& t,
                         const std::vector<const df::Series*>& keys,
                         std::int64_t n) {
    const LookupTable* table = t.slot ? t.slot->table.get() : nullptr;
    if (!table) fail("'" + t.name + "' was read before its rows were bound");
    if (t.range != RangeRead::NONE) return range_column(t, *table, keys, n);
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
        const std::size_t subject = t.keys.size() - t.correlated;
        std::string scratch;
        for (std::int64_t r = 0; r < n; ++r) {
            const auto i = static_cast<std::size_t>(r);
            bool known = true;
            for (std::size_t k = 0; k < subject && known; ++k) {
                scratch.clear();
                known = append_cell_key(scratch, *keys[k], r);
            }
            if (!known) continue;
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

df::Series in_column(const std::vector<const df::Series*>& keys,
                     std::size_t subject, const df::Series& matched,
                     bool negated) {
    const std::int64_t n = matched.length();
    std::vector<std::uint8_t> out((static_cast<std::size_t>(n) + 7) / 8, 0);
    std::vector<std::uint8_t> valid(out.size(), 0);
    std::string scratch;
    for (std::int64_t r = 0; r < n; ++r) {
        bool known = true;
        for (std::size_t k = 0; k < subject && known; ++k) {
            scratch.clear();
            known = append_cell_key(scratch, *keys[k], r);
        }
        if (!known) continue;
        const auto i = static_cast<std::size_t>(r);
        const auto b = static_cast<std::uint8_t>(1u << (r & 7));
        if (matched.is_null(r) == negated) out[i >> 3] |= b;
        valid[i >> 3] |= b;
    }
    return df::Series::flat(df::TypeId::Bool, out.data(), n, valid.data());
}

df::Series scalar_column(const std::string& name,
                         const std::vector<const df::Series*>& keys,
                         const df::Series& nest, std::size_t field,
                         const df::DataFrame* empty) {
    const std::int64_t n = nest.length();
    const std::int32_t* off = nest.offsets();
    df::DataFrame values;
    values.names = {CORRELATED_VALUE};
    values.columns.push_back(
        nest.child(0).child(static_cast<std::int64_t>(field)));
    std::int64_t none = -1;
    if (empty) {
        if (empty->num_rows() != 1)
            fail("'" + name + "' over no rows gives " +
                 std::to_string(empty->num_rows()) + " rows, not one");
        none = values.num_rows();
        values = df::concat({&values, empty}, df::ConcatHow::Diagonal);
    }
    std::vector<std::int64_t> pick(static_cast<std::size_t>(n), none);
    for (std::int64_t r = 0; r < n; ++r) {
        const std::int64_t rows = nest.is_null(r) ? 0 : off[r + 1] - off[r];
        if (rows > 1)
            scalar_conflict(name, row_key(keys, r),
                            static_cast<std::size_t>(rows));
        if (rows == 1) pick[static_cast<std::size_t>(r)] = off[r];
    }
    const auto v = column_of(values, CORRELATED_VALUE);
    return values.columns[*v].take(pick);
}

}  // namespace dftracer::utils::duql
