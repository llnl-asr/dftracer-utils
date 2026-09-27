#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/field_ref.h>
#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/lookup.h>
#include <dftracer/utils/duql/numbers.h>
#include <dftracer/utils/duql/pattern.h>
#include <dftracer/utils/duql/pattern_engine.h>
#include <dftracer/utils/duql/term.h>
#include <dftracer/utils/json/canonical.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/json/record_parser.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dftracer::utils::duql {

namespace {

Truth of(bool b) { return b ? Truth::YES : Truth::NO; }

Truth t_not(Truth t) {
    return t == Truth::YES  ? Truth::NO
           : t == Truth::NO ? Truth::YES
                            : Truth::UNKNOWN;
}

Truth t_and(Truth l, Truth r) {
    if (l == Truth::NO || r == Truth::NO) return Truth::NO;
    return l == Truth::YES && r == Truth::YES ? Truth::YES : Truth::UNKNOWN;
}

Truth t_or(Truth l, Truth r) {
    if (l == Truth::YES || r == Truth::YES) return Truth::YES;
    return l == Truth::NO && r == Truth::NO ? Truth::NO : Truth::UNKNOWN;
}

using dftracer::utils::duql::compare_numbers;
using dftracer::utils::duql::detail::order;

// -1/0/1 for less/equal/greater; nullopt when the types differ.
std::optional<int> compare(const JsonValue& field, const LiteralNode& lit) {
    return std::visit(
        [&field](const auto& v) -> std::optional<int> {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                if (!field.is_string()) return std::nullopt;
                return order(field.get<std::string_view>(),
                             std::string_view(v));
            } else if constexpr (std::is_same_v<T, bool>) {
                if (!field.is_bool()) return std::nullopt;
                return order(field.get<bool>(), v);
            } else {
                if (field.is_int())
                    return compare_numbers(field.get<std::int64_t>(), v);
                if (field.is_uint())
                    return compare_numbers(field.get<std::uint64_t>(), v);
                if (field.is_number())
                    return compare_numbers(field.get<double>(), v);
                return std::nullopt;
            }
        },
        lit.value);
}

std::optional<int> compare(const LiteralValue& a, const LiteralNode& lit) {
    return std::visit(
        [](const auto& va, const auto& vb) -> std::optional<int> {
            using A = std::decay_t<decltype(va)>;
            using B = std::decay_t<decltype(vb)>;
            if constexpr (std::is_same_v<A, B>)
                return order(va, vb);
            else if constexpr (std::is_arithmetic_v<A> &&
                               std::is_arithmetic_v<B> &&
                               !std::is_same_v<A, bool> &&
                               !std::is_same_v<B, bool>)
                return compare_numbers(va, vb);
            else
                return std::nullopt;
        },
        a, lit.value);
}

std::optional<std::string_view> as_string(const JsonValue& v) {
    if (!v.is_string()) return std::nullopt;
    return v.get<std::string_view>();
}

std::optional<std::string_view> as_string(const LiteralValue& v) {
    if (const auto* s = std::get_if<std::string>(&v)) return *s;
    return std::nullopt;
}

bool plain_string(const JsonValue& v) { return v.is_string(); }
bool plain_string(const LiteralValue& v) {
    return std::holds_alternative<std::string>(v);
}

// YES when the value equals an element, NO when it is comparable with some
// element and equals none, UNKNOWN when it is comparable with none.
template <class V>
Truth in_values(const ArrayNode& values, const V& v) {
    if (values.set) {
        if (!plain_string(v)) return Truth::UNKNOWN;
        return of(values.set->values.contains(*as_string(v)));
    }
    bool comparable = false;
    for (const auto& elem : values.elements) {
        const auto c = compare(v, elem);
        if (!c) continue;
        if (*c == 0) return Truth::YES;
        comparable = true;
    }
    return comparable ? Truth::NO : Truth::UNKNOWN;
}

Truth apply(CompareOp op, std::optional<int> c) {
    if (!c) return Truth::UNKNOWN;
    switch (op) {
        case CompareOp::EQ:
            return of(*c == 0);
        case CompareOp::NE:
            return of(*c != 0);
        case CompareOp::GT:
            return of(*c > 0);
        case CompareOp::LT:
            return of(*c < 0);
        case CompareOp::GE:
            return of(*c >= 0);
        case CompareOp::LE:
            return of(*c <= 0);
    }
    return Truth::UNKNOWN;
}

// A match reaching its work limit is UNKNOWN.
Truth pattern_truth(const CompiledPattern& p, std::string_view s) {
    switch (match(p, s)) {
        case MatchResult::YES:
            return Truth::YES;
        case MatchResult::NO:
            return Truth::NO;
        case MatchResult::LIMIT:
            break;
    }
    return Truth::UNKNOWN;
}

struct VMissing {};
struct VNull {};

// A value during expression evaluation. Strings and containers borrow from
// the record, the term or the scratch arena, and live for one leaf.
using Value = std::variant<VMissing, VNull, bool, std::int64_t, std::uint64_t,
                           double, std::string_view, simdjson::dom::element>;

bool absent(const Value& v) {
    return std::holds_alternative<VMissing>(v) ||
           std::holds_alternative<VNull>(v);
}

// Owned strings and parsed containers of one expression leaf.
struct Scratch {
    std::deque<std::string> strings;
    std::vector<std::unique_ptr<dftracer::utils::json::RecordParser>> parsers;
    std::size_t next_parser = 0;

    std::string_view own(std::string s) {
        return strings.emplace_back(std::move(s));
    }

    Value parse(std::string_view json) {
        if (next_parser == parsers.size())
            parsers.push_back(
                std::make_unique<dftracer::utils::json::RecordParser>());
        simdjson::dom::element el;
        if (parsers[next_parser++]->parse(json.data(), json.size()).get(el) !=
            simdjson::SUCCESS)
            return VMissing{};
        return el;
    }

    void reset() {
        strings.clear();
        next_parser = 0;
    }
};

thread_local Scratch scratch;

Value of_element(simdjson::dom::element e) {
    using T = simdjson::dom::element_type;
    switch (e.type()) {
        case T::ARRAY:
        case T::OBJECT:
            return e;
        case T::INT64:
            return e.get_int64().value_unsafe();
        case T::UINT64:
            return e.get_uint64().value_unsafe();
        case T::DOUBLE:
            return e.get_double().value_unsafe();
        case T::STRING:
            return e.get_string().value_unsafe();
        case T::BOOL:
            return e.get_bool().value_unsafe();
        case T::NULL_VALUE:
        // An integer past uint64 has no value here; it compares as unknown,
        // as on the literal-leaf path.
        case T::BIGINT:
            return VNull{};
    }
    return VMissing{};
}

Value of_cell(const Cell& c) {
    switch (c.kind) {
        case Cell::Kind::NULL_VALUE:
            return VNull{};
        case Cell::Kind::ARRAY:
        case Cell::Kind::OBJECT:
            return scratch.parse(std::get<std::string>(c.value));
        case Cell::Kind::VALUE:
            break;
    }
    return std::visit(
        [](const auto& v) -> Value {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>)
                return std::string_view(v);
            else
                return v;
        },
        c.value);
}

// Walks the steps of `f` past its base, or every step of an element path.
Value walk(Value v, const TField& f) {
    const bool element = f.root == FieldRoot::ELEMENT;
    for (std::size_t i = f.neg_at; i < f.steps.size(); ++i) {
        const auto& step = f.steps[i];
        if ((i != f.neg_at || element) && !step.key.empty()) {
            const auto* el = std::get_if<simdjson::dom::element>(&v);
            simdjson::dom::object obj;
            simdjson::dom::element child;
            if (!el || el->get_object().get(obj) != simdjson::SUCCESS ||
                obj.at_key(step.key).get(child) != simdjson::SUCCESS)
                return VMissing{};
            v = of_element(child);
        }
        if (step.index) {
            const auto* el = std::get_if<simdjson::dom::element>(&v);
            simdjson::dom::array arr;
            if (!el || el->get_array().get(arr) != simdjson::SUCCESS)
                return VMissing{};
            const auto size = static_cast<std::int64_t>(arr.size());
            const std::int64_t at =
                *step.index < 0 ? size + *step.index : *step.index;
            simdjson::dom::element child;
            if (at < 0 || at >= size ||
                arr.at(static_cast<std::size_t>(at)).get(child) !=
                    simdjson::SUCCESS)
                return VMissing{};
            v = of_element(child);
        }
    }
    return v;
}

// -1/0/1, or nullopt across types and for missing, null and containers.
std::optional<int> cmp(const Value& a, const Value& b) {
    return std::visit(
        [](const auto& x, const auto& y) -> std::optional<int> {
            using A = std::decay_t<decltype(x)>;
            using B = std::decay_t<decltype(y)>;
            constexpr bool NUM_A = std::is_same_v<A, std::int64_t> ||
                                   std::is_same_v<A, std::uint64_t> ||
                                   std::is_same_v<A, double>;
            constexpr bool NUM_B = std::is_same_v<B, std::int64_t> ||
                                   std::is_same_v<B, std::uint64_t> ||
                                   std::is_same_v<B, double>;
            if constexpr (NUM_A && NUM_B)
                return compare_numbers(x, y);
            else if constexpr (std::is_same_v<A, B> &&
                               (std::is_same_v<A, bool> ||
                                std::is_same_v<A, std::string_view>))
                return order(x, y);
            else
                return std::nullopt;
        },
        a, b);
}

Truth truth(const Value& v) {
    if (const auto* b = std::get_if<bool>(&v)) return of(*b);
    return Truth::UNKNOWN;
}

Value of_truth(Truth t) {
    if (t == Truth::UNKNOWN) return VNull{};
    return t == Truth::YES;
}

// An integer as sign and magnitude, so int64 and uint64 mix without loss.
struct Int {
    bool neg = false;
    std::uint64_t mag = 0;
};

constexpr std::uint64_t U64_MAX = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t I64_MIN_MAG = std::uint64_t{1} << 63;

std::optional<Int> as_int(const Value& v) {
    if (const auto* i = std::get_if<std::int64_t>(&v)) {
        if (*i < 0)
            return Int{true, std::uint64_t{0} - static_cast<std::uint64_t>(*i)};
        return Int{false, static_cast<std::uint64_t>(*i)};
    }
    if (const auto* u = std::get_if<std::uint64_t>(&v)) return Int{false, *u};
    return std::nullopt;
}

std::optional<double> as_double(const Value& v) {
    if (const auto* d = std::get_if<double>(&v)) return *d;
    if (const auto* i = std::get_if<std::int64_t>(&v))
        return static_cast<double>(*i);
    if (const auto* u = std::get_if<std::uint64_t>(&v))
        return static_cast<double>(*u);
    return std::nullopt;
}

// An arithmetic result outside int64 is unknown, as in a column.
Value of_int(Int x) {
    if (x.mag == 0) return std::int64_t{0};
    if (!x.neg) {
        if (x.mag <= static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max()))
            return static_cast<std::int64_t>(x.mag);
        return VNull{};
    }
    if (x.mag > I64_MIN_MAG) return VNull{};
    return static_cast<std::int64_t>(std::uint64_t{0} - x.mag);
}

Value of_double(double d) {
    if (std::isnan(d)) return VNull{};
    return d;
}

std::optional<Int> add(Int a, Int b) {
    if (a.neg == b.neg) {
        if (a.mag > U64_MAX - b.mag) return std::nullopt;
        return Int{a.neg, a.mag + b.mag};
    }
    if (a.mag >= b.mag) return Int{a.neg, a.mag - b.mag};
    return Int{b.neg, b.mag - a.mag};
}

std::optional<Int> mul(Int a, Int b) {
    if (a.mag != 0 && b.mag > U64_MAX / a.mag) return std::nullopt;
    return Int{a.neg != b.neg, a.mag * b.mag};
}

Value arith(TermOp op, const Value& l, const Value& r) {
    const auto li = as_int(l);
    const auto ri = as_int(r);
    if (li && ri && op != TermOp::DIV) {
        const Int a = *li;
        const Int b = *ri;
        std::optional<Int> out;
        switch (op) {
            case TermOp::ADD:
                out = add(a, b);
                break;
            case TermOp::SUB:
                out = add(a, Int{!b.neg, b.mag});
                break;
            case TermOp::MUL:
                out = mul(a, b);
                break;
            case TermOp::IDIV: {
                if (b.mag == 0) return VNull{};
                Int q{a.neg != b.neg, a.mag / b.mag};
                if (q.neg && a.mag % b.mag != 0) ++q.mag;
                out = q;
                break;
            }
            case TermOp::MOD: {
                if (b.mag == 0) return VNull{};
                std::uint64_t rem = a.mag % b.mag;
                if (rem != 0 && a.neg != b.neg) rem = b.mag - rem;
                out = Int{b.neg, rem};
                break;
            }
            default:
                break;
        }
        if (!out) return VNull{};
        return of_int(*out);
    }
    const auto a = as_double(l);
    const auto b = as_double(r);
    if (!a || !b) return VNull{};
    switch (op) {
        case TermOp::ADD:
            return of_double(*a + *b);
        case TermOp::SUB:
            return of_double(*a - *b);
        case TermOp::MUL:
            return of_double(*a * *b);
        case TermOp::DIV:
            if (*b == 0) return VNull{};
            return of_double(*a / *b);
        case TermOp::IDIV:
            if (*b == 0) return VNull{};
            return of_double(std::floor(*a / *b));
        case TermOp::MOD: {
            if (*b == 0) return VNull{};
            double rem = std::fmod(*a, *b);
            if (rem != 0 && (rem < 0) != (*b < 0)) rem += *b;
            return of_double(rem);
        }
        default:
            return VNull{};
    }
}

void append_double(std::string& out, double d) {
    char buf[32];
    if (char* end = to_chars_double(buf, buf + sizeof(buf), d))
        out.append(buf, end);
}

// The text of a present value: strings as they are, containers as canonical
// JSON; `json_strings` also quotes strings and writes null.
std::optional<std::string> text_of(const Value& v, bool json_strings) {
    return std::visit(
        [json_strings](const auto& x) -> std::optional<std::string> {
            using T = std::decay_t<decltype(x)>;
            std::string out;
            if constexpr (std::is_same_v<T, VMissing>) {
                return std::nullopt;
            } else if constexpr (std::is_same_v<T, VNull>) {
                if (!json_strings) return std::nullopt;
                out = "null";
            } else if constexpr (std::is_same_v<T, bool>) {
                out = x ? "true" : "false";
            } else if constexpr (std::is_same_v<T, double>) {
                append_double(out, x);
            } else if constexpr (std::is_same_v<T, std::string_view>) {
                if (json_strings) {
                    out += '"';
                    json::append_json_escaped(out, x);
                    out += '"';
                } else {
                    out = x;
                }
            } else if constexpr (std::is_same_v<T, simdjson::dom::element>) {
                json::append_canonical_json(out, x);
            } else {
                out = std::to_string(x);
            }
            return out;
        },
        v);
}

bool is_array(const Value& v, simdjson::dom::array& out) {
    const auto* el = std::get_if<simdjson::dom::element>(&v);
    return el && el->get_array().get(out) == simdjson::SUCCESS;
}

bool continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

std::size_t utf8_length(std::string_view s) {
    return static_cast<std::size_t>(std::count_if(
        s.begin(), s.end(), [](char c) { return !continuation(c); }));
}

// Byte offset of code point `n` in `s`, or s.size() past the end.
std::size_t utf8_offset(std::string_view s, std::uint64_t n) {
    std::size_t i = 0;
    while (i < s.size() && n > 0) {
        ++i;
        while (i < s.size() && continuation(s[i])) ++i;
        --n;
    }
    return i;
}

std::string_view type_name(const Value& v) {
    return std::visit(
        [](const auto& x) -> std::string_view {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, VMissing>)
                return "missing";
            else if constexpr (std::is_same_v<T, VNull>)
                return "null";
            else if constexpr (std::is_same_v<T, bool>)
                return "bool";
            else if constexpr (std::is_same_v<T, std::string_view>)
                return "string";
            else if constexpr (std::is_same_v<T, simdjson::dom::element>)
                return x.is_array() ? "array" : "object";
            else
                return "number";
        },
        v);
}

template <class V>
std::optional<V> parse_number(std::string_view s) {
    V out{};
    std::from_chars_result res{};
    if constexpr (std::is_same_v<V, double>)
        res = from_chars_double(s.data(), s.data() + s.size(), out);
    else
        res = std::from_chars(s.data(), s.data() + s.size(), out);
    if (res.ec != std::errc{} || res.ptr != s.data() + s.size())
        return std::nullopt;
    return out;
}

// An int64, as a column converts; anything outside int64 is unknown.
Value to_int(const Value& v) {
    if (const auto i = as_int(v)) return of_int(*i);
    if (const auto* b = std::get_if<bool>(&v)) return std::int64_t{*b};
    std::optional<double> d;
    if (const auto* x = std::get_if<double>(&v)) d = *x;
    if (const auto* s = std::get_if<std::string_view>(&v)) {
        if (auto i = parse_number<std::int64_t>(*s)) return *i;
        d = parse_number<double>(*s);
    }
    if (!d || !std::isfinite(*d)) return VNull{};
    const double t = std::trunc(*d);
    if (t >= -9223372036854775808.0 && t < 9223372036854775808.0)
        return static_cast<std::int64_t>(t);
    return VNull{};
}

Value to_float(const Value& v) {
    if (const auto d = as_double(v)) return *d;
    if (const auto* b = std::get_if<bool>(&v)) return *b ? 1.0 : 0.0;
    if (const auto* s = std::get_if<std::string_view>(&v))
        if (auto d = parse_number<double>(*s)) return of_double(*d);
    return VNull{};
}

Value round_to(const Value& v, const Value& digits) {
    const auto d = as_int(digits);
    if (!d) return VNull{};
    if (as_int(v) && !d->neg) return v;
    const auto x = as_double(v);
    if (!x || d->mag > 308) return VNull{};
    const double scale = std::pow(10.0, d->neg ? -static_cast<double>(d->mag)
                                               : static_cast<double>(d->mag));
    const double r = std::round(*x * scale) / scale;
    if (as_int(v)) return to_int(Value{r});
    return of_double(r);
}

Value length(const Value& v) {
    if (const auto* s = std::get_if<std::string_view>(&v))
        return static_cast<std::int64_t>(utf8_length(*s));
    if (const auto* el = std::get_if<simdjson::dom::element>(&v)) {
        simdjson::dom::array arr;
        if (el->get_array().get(arr) == simdjson::SUCCESS)
            return static_cast<std::int64_t>(arr.size());
        simdjson::dom::object obj;
        if (el->get_object().get(obj) == simdjson::SUCCESS)
            return static_cast<std::int64_t>(obj.size());
    }
    return VNull{};
}

Value contains(const Value& a, const Value& b) {
    if (absent(b)) return VNull{};
    if (const auto* s = std::get_if<std::string_view>(&a)) {
        const auto* p = std::get_if<std::string_view>(&b);
        if (!p) return VNull{};
        return s->find(*p) != std::string_view::npos;
    }
    simdjson::dom::array arr;
    if (!is_array(a, arr)) return VNull{};
    for (auto el : arr) {
        const auto o = cmp(of_element(el), b);
        if (o && *o == 0) return true;
    }
    return false;
}

Value sum(const Value& v) {
    simdjson::dom::array arr;
    if (!is_array(v, arr)) return VNull{};
    Int total;
    double dtotal = 0;
    bool is_double = false;
    for (auto el : arr) {
        const Value x = of_element(el);
        if (!is_double) {
            if (const auto i = as_int(x)) {
                const auto next = add(total, *i);
                if (!next) return VNull{};
                total = *next;
                continue;
            }
            const auto t = as_double(of_int(total));
            if (!t) return VNull{};
            dtotal = *t;
            is_double = true;
        }
        const auto d = as_double(x);
        if (!d) return VNull{};
        dtotal += *d;
    }
    return is_double ? of_double(dtotal) : of_int(total);
}

// The lookup key of `v`; false for a value with none.
bool value_key(std::string& out, const Value& v) {
    return std::visit(
        [&out](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::int64_t>) {
                append_int_key(
                    out, x < 0,
                    x < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(x)
                          : static_cast<std::uint64_t>(x));
                return true;
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                append_int_key(out, false, x);
                return true;
            } else if constexpr (std::is_same_v<T, double>) {
                return append_double_key(out, x);
            } else if constexpr (std::is_same_v<T, std::string_view>) {
                append_string_key(out, x);
                return true;
            } else if constexpr (std::is_same_v<T, bool>) {
                append_bool_key(out, x);
                return true;
            } else {
                return false;
            }
        },
        v);
}

// Row `r` of the column `t` reads; the table outlives the leaf.
Value table_value(const LookupTable& t, std::int64_t r) {
    const auto& c = t.cells[static_cast<std::size_t>(r)];
    if (!c)
        throw DFTUtilsException(
            ErrorCode::INVALID_ARGUMENT,
            "View::duql a scan filter reads a list or object column of '" +
                t.name + "'; read it in a stage after the scan filter");
    return of_cell(*c);
}

// Rows `a` and `b` of the column `t` reads hold equal values.
bool same_value(const LookupTable& t, std::int64_t a, std::int64_t b) {
    std::string ka;
    std::string kb;
    const bool ha = value_key(ka, table_value(t, a));
    const bool hb = value_key(kb, table_value(t, b));
    return ha == hb && ka == kb;
}

const LookupTable& bound_table(const TLookup& l) {
    if (!l.slot || !l.slot->table)
        throw DFTUtilsException(
            ErrorCode::INVALID_ARGUMENT,
            "View::duql '" + l.name + "' was read before its rows were bound");
    return *l.slot->table;
}

std::optional<std::string_view> str(const Value& v) {
    if (const auto* s = std::get_if<std::string_view>(&v)) return *s;
    return std::nullopt;
}

// The container `json` spells, parsed into the scratch arena.
Value parsed(const std::string& json) {
    const Value v = scratch.parse(scratch.own(json));
    if (std::holds_alternative<VMissing>(v)) return VNull{};
    return v;
}

void append_element(std::string& out, simdjson::dom::element el) {
    if (out.size() > 1) out += ',';
    json::append_canonical_json(out, el);
}

// A slice index: from the end when negative, clamped to [0, size].
std::optional<std::uint64_t> slice_at(const Value& v, std::uint64_t size) {
    const auto i = as_int(v);
    if (!i) return std::nullopt;
    if (!i->neg) return std::min(i->mag, size);
    return i->mag >= size ? 0 : size - i->mag;
}

Value slice(const Value& v, const Value& from, const std::optional<Value>& to) {
    simdjson::dom::array arr;
    if (!is_array(v, arr)) return VNull{};
    const std::uint64_t size = arr.size();
    const auto b = slice_at(from, size);
    const auto e = to ? slice_at(*to, size) : std::optional{size};
    if (!b || !e) return VNull{};
    std::string out = "[";
    std::uint64_t i = 0;
    for (auto el : arr) {
        if (i >= *b && i < *e) append_element(out, el);
        ++i;
    }
    return parsed(out + "]");
}

Value flatten(const Value& v) {
    simdjson::dom::array arr;
    if (!is_array(v, arr)) return VNull{};
    std::string out = "[";
    for (auto el : arr) {
        simdjson::dom::array inner;
        if (el.get_array().get(inner) != simdjson::SUCCESS) {
            append_element(out, el);
            continue;
        }
        for (auto x : inner) append_element(out, x);
    }
    return parsed(out + "]");
}

// The keys or the values of an object, in the byte order of the keys.
Value members(const Value& v, bool keys) {
    const auto* el = std::get_if<simdjson::dom::element>(&v);
    simdjson::dom::object obj;
    if (!el || el->get_object().get(obj) != simdjson::SUCCESS) return VNull{};
    std::vector<std::pair<std::string_view, simdjson::dom::element>> fields;
    for (auto f : obj) fields.emplace_back(f.key, f.value);
    std::stable_sort(
        fields.begin(), fields.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string out = "[";
    for (const auto& [k, x] : fields) {
        if (!keys) {
            append_element(out, x);
            continue;
        }
        if (out.size() > 1) out += ',';
        out += '"';
        json::append_json_escaped(out, k);
        out += '"';
    }
    return parsed(out + "]");
}

Value parse_json(const Value& v) {
    const auto s = str(v);
    if (!s) return VNull{};
    const Value x = scratch.parse(*s);
    if (const auto* el = std::get_if<simdjson::dom::element>(&x))
        return of_element(*el);
    return VNull{};
}

Value split(const Value& v, const Value& sep) {
    const auto s = str(v);
    const auto p = str(sep);
    if (!s || !p || p->empty()) return VNull{};
    std::string out = "[";
    std::size_t at = 0;
    for (;;) {
        const std::size_t hit = s->find(*p, at);
        if (out.size() > 1) out += ',';
        out += '"';
        json::append_json_escaped(out, s->substr(at, hit - at));
        out += '"';
        if (hit == std::string_view::npos) break;
        at = hit + p->size();
    }
    return parsed(out + "]");
}

template <class Record>
class TermEval {
   public:
    explicit TermEval(const Record& rec, const Value* element = nullptr)
        : rec_(rec), element_(element) {}

    Value operator()(const Term& t) const {
        return std::visit([this](const auto& n) { return this->node(n); },
                          t.node);
    }

   private:
    const Record& rec_;
    const Value* element_;

    Value node(const TConst& c) const {
        return std::visit(
            [](const auto& v) -> Value {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, TNull>)
                    return VNull{};
                else if constexpr (std::is_same_v<T, std::string>)
                    return std::string_view(v);
                else
                    return v;
            },
            c.value);
    }

    Value node(const TField& f) const {
        if (f.root != FieldRoot::ELEMENT) return walk(rec_.get(f), f);
        if (!element_) return VMissing{};
        return walk(*element_, f);
    }

    // any: YES when some element is YES, NO when every one is NO; all: NO
    // when some element is NO, YES when every one is YES. UNKNOWN otherwise.
    Value node(const TQuant& q) const {
        if (const auto* l = std::get_if<TLookup>(&q.subject->node); l && l->all)
            return matching(*l, q);
        simdjson::dom::array arr;
        if (!is_array((*this)(*q.subject), arr)) return VNull{};
        const Truth stop = q.all ? Truth::NO : Truth::YES;
        Truth out = q.all ? Truth::YES : Truth::NO;
        for (auto el : arr) {
            const Value v = of_element(el);
            const Truth t = truth(TermEval(rec_, &v)(*q.cond));
            if (t == stop) return of_truth(stop);
            if (t == Truth::UNKNOWN) out = Truth::UNKNOWN;
        }
        return of_truth(out);
    }

    // The keys of `l` for this record, or nullopt when a part has none.
    std::optional<std::string> key(const TLookup& l) const {
        std::string out;
        for (const auto& k : l.keys)
            if (!value_key(out, (*this)(*k))) return std::nullopt;
        return out;
    }

    // `q` over the values of the rows of `l`'s table with this record's key:
    // unknown when there is none.
    Value matching(const TLookup& l, const TQuant& q) const {
        const LookupTable& t = bound_table(l);
        const auto k = key(l);
        const auto* hits = k ? t.find(*k) : nullptr;
        if (!hits) return VNull{};
        const Truth stop = q.all ? Truth::NO : Truth::YES;
        Truth out = q.all ? Truth::YES : Truth::NO;
        for (const std::int64_t r : *hits) {
            const Value v = t.value ? table_value(t, r) : Value{VMissing{}};
            const Truth x = truth(TermEval(rec_, &v)(*q.cond));
            if (x == stop) return of_truth(stop);
            if (x == Truth::UNKNOWN) out = Truth::UNKNOWN;
        }
        return of_truth(out);
    }

    Value node(const TLookup& l) const {
        const LookupTable& t = bound_table(l);
        if (l.kind == LookupKind::SCALAR) {
            if (t.cells.empty()) return VNull{};
            return table_value(t, 0);
        }
        const auto k = key(l);
        if (l.kind == LookupKind::IN) {
            if (!k) return VNull{};
            return (t.find(*k) != nullptr) != l.negated;
        }
        const auto* hits = k ? t.find(*k) : nullptr;
        if (!hits || !t.value) return VMissing{};
        const std::int64_t first = hits->front();
        for (std::size_t i = 1; i < hits->size(); ++i)
            if (!same_value(t, first, (*hits)[i])) conflict(t, l.column, *k);
        return table_value(t, first);
    }

    Value node(const TUnary& u) const {
        const Value v = (*this)(*u.operand);
        if (u.op == TermOp::NOT) return of_truth(t_not(truth(v)));
        if (const auto i = as_int(v)) return of_int(Int{!i->neg, i->mag});
        if (const auto* d = std::get_if<double>(&v)) return -*d;
        return VNull{};
    }

    Value node(const TBinary& b) const {
        switch (b.op) {
            case TermOp::AND: {
                const Truth l = truth((*this)(*b.left));
                if (l == Truth::NO) return false;
                return of_truth(t_and(l, truth((*this)(*b.right))));
            }
            case TermOp::OR: {
                const Truth l = truth((*this)(*b.left));
                if (l == Truth::YES) return true;
                return of_truth(t_or(l, truth((*this)(*b.right))));
            }
            case TermOp::COALESCE: {
                Value l = (*this)(*b.left);
                if (!absent(l)) return l;
                return (*this)(*b.right);
            }
            case TermOp::EQ:
                return of_truth(apply(CompareOp::EQ, compared(b)));
            case TermOp::NE:
                return of_truth(apply(CompareOp::NE, compared(b)));
            case TermOp::LT:
                return of_truth(apply(CompareOp::LT, compared(b)));
            case TermOp::LE:
                return of_truth(apply(CompareOp::LE, compared(b)));
            case TermOp::GT:
                return of_truth(apply(CompareOp::GT, compared(b)));
            case TermOp::GE:
                return of_truth(apply(CompareOp::GE, compared(b)));
            default:
                return arith(b.op, (*this)(*b.left), (*this)(*b.right));
        }
    }

    std::optional<int> compared(const TBinary& b) const {
        return cmp((*this)(*b.left), (*this)(*b.right));
    }

    Value node(const TBetween& b) const {
        const Value v = (*this)(*b.subject);
        const Truth t = t_and(apply(CompareOp::GE, cmp(v, (*this)(*b.low))),
                              apply(CompareOp::LE, cmp(v, (*this)(*b.high))));
        return of_truth(b.negated ? t_not(t) : t);
    }

    Value node(const TIs& is) const {
        const Value v = (*this)(*is.subject);
        const bool hit = is.missing ? std::holds_alternative<VMissing>(v)
                                    : std::holds_alternative<VNull>(v);
        return hit != is.negated;
    }

    Value node(const TIn& in) const {
        const Value v = (*this)(*in.subject);
        bool comparable = false;
        Truth t = Truth::UNKNOWN;
        for (const auto& item : in.list) {
            const auto c = cmp(v, (*this)(*item));
            if (!c) continue;
            comparable = true;
            if (*c == 0) {
                t = Truth::YES;
                break;
            }
        }
        if (t != Truth::YES && comparable) t = Truth::NO;
        return of_truth(in.negated ? t_not(t) : t);
    }

    Value node(const TMatch& m) const {
        const Value v = (*this)(*m.subject);
        const auto s = str(v);
        if (!s || !m.compiled) return VNull{};
        const Truth t = pattern_truth(*m.compiled, *s);
        return of_truth(m.negated ? t_not(t) : t);
    }

    Value arg(const TCall& c, std::size_t i) const {
        return (*this)(*c.args[i]);
    }

    Value node(const TCall& c) const {
        switch (c.fn) {
            case Fn::EXISTS:
                return !std::holds_alternative<VMissing>(arg(c, 0));
            case Fn::COALESCE:
                for (const auto& a : c.args) {
                    Value v = (*this)(*a);
                    if (!absent(v)) return v;
                }
                return VNull{};
            case Fn::IF:
                return truth(arg(c, 0)) == Truth::YES ? arg(c, 1) : arg(c, 2);
            case Fn::CASE:
                for (std::size_t i = 0; i + 1 < c.args.size(); i += 2)
                    if (truth(arg(c, i)) == Truth::YES) return arg(c, i + 1);
                return arg(c, c.args.size() - 1);
            case Fn::ABS: {
                const Value v = arg(c, 0);
                if (const auto i = as_int(v)) return of_int(Int{false, i->mag});
                if (const auto* d = std::get_if<double>(&v))
                    return std::fabs(*d);
                return VNull{};
            }
            case Fn::FLOOR:
            case Fn::CEIL: {
                const Value v = arg(c, 0);
                if (as_int(v)) return v;
                if (const auto* d = std::get_if<double>(&v))
                    return c.fn == Fn::FLOOR ? std::floor(*d) : std::ceil(*d);
                return VNull{};
            }
            case Fn::ROUND:
                return round_to(arg(c, 0), c.args.size() > 1
                                               ? arg(c, 1)
                                               : Value{std::int64_t{0}});
            case Fn::MIN:
            case Fn::MAX:
                return extreme(c, c.fn == Fn::MIN ? -1 : 1);
            case Fn::LOG: {
                const auto d = as_double(arg(c, 0));
                if (!d || *d <= 0) return VNull{};
                return std::log(*d);
            }
            case Fn::EXP: {
                const auto d = as_double(arg(c, 0));
                if (!d) return VNull{};
                return std::exp(*d);
            }
            case Fn::POW: {
                const auto a = as_double(arg(c, 0));
                const auto b = as_double(arg(c, 1));
                if (!a || !b) return VNull{};
                return of_double(std::pow(*a, *b));
            }
            case Fn::LEN:
                return length(arg(c, 0));
            case Fn::CONCAT: {
                std::string out;
                for (const auto& a : c.args) {
                    const auto t = text_of((*this)(*a), false);
                    if (!t) return VNull{};
                    out += *t;
                }
                return scratch.own(std::move(out));
            }
            case Fn::LOWER:
            case Fn::UPPER: {
                const auto s = str(arg(c, 0));
                if (!s) return VNull{};
                std::string out(*s);
                for (char& ch : out) {
                    if (c.fn == Fn::LOWER && ch >= 'A' && ch <= 'Z')
                        ch = static_cast<char>(ch - 'A' + 'a');
                    if (c.fn == Fn::UPPER && ch >= 'a' && ch <= 'z')
                        ch = static_cast<char>(ch - 'a' + 'A');
                }
                return scratch.own(std::move(out));
            }
            case Fn::TRIM: {
                const auto s = str(arg(c, 0));
                if (!s) return VNull{};
                constexpr std::string_view WS = " \t\n\r\f\v";
                const auto b = s->find_first_not_of(WS);
                if (b == std::string_view::npos) return std::string_view{};
                return s->substr(b, s->find_last_not_of(WS) - b + 1);
            }
            case Fn::STARTS_WITH:
            case Fn::ENDS_WITH: {
                const auto s = str(arg(c, 0));
                const auto p = str(arg(c, 1));
                if (!s || !p) return VNull{};
                return c.fn == Fn::STARTS_WITH ? s->starts_with(*p)
                                               : s->ends_with(*p);
            }
            case Fn::CONTAINS:
                return contains(arg(c, 0), arg(c, 1));
            case Fn::SUBSTR:
                return substr(c);
            case Fn::REPLACE:
                return replace(c);
            case Fn::FIRST:
            case Fn::LAST: {
                simdjson::dom::array arr;
                if (!is_array(arg(c, 0), arr) || arr.size() == 0)
                    return VNull{};
                simdjson::dom::element el;
                if (arr.at(c.fn == Fn::FIRST ? 0 : arr.size() - 1).get(el) !=
                    simdjson::SUCCESS)
                    return VNull{};
                return of_element(el);
            }
            case Fn::SUM:
                return sum(arg(c, 0));
            case Fn::JSON: {
                auto t = text_of(arg(c, 0), true);
                if (!t) return VNull{};
                return scratch.own(std::move(*t));
            }
            case Fn::TYPE:
                return type_name(arg(c, 0));
            case Fn::INT:
                return to_int(arg(c, 0));
            case Fn::FLOAT:
                return to_float(arg(c, 0));
            case Fn::STRING: {
                const Value v = arg(c, 0);
                if (str(v)) return v;
                auto t = text_of(v, false);
                if (!t) return VNull{};
                return scratch.own(std::move(*t));
            }
            case Fn::SLICE:
                return slice(arg(c, 0), arg(c, 1),
                             c.args.size() > 2 ? std::optional{arg(c, 2)}
                                               : std::nullopt);
            case Fn::FLATTEN:
                return flatten(arg(c, 0));
            case Fn::KEYS:
            case Fn::VALUES:
                return members(arg(c, 0), c.fn == Fn::KEYS);
            case Fn::PARSE_JSON:
                return parse_json(arg(c, 0));
            case Fn::SPLIT:
                return split(arg(c, 0), arg(c, 1));
            case Fn::EXTRACT: {
                const auto s = str(arg(c, 0));
                if (!s || !c.pattern) return VNull{};
                std::size_t group = 0;
                if (c.args.size() > 2) {
                    const auto g = as_int(arg(c, 2));
                    if (!g || g->neg) return VNull{};
                    group = static_cast<std::size_t>(g->mag);
                }
                std::string_view out;
                if (extract(*c.pattern, *s, group, out) != MatchResult::YES)
                    return VNull{};
                return out;
            }
        }
        return VNull{};
    }

    Value extreme(const TCall& c, int want) const {
        std::vector<Value> values;
        if (c.args.size() == 1) {
            simdjson::dom::array arr;
            if (!is_array(arg(c, 0), arr)) return VNull{};
            for (auto el : arr) values.push_back(of_element(el));
        } else {
            for (const auto& a : c.args) values.push_back((*this)(*a));
        }
        if (values.empty()) return VNull{};
        Value best = values.front();
        for (const Value& v : values) {
            const auto o = cmp(v, best);
            if (!o) return VNull{};
            if (*o == want) best = v;
        }
        return best;
    }

    Value substr(const TCall& c) const {
        const auto s = str(arg(c, 0));
        const auto start = as_int(arg(c, 1));
        if (!s || !start || start->neg) return VNull{};
        const std::string_view rest = s->substr(utf8_offset(*s, start->mag));
        if (c.args.size() < 3) return rest;
        const auto len = as_int(arg(c, 2));
        if (!len || len->neg) return VNull{};
        return rest.substr(0, utf8_offset(rest, len->mag));
    }

    Value replace(const TCall& c) const {
        const auto s = str(arg(c, 0));
        const auto from = str(arg(c, 1));
        const auto to = str(arg(c, 2));
        if (!s || !from || !to) return VNull{};
        if (from->empty()) return *s;
        std::string out;
        std::size_t pos = 0;
        for (std::size_t hit = s->find(*from); hit != std::string_view::npos;
             hit = s->find(*from, pos)) {
            out.append(s->substr(pos, hit - pos));
            out.append(*to);
            pos = hit + from->size();
        }
        out.append(s->substr(pos));
        return scratch.own(std::move(out));
    }
};

// A JSON record. With `args_fallback`, a bare name absent from the record
// resolves under "args.", where dftracer keeps domain fields; a name present
// as null does not.
struct JsonRecord {
    const JsonValue& event;
    bool args_fallback;

    JsonValue lookup(const std::string& path) const {
        JsonValue v = event.at(path);
        if (args_fallback && !v.exists() && !has_args_prefix(path))
            v = event.at("args." + path);
        return v;
    }

    template <class Leaf>
    Truth field(const FieldNode& f, Leaf&& leaf) const {
        const JsonValue v = lookup(f.path);
        if (!f.any) {
            if (!v.exists() || v.is_null()) return Truth::UNKNOWN;
            return leaf(v);
        }
        if (!v.is_array()) return Truth::UNKNOWN;
        const simdjson::dom::array arr = v.element().get_array().value_unsafe();
        for (auto el : arr) {
            const JsonValue ev(el);
            if (ev.is_null() || ev.is_object() || ev.is_array()) continue;
            if (leaf(ev) == Truth::YES) return Truth::YES;
        }
        return Truth::NO;
    }

    Value get(const TField& f) const {
        const JsonValue v = lookup(f.base);
        if (!v.exists()) return VMissing{};
        return of_element(v.element());
    }
};

bool is_position(std::string_view digits) {
    return !digits.empty() &&
           std::all_of(digits.begin(), digits.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
}

bool is_position_of(std::string_view key, std::string_view path) {
    return key.size() > path.size() + 1 && key.starts_with(path) &&
           key[path.size()] == '.' && is_position(key.substr(path.size() + 1));
}

bool scalar(const Cell& c) { return c.kind == Cell::Kind::VALUE; }

// A flattened record: a field absent from the map is MISSING, and an any()
// field reads its positions `<path>.<k>` (or without "args.", as event args
// are keyed).
struct MapRecord {
    const ValueMap& fields;

    template <class Leaf>
    Truth field(const FieldNode& f, Leaf&& leaf) const {
        if (!f.any) {
            const auto it = fields.find(f.path);
            if (it == fields.end() || !scalar(it->second))
                return Truth::UNKNOWN;
            return leaf(it->second.value);
        }
        const std::string_view path = f.path;
        const std::string_view bare =
            path.starts_with("args.") ? path.substr(5) : path;
        bool array = false;
        for (const auto& [key, cell] : fields) {
            if (!is_position_of(key, path) && !is_position_of(key, bare))
                continue;
            array = true;
            if (scalar(cell) && leaf(cell.value) == Truth::YES)
                return Truth::YES;
        }
        return array ? Truth::NO : Truth::UNKNOWN;
    }

    Value get(const TField& f) const {
        const auto it = fields.find(f.base);
        if (it == fields.end()) return VMissing{};
        return of_cell(it->second);
    }
};

template <class Record>
Truth eval(const QueryNode& node, const Record& rec) {
    return std::visit(
        [&rec](const auto& n) -> Truth {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, CompareNode>) {
                return rec.field(n.field, [&](const auto& v) {
                    return apply(n.op, compare(v, n.value));
                });
            } else if constexpr (std::is_same_v<T, InNode>) {
                return rec.field(n.field, [&](const auto& v) {
                    return in_values(n.values, v);
                });
            } else if constexpr (std::is_same_v<T, NotInNode>) {
                return rec.field(n.field, [&](const auto& v) {
                    return t_not(in_values(n.values, v));
                });
            } else if constexpr (std::is_same_v<T, MatchNode>) {
                if (!n.compiled) return Truth::UNKNOWN;
                return rec.field(n.field, [&](const auto& v) {
                    const auto s = as_string(v);
                    if (!s) return Truth::UNKNOWN;
                    const Truth t = pattern_truth(*n.compiled, *s);
                    return n.negated ? t_not(t) : t;
                });
            } else if constexpr (std::is_same_v<T, AndNode>) {
                const Truth l = eval(*n.left, rec);
                if (l == Truth::NO) return Truth::NO;
                return t_and(l, eval(*n.right, rec));
            } else if constexpr (std::is_same_v<T, OrNode>) {
                const Truth l = eval(*n.left, rec);
                if (l == Truth::YES) return Truth::YES;
                return t_or(l, eval(*n.right, rec));
            } else if constexpr (std::is_same_v<T, NotNode>) {
                return t_not(eval(*n.operand, rec));
            } else {
                scratch.reset();
                return truth(TermEval<Record>(rec)(*n.term));
            }
        },
        node.data);
}

}  // namespace

Truth evaluate_truth(const QueryNode& node, const JsonValue& event,
                     bool args_fallback) {
    return eval(node, JsonRecord{event, args_fallback});
}

Truth evaluate_truth(const QueryNode& node, const ValueMap& fields) {
    return eval(node, MapRecord{fields});
}

bool evaluate(const QueryNode& node, const JsonValue& event,
              bool args_fallback) {
    return evaluate_truth(node, event, args_fallback) == Truth::YES;
}

bool evaluate(const QueryNode& node, const ValueMap& fields) {
    return evaluate_truth(node, fields) == Truth::YES;
}

}  // namespace dftracer::utils::duql
