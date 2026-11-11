// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/yaml/document.h>
#include <dftracer/utils/yaml/tape.h>

#include <cstddef>
#include <limits>
#include <utility>

namespace dftracer::utils::yaml {

namespace {

constexpr std::size_t NONE = static_cast<std::size_t>(-1);
/// How many merge keys a lookup follows, one inside the other.
constexpr std::size_t MERGE_DEPTH_LIMIT = 64;

const TapeEntry& entry_at(const DocumentStorage* storage, std::size_t id) {
    return storage->tape[id];
}

// An alias reads as the node it names.
std::size_t resolve(const DocumentStorage* storage, std::size_t id) {
    if (id != NONE && storage->tape[id].kind == TapeKind::Alias)
        return storage->tape[id].pos;
    return id;
}

TapeKind kind_of(const DocumentStorage* storage, std::size_t id) {
    if (id == NONE) return TapeKind::Null;
    return entry_at(storage, resolve(storage, id)).kind;
}

// The tape index after the node at `id`.
std::size_t skip(const DocumentStorage* storage, std::size_t id) {
    const TapeEntry& entry = entry_at(storage, id);
    return entry.is_container() ? entry.pos : id + 1;
}

// The value index of the key `key` among the own members of the map at
// `map`, and the index of its merge value in `merge`.
struct MapScan {
    std::size_t found = NONE;
    std::size_t merge = NONE;
};

MapScan scan_map(const DocumentStorage* storage, std::size_t map,
                 dstd::StringView key) {
    MapScan scan;
    const std::size_t end = entry_at(storage, map).pos;
    for (std::size_t i = map + 1; i < end;) {
        const TapeEntry& entry = entry_at(storage, i);
        const std::size_t value = i + 1;
        if ((entry.flags & tape_flag::MERGE) != 0) {
            scan.merge = value;
        } else if (key_text(*storage, i) == key) {
            scan.found = value;
            return scan;
        }
        i = skip(storage, value);
    }
    return scan;
}

std::size_t find_in(const DocumentStorage* storage, std::size_t map,
                    dstd::StringView key, std::size_t depth);

// The merge value is a map, an alias of one or a sequence of them; the first
// map of a sequence that has the key wins.
std::size_t find_merged(const DocumentStorage* storage, std::size_t merge,
                        dstd::StringView key, std::size_t depth) {
    if (depth >= MERGE_DEPTH_LIMIT) return NONE;
    const std::size_t node = resolve(storage, merge);
    const TapeEntry& entry = entry_at(storage, node);
    if (entry.kind == TapeKind::Map) return find_in(storage, node, key, depth);
    if (entry.kind != TapeKind::Seq) return NONE;
    for (std::size_t i = node + 1; i < entry.pos; i = skip(storage, i)) {
        const std::size_t element = resolve(storage, i);
        if (entry_at(storage, element).kind != TapeKind::Map) continue;
        const std::size_t found = find_in(storage, element, key, depth);
        if (found != NONE) return found;
    }
    return NONE;
}

std::size_t find_in(const DocumentStorage* storage, std::size_t map,
                    dstd::StringView key, std::size_t depth) {
    const MapScan scan = scan_map(storage, map, key);
    if (scan.found != NONE || scan.merge == NONE) return scan.found;
    return find_merged(storage, scan.merge, key, depth + 1);
}

// Calls `fn` with every member the map shows: its own, and those of its merge
// values that no earlier member overrides. `root` is the map the lookup that
// decides which member wins starts from.
template <class Fn>
void each_member(const DocumentStorage* storage, std::size_t root,
                 std::size_t map, std::size_t depth, Fn& fn);

template <class Fn>
void each_merged_member(const DocumentStorage* storage, std::size_t root,
                        std::size_t merge, std::size_t depth, Fn& fn) {
    if (depth >= MERGE_DEPTH_LIMIT) return;
    const std::size_t node = resolve(storage, merge);
    const TapeEntry& entry = entry_at(storage, node);
    if (entry.kind == TapeKind::Map) {
        each_member(storage, root, node, depth + 1, fn);
    } else if (entry.kind == TapeKind::Seq) {
        for (std::size_t i = node + 1; i < entry.pos; i = skip(storage, i)) {
            const std::size_t element = resolve(storage, i);
            if (entry_at(storage, element).kind == TapeKind::Map)
                each_member(storage, root, element, depth + 1, fn);
        }
    }
}

template <class Fn>
void each_member(const DocumentStorage* storage, std::size_t root,
                 std::size_t map, std::size_t depth, Fn& fn) {
    const TapeEntry& begin = entry_at(storage, map);
    const bool merges = (begin.flags & tape_flag::MERGE) != 0;
    for (std::size_t i = map + 1; i < begin.pos;) {
        const TapeEntry& entry = entry_at(storage, i);
        const std::size_t value = i + 1;
        if ((entry.flags & tape_flag::MERGE) != 0) {
            each_merged_member(storage, root, value, depth, fn);
        } else {
            const dstd::StringView key = key_text(*storage, i);
            // Without a merge key nothing overrides a member.
            if (!merges && depth == 0) {
                fn(key, value);
            } else if (find_in(storage, root, key, 0) == value) {
                fn(key, value);
            }
        }
        i = skip(storage, value);
    }
}

bool is_flexible_case(dstd::StringView s) {
    if (s.empty()) return true;
    bool lower = true;
    for (char c : s) lower = lower && dstd::is_ascii_lower(c);
    if (lower) return true;
    if (!dstd::is_ascii_upper(s[0])) return false;
    bool rest_lower = true;
    bool rest_upper = true;
    for (std::size_t i = 1; i < s.size(); ++i) {
        rest_lower = rest_lower && dstd::is_ascii_lower(s[i]);
        rest_upper = rest_upper && dstd::is_ascii_upper(s[i]);
    }
    return rest_lower || rest_upper;
}

}  // namespace

Document::Document(dstd::UniquePtr<DocumentStorage> storage)
    : storage_(std::move(storage)) {}
Document::Document(Document&&) noexcept = default;
Document& Document::operator=(Document&&) noexcept = default;
Document::~Document() = default;

dstd::Result<Document> Document::parse(dstd::String text,
                                       dstd::StringView source_name) {
    auto storage = dstd::make_unique<DocumentStorage>();
    storage->text = std::move(text);
    storage->base = storage->text.data();
    storage->size = storage->text.size();
    TapeParser parser;
    if (auto parsed = parser.parse(*storage, source_name); !parsed)
        return parsed.error();
    return Document(std::move(storage));
}

dstd::Result<Document> Document::load_file(dstd::PathView path) {
    auto text = dstd::fs::read_file(path);
    if (!text) return text.error();
    return parse(std::move(*text), path.str());
}

Value Document::root() const {
    return Value(storage_.get(), storage_->tape.empty() ? NONE : 0);
}

bool Value::is_object() const {
    return kind_of(storage_, id_) == TapeKind::Map;
}

bool Value::is_array() const { return kind_of(storage_, id_) == TapeKind::Seq; }

bool Value::is_string() const {
    return kind_of(storage_, id_) == TapeKind::Scalar;
}

bool Value::is_null() const { return kind_of(storage_, id_) == TapeKind::Null; }

dstd::Optional<Value> Value::find(dstd::StringView key) const {
    if (!is_object()) return dstd::nullopt;
    const std::size_t found = find_in(storage_, resolve(storage_, id_), key, 0);
    if (found == NONE) return dstd::nullopt;
    return Value(storage_, found);
}

void Value::for_each_member(
    dstd::FunctionRef<void(dstd::StringView, Value)> fn) const {
    if (!is_object()) return;
    const DocumentStorage* storage = storage_;
    auto emit = [&](dstd::StringView key, std::size_t value) {
        fn(key, Value(storage, value));
    };
    const std::size_t map = resolve(storage, id_);
    each_member(storage, map, map, 0, emit);
}

void Value::for_each_element(dstd::FunctionRef<void(Value)> fn) const {
    if (!is_array()) return;
    const std::size_t seq = resolve(storage_, id_);
    const std::size_t end = entry_at(storage_, seq).pos;
    for (std::size_t i = seq + 1; i < end; i = skip(storage_, i))
        fn(Value(storage_, i));
}

dstd::StringView Value::raw() const {
    if (!is_string()) return {};
    return entry_text(*storage_, entry_at(storage_, resolve(storage_, id_)));
}

dstd::Optional<dstd::StringView> Value::as_string() const {
    if (!is_string()) return dstd::nullopt;
    return raw();
}

dstd::Optional<bool> Value::as_bool() const {
    if (!is_string()) return dstd::nullopt;
    const dstd::StringView text = raw();
    if (!is_flexible_case(text)) return dstd::nullopt;
    static constexpr dstd::StringView TRUE_NAMES[] = {"y", "yes", "true", "on"};
    static constexpr dstd::StringView FALSE_NAMES[] = {"n", "no", "false",
                                                       "off"};
    for (const dstd::StringView name : TRUE_NAMES)
        if (dstd::ascii_iequals(text, name)) return true;
    for (const dstd::StringView name : FALSE_NAMES)
        if (dstd::ascii_iequals(text, name)) return false;
    return dstd::nullopt;
}

dstd::Optional<double> Value::as_double() const {
    if (!is_string()) return dstd::nullopt;
    dstd::StringView text = raw();
    if (text == ".inf" || text == ".Inf" || text == ".INF" || text == "+.inf" ||
        text == "+.Inf" || text == "+.INF")
        return std::numeric_limits<double>::infinity();
    if (text == "-.inf" || text == "-.Inf" || text == "-.INF")
        return -std::numeric_limits<double>::infinity();
    if (text == ".nan" || text == ".NaN" || text == ".NAN")
        return std::numeric_limits<double>::quiet_NaN();
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    return dstd::from_chars<double>(text);
}

dstd::Optional<std::int64_t> Value::as_int() const {
    if (!is_string()) return dstd::nullopt;
    dstd::StringView text = raw();
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    return dstd::from_chars<std::int64_t>(text);
}

dstd::Optional<std::uint64_t> Value::as_uint() const {
    if (!is_string()) return dstd::nullopt;
    dstd::StringView text = raw();
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    return dstd::from_chars<std::uint64_t>(text);
}

}  // namespace dftracer::utils::yaml
