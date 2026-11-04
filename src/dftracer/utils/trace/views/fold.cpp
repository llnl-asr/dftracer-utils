// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/memory_budget.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_arena.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/numbers.h>
#include <dftracer/utils/index/event_cost.h>
#include <dftracer/utils/index/store/file.h>
#include <dftracer/utils/json/canonical.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/value_access.h>
#include <dftracer/utils/trace/internal/parse_walks.h>
#include <dftracer/utils/trace/views/event_source.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_scanner_utility.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace dftracer::utils::trace::views::detail {

void DynamicPrune::exclude_file(const dstd::String& file_path) {
    std::lock_guard<std::mutex> lk(mu_);
    excluded_files_.insert(file_path);
}

void DynamicPrune::exclude_checkpoints(
    const dstd::String& file_path, dstd::Vector<std::uint64_t> checkpoints) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& set = excluded_checkpoints_[file_path];
    for (std::uint64_t c : checkpoints) set.insert(c);
}

bool DynamicPrune::is_excluded(const dstd::String& file_path,
                               std::uint64_t checkpoint_idx) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (excluded_files_.count(file_path)) return true;
    auto it = excluded_checkpoints_.find(file_path);
    if (it == excluded_checkpoints_.end()) return false;
    return it->second.count(checkpoint_idx) != 0;
}

namespace {

struct ScanPermit {
    coro::CoroSemaphore& sem;
    std::uint64_t bytes;
    ScanPermit(coro::CoroSemaphore& s, std::uint64_t n) : sem(s), bytes(n) {}
    ~ScanPermit() { sem.release(bytes); }
    ScanPermit(const ScanPermit&) = delete;
    ScanPermit& operator=(const ScanPermit&) = delete;
};

// Interns through `cache` when there is one.
struct Ids {
    dftracer::utils::dstd::StringIntern& intern;
    dftracer::utils::dstd::InternCache* cache;

    std::uint32_t get_or_insert(dstd::StringView sv) {
        return cache ? cache->get_or_insert(sv) : intern.get_or_insert(sv);
    }
};

std::uint32_t intern_string(Ids& ids, simdjson::dom::element val) {
    if (!val.is_string()) return dftracer::utils::dstd::StringIntern::NO_ID;
    return ids.get_or_insert(val.get_string().value_unsafe());
}

std::uint32_t intern_leaf_path(dftracer::utils::dstd::StringIntern& intern,
                               dstd::StringView path) {
#if DFTRACER_UTILS_TEST_PROBES
    dftracer::utils::index::event_cost::g_leaf_path_interns.fetch_add(
        1, std::memory_order_relaxed);
#endif
    return intern.get_or_insert(path);
}

std::uint8_t leaf_type_tag(simdjson::dom::element v) {
    using dftracer::utils::index::store::PathType;
    using T = simdjson::dom::element_type;
    PathType t = PathType::NullValue;
    switch (v.type()) {
        case T::STRING:
            t = PathType::String;
            break;
        case T::INT64:
            t = PathType::Int;
            break;
        case T::UINT64:
            t = v.get_int64().error() == simdjson::SUCCESS ? PathType::Int
                                                           : PathType::Uint;
            break;
        case T::DOUBLE:
            t = PathType::Double;
            break;
        case T::BOOL:
            t = PathType::Bool;
            break;
        case T::ARRAY:
            t = PathType::Array;
            break;
        case T::OBJECT:
            t = PathType::Object;
            break;
        default:
            break;
    }
    return static_cast<std::uint8_t>(t);
}

// Walk `v` to every scalar or null leaf, emitting (interned dotted path ->
// PathType) into ev.schema_leaves. Objects recurse by key (a.b.c), arrays by
// index (a.0.b), so the harvest lists every column a row fold emits. `path` is
// a reused buffer (restored on return).
void enumerate_leaves(dstd::String& path, simdjson::dom::element v,
                      dftracer::utils::dstd::StringIntern& intern,
                      FoldEvent& ev) {
    simdjson::dom::object obj;
    if (v.get_object().get(obj) == simdjson::SUCCESS && obj.size() > 0) {
        for (auto kv : obj) {
            const std::size_t base = path.size();
            if (base) path.push_back('.');
            path.append(kv.key);
            enumerate_leaves(path, kv.value, intern, ev);
            path.resize(base);
        }
        return;
    }
    simdjson::dom::array arr;
    if (v.get_array().get(arr) == simdjson::SUCCESS && arr.size() > 0) {
        std::size_t i = 0;
        for (auto el : arr) {
            const std::size_t base = path.size();
            if (base) path.push_back('.');
            dftracer::utils::trace::detail::append_index(path, i++);
            enumerate_leaves(path, el, intern, ev);
            path.resize(base);
        }
        return;
    }
    if (!path.empty())
        ev.schema_leaves.emplace_back(intern_leaf_path(intern, path),
                                      leaf_type_tag(v));
}

std::uint32_t arg_leaf_id(DecodeHints& h,
                          dftracer::utils::dstd::StringIntern& intern,
                          std::uint32_t key_id, dstd::StringView key) {
    auto resolve = [&] {
        dstd::String path("args.");
        path.append(key);
        return intern_leaf_path(intern, path);
    };
    if (key_id < DecodeHints::FIELD_CACHE_IDS) {
        if (key_id >= h.arg_leaf.size())
            h.arg_leaf.resize(key_id + 1,
                              dftracer::utils::dstd::StringIntern::NO_ID);
        std::uint32_t& slot = h.arg_leaf[key_id];
        if (slot == dftracer::utils::dstd::StringIntern::NO_ID)
            slot = resolve();
        return slot;
    }
    auto [it, fresh] = h.arg_leaf_far.try_emplace(key_id, 0);
    if (fresh) it->second = resolve();
    return it->second;
}

std::uint64_t pack_key(dstd::StringView key) {
    std::uint64_t packed = 0;
    std::memcpy(&packed, key.data(), key.size());
    return packed;
}

std::uint32_t top_leaf_id(DecodeHints& h,
                          dftracer::utils::dstd::StringIntern& intern,
                          dstd::StringView key) {
    const bool small = key.size() <= sizeof(std::uint64_t);
    const std::uint64_t packed = small ? pack_key(key) : 0;
    for (const auto& leaf : h.top_leaf)
        if (leaf.key.size() == key.size() &&
            (small ? leaf.packed == packed : leaf.key == key))
            return leaf.id;
    const std::uint32_t id = intern_leaf_path(intern, key);
    h.top_leaf.push_back({dstd::String(key), packed, id});
    return id;
}

// A switch on the length and bytes keeps this to a few byte compares per key.
bool is_axis_key(dstd::StringView k, bool skip_args) {
    const char* p = k.data();
    switch (k.size()) {
        case 2:
            return (p[0] == 'i' && p[1] == 'd') ||
                   (p[0] == 't' && p[1] == 's') || (p[0] == 'p' && p[1] == 'h');
        case 3:
            return (p[0] == 'p' && p[1] == 'i' && p[2] == 'd') ||
                   (p[0] == 't' && p[1] == 'i' && p[2] == 'd') ||
                   (p[0] == 'd' && p[1] == 'u' && p[2] == 'r');
        case 4:
            return skip_args && p[0] == 'a' && p[1] == 'r' && p[2] == 'g' &&
                   p[3] == 's';
        default:
            return false;
    }
}

void append_arg_leaf(FoldEvent& ev, std::uint32_t key_id, dstd::StringView key,
                     simdjson::dom::element v, Ids& ids, DecodeHints* leaves) {
    append_scalar_arg(ev.args, ev.specials, key_id, v, ids);
    if (leaves)
        ev.schema_leaves.emplace_back(
            arg_leaf_id(*leaves, ids.intern, key_id, key), leaf_type_tag(v));
}

// The path of a member or element below `path`, as event.h flatten_arg names
// it. Each returns the length to cut `path` back to.
std::size_t enter_key(dstd::String& path, dstd::StringView key) {
    const std::size_t base = path.size();
    path.push_back('.');
    path.append(key);
    return base;
}

std::size_t enter_index(dstd::String& path, std::size_t index) {
    const std::size_t base = path.size();
    path.push_back('.');
    dftracer::utils::trace::detail::append_index(path, index);
    return base;
}

// The fixed fields of `ev` from `s`, which holds phase, pid, tid, ts, dur,
// has_dur, cat and name. The args are a separate step.
template <class Scalars>
void set_fold_scalars(FoldEvent& ev, const Scalars& s, Ids& ids) {
    ev.phase = s.phase;
    ev.pid = s.pid;
    ev.tid = s.tid;
    ev.ts = s.ts;
    ev.dur = s.dur;
    ev.has_dur = s.has_dur;
    if (!s.cat.empty()) ev.cat_id = ids.get_or_insert(s.cat);
    if (!s.name.empty()) ev.name_id = ids.get_or_insert(s.name);
}

// The cat and name already interned for an event, so the strings of its args
// follow them as in the DOM form.
struct InternedHead {
    bool done = false;
    dstd::StringView cat;
    dstd::StringView name;
};

// As set_fold_scalars, interning only the cat and name that `head` has not.
template <class Scalars>
void set_fold_scalars(FoldEvent& ev, const Scalars& s, Ids& ids,
                      const InternedHead& head) {
    ev.phase = s.phase;
    ev.pid = s.pid;
    ev.tid = s.tid;
    ev.ts = s.ts;
    ev.dur = s.dur;
    ev.has_dur = s.has_dur;
    if (!s.cat.empty() && !(head.done && head.cat == s.cat))
        ev.cat_id = ids.get_or_insert(s.cat);
    if (!s.name.empty() && !(head.done && head.name == s.name))
        ev.name_id = ids.get_or_insert(s.name);
}

// Flatten a nested arg into dotted scalar keys named as event.h flatten_arg
// names them: objects "a.b", arrays "a.0". `path` holds the key so far.
void flatten_nested_arg(dstd::String& path, simdjson::dom::element v,
                        dftracer::utils::dstd::StringIntern& intern,
                        FoldEvent& ev, DecodeHints* leaves = nullptr) {
    simdjson::dom::object obj;
    if (v.get_object().get(obj) == simdjson::SUCCESS && obj.size() > 0) {
        for (auto kv : obj) {
            const std::size_t base = enter_key(path, kv.key);
            flatten_nested_arg(path, kv.value, intern, ev, leaves);
            path.resize(base);
        }
        return;
    }
    simdjson::dom::array arr;
    if (v.get_array().get(arr) == simdjson::SUCCESS && arr.size() > 0) {
        std::size_t i = 0;
        for (auto el : arr) {
            const std::size_t base = enter_index(path, i++);
            flatten_nested_arg(path, el, intern, ev, leaves);
            path.resize(base);
        }
        return;
    }
    Ids ids{intern, nullptr};
    append_arg_leaf(ev, intern.get_or_insert(path), path, v, ids, leaves);
}

}  // namespace

void flatten_container(FoldEvent& ev, dstd::String path,
                       simdjson::dom::element v,
                       dftracer::utils::dstd::StringIntern& intern) {
    flatten_nested_arg(path, v, intern, ev);
}

namespace {

// A value rebuilt from flattened keys: a leaf's JSON text, or members.
struct JsonNode {
    dstd::Map<dstd::String, JsonNode, std::less<>> kids;
    dstd::String leaf;
    bool is_leaf = false;
};

void insert_leaf(JsonNode& root, dstd::StringView rel, dstd::String text) {
    JsonNode* n = &root;
    std::size_t start = 0;
    for (;;) {
        const std::size_t dot = rel.find('.', start);
        auto it = n->kids.find(rel.substr(start, dot - start));
        if (it == n->kids.end())
            it = n->kids
                     .emplace(dstd::String(rel.substr(start, dot - start)),
                              JsonNode{})
                     .first;
        n = &it->second;
        if (dot == dstd::StringView::npos) break;
        start = dot + 1;
    }
    n->is_leaf = true;
    n->leaf = std::move(text);
}

// Members keyed 0..n-1 form an array, anything else an object.
void put_node(dstd::String& out, const JsonNode& n) {
    if (n.is_leaf) {
        out += n.leaf;
        return;
    }
    dstd::Vector<const JsonNode*> items(n.kids.size(), nullptr);
    bool array = true;
    for (const auto& [k, kid] : n.kids) {
        const dstd::Optional<std::size_t> at = dstd::from_chars<std::size_t>(k);
        if (!at || *at >= items.size()) {
            array = false;
            break;
        }
        items[*at] = &kid;
    }
    if (array) {
        out += '[';
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i) out += ',';
            put_node(out, *items[i]);
        }
        out += ']';
        return;
    }
    out += '{';
    bool first = true;
    for (const auto& [k, kid] : n.kids) {
        if (!first) out += ',';
        first = false;
        out += '"';
        dftracer::utils::json::append_json_escaped(out, k);
        out += "\":";
        put_node(out, kid);
    }
    out += '}';
}

dstd::String arg_json(const FoldEvent::ArgValue& v,
                      const dftracer::utils::dstd::StringIntern& intern) {
    dstd::String out;
    if (const auto* id = std::get_if<std::uint32_t>(&v)) {
        out += '"';
        dftracer::utils::json::append_json_escaped(out, intern.resolve(*id));
        out += '"';
    } else if (const auto* i = std::get_if<std::int64_t>(&v)) {
        out = dstd::int_text(*i);
    } else if (const auto* u = std::get_if<std::uint64_t>(&v)) {
        out = dstd::int_text(*u);
    } else {
        out = dstd::double_text(std::get<double>(v));
    }
    return out;
}

dstd::StringView special_json(FoldEvent::Special s) {
    switch (s) {
        case FoldEvent::Special::NullValue:
            return "null";
        case FoldEvent::Special::FalseValue:
            return "false";
        case FoldEvent::Special::TrueValue:
            return "true";
        case FoldEvent::Special::EmptyArray:
            return "[]";
        case FoldEvent::Special::EmptyObject:
            return "{}";
        case FoldEvent::Special::JsonArray:
        case FoldEvent::Special::JsonObject:
            break;
    }
    return "null";
}

// The canonical JSON text a declared json field holds in `args`.
dstd::StringView json_text(const FoldEvent& ev,
                           const dftracer::utils::dstd::StringIntern& intern,
                           std::uint32_t id) {
    for (const auto& [k, v] : ev.args)
        if (k == id)
            if (const auto* text = std::get_if<std::uint32_t>(&v))
                return intern.resolve(*text);
    return "null";
}

bool is_json_text(FoldEvent::Special s) {
    return s == FoldEvent::Special::JsonArray ||
           s == FoldEvent::Special::JsonObject;
}

}  // namespace

dstd::Optional<duql::Cell> container_cell(
    const FoldEvent& ev, const dftracer::utils::dstd::StringIntern& intern,
    dstd::StringView path) {
    const dstd::StringView bare = common::strip_args_prefix(path);
    const std::array<dstd::StringView, 2> prefixes{path, bare};
    const std::size_t tries = bare != path && !ev.by_path ? 2 : 1;
    for (std::size_t t = 0; t < tries; ++t) {
        const dstd::StringView prefix = prefixes[t];
        auto rel = [&](std::uint32_t id) -> dstd::Optional<dstd::StringView> {
            const dstd::StringView name = intern.resolve(id);
            if (name.size() <= prefix.size() || !name.starts_with(prefix) ||
                name[prefix.size()] != '.')
                return dstd::nullopt;
            return name.substr(prefix.size() + 1);
        };
        JsonNode root;
        for (const auto& [id, v] : ev.args)
            if (const auto r = rel(id))
                insert_leaf(root, *r, arg_json(v, intern));
        for (const auto& s : ev.specials)
            if (!s.top)
                if (const auto r = rel(s.key))
                    insert_leaf(root, *r,
                                dstd::String(is_json_text(s.kind)
                                                 ? json_text(ev, intern, s.key)
                                                 : special_json(s.kind)));
        if (!root.kids.empty()) {
            dstd::String text;
            put_node(text, root);
            const bool array = text.front() == '[';
            return duql::Cell::json(
                dftracer::utils::json::canonical_json_text(text), array);
        }
    }
    return dstd::nullopt;
}

void FoldPortBus::publish(std::uint64_t key, const void* data,
                          std::uint32_t len) {
    const std::size_t offset = arena_.size();
    const auto* p = static_cast<const std::byte*>(data);
    arena_.insert(arena_.end(), p, p + len);
    index_[key] = Entry{offset, len};
}

const void* FoldPortBus::consume(std::uint64_t key,
                                 std::uint32_t* out_len) const {
    auto it = index_.find(key);
    if (it == index_.end()) {
        if (out_len) *out_len = 0;
        return nullptr;
    }
    if (out_len) *out_len = it->second.len;
    return arena_.data() + it->second.offset;
}

void FoldPortBus::clear() {
    index_.clear();
    arena_.clear();
    frame.reset();
}

void build_fold_event_into(FoldEvent& ev, const DFTracerEvent& scalars,
                           simdjson::dom::element args, bool has_args,
                           dftracer::utils::dstd::StringIntern& intern,
                           bool needs_args, DecodeHints* leaves,
                           dftracer::utils::dstd::InternCache* cache) {
    if (!needs_args) leaves = nullptr;
    Ids ids{intern, cache};
    reset_fold_event(ev);
    set_fold_scalars(ev, scalars, ids);

    if (!has_args) return;

    // fhash/hhash are group dimensions even for folds that want no other args,
    // so they are always harvested; the rest only when a fold asks.
    for (auto field : args.get_object()) {
        dstd::StringView key = field.key;
        if (key == "fhash" || key == "hhash") {
            const std::uint32_t id = intern_string(ids, field.value);
            (key == "fhash" ? ev.fhash_id : ev.hhash_id) = id;
            if (leaves)
                ev.schema_leaves.emplace_back(
                    arg_leaf_id(*leaves, intern, ids.get_or_insert(key), key),
                    leaf_type_tag(field.value));
        } else if (needs_args) {
            const simdjson::dom::element v = field.value;
            switch (v.type()) {
                case simdjson::dom::element_type::OBJECT:
                case simdjson::dom::element_type::ARRAY: {
                    dstd::String path(key);
                    flatten_nested_arg(path, v, intern, ev, leaves);
                    break;
                }
                default:
                    append_arg_leaf(ev, ids.get_or_insert(key), key, v, ids,
                                    leaves);
            }
        }
    }
}

void extract_fold_event_into(FoldEvent& ev, simdjson::dom::element root,
                             dftracer::utils::dstd::StringIntern& intern,
                             bool needs_args,
                             const dstd::Vector<dstd::String>* extra_fields,
                             bool capture_schema, DecodeHints* hints) {
    DFTracerEvent scalars;
    simdjson::dom::element args;
    bool has_args = false;
    if (!DFTracerEvent::parse_scalars(root, scalars, args, has_args)) {
        reset_fold_event(ev);
        return;
    }
    DecodeHints* leaves =
        capture_schema && args_leaves_in_walk(needs_args, has_args, args)
            ? hints
            : nullptr;
    build_fold_event_into(ev, scalars, args, has_args, intern, needs_args,
                          leaves);
    if (extra_fields)
        for (const auto& name : *extra_fields)
            capture_extra_field(ev, root, intern, name);
    if (capture_schema)
        capture_schema_leaves(ev, root, intern, leaves != nullptr, leaves);
}

namespace {

constexpr std::size_t MAX_READ_DEPTH =
    dftracer::utils::common::constants::reader::MAX_READ_DEPTH;

using OnDemandValue = simdjson::ondemand::value;
using OnDemandType = simdjson::ondemand::json_type;

struct FoldScalars {
    RecordPhase phase = RecordPhase::Unknown;
    std::uint64_t pid = 0;
    std::uint64_t tid = 0;
    std::uint64_t ts = 0;
    std::uint64_t dur = 0;
    bool has_dur = false;
    dstd::StringView cat;
    dstd::StringView name;
};

// A number as the DOM classifies it: an integer that fits int64 is Int, one
// that needs uint64 is Uint, a fraction or exponent is Double, and an integer
// wider than 64 bits is Big, kept as its digits.
struct Number {
    enum class Kind : std::uint8_t { Int, Uint, Double, Big };
    Kind kind = Kind::Int;
    union {
        std::int64_t i;
        std::uint64_t u;
        double d;
    };
    Number() : i(0) {}
};

// Whether simdjson refused an integer wider than 64 bits. It reports one of
// 21 digits or more, or a negative one, as BIGINT_ERROR and a 20-digit one
// above uint64 as NUMBER_ERROR.
bool refused_big_integer(OnDemandValue& v) {
    return json::detail::is_integer_number(
        json::detail::trim_token(v.raw_json_token()));
}

// The double of a fraction or exponent form that simdjson refused for its
// magnitude, as Python's json gives it.
dstd::Optional<double> refused_real(OnDemandValue& v) {
    const dstd::StringView token = json::detail::trim_token(v.raw_json_token());
    if (token.find_first_of(".eE") == dstd::StringView::npos)
        return dstd::nullopt;
    return json::detail::refused_number(token);
}

// False when the text is not a valid number.
bool read_number(OnDemandValue& v, Number& out) {
    simdjson::ondemand::number n;
    const simdjson::error_code code = v.get_number().get(n);
    if (code == simdjson::BIGINT_ERROR) {
        out.kind = Number::Kind::Big;
        return true;
    }
    if (code == simdjson::NUMBER_ERROR) {
        if (refused_big_integer(v)) {
            out.kind = Number::Kind::Big;
            return true;
        }
        const auto refused = refused_real(v);
        if (!refused) return false;
        out.kind = Number::Kind::Double;
        out.d = *refused;
        return true;
    }
    if (code) return false;
    if (n.is_int64()) {
        out.kind = Number::Kind::Int;
        out.i = n.get_int64();
    } else if (n.is_uint64()) {
        out.kind = Number::Kind::Uint;
        out.u = n.get_uint64();
    } else {
        out.kind = Number::Kind::Double;
        out.d = n.get_double();
    }
    return true;
}

// A key of known length, compared without a call into libc.
template <std::size_t N>
bool is_key(dstd::StringView key, const char (&name)[N]) {
    return std::memcmp(key.data(), name, N - 1) == 0;
}

enum class Field : std::uint8_t { Used, Ignored, Error };

// Reads an unsigned integer field. Anything else is Ignored and leaves `out`.
Field read_unsigned(OnDemandValue& v, std::uint64_t& out) {
    // A failed get_uint64 leaves the value unread for the general path below.
    std::uint64_t plain;
    if (!v.get_uint64().get(plain)) {
        out = plain;
        return Field::Used;
    }
    OnDemandType type;
    if (v.type().get(type) || type == OnDemandType::unknown)
        return Field::Error;
    if (type != OnDemandType::number) return Field::Ignored;
    Number n;
    if (!read_number(v, n)) return Field::Error;
    if (n.kind == Number::Kind::Int && n.i >= 0) {
        out = static_cast<std::uint64_t>(n.i);
        return Field::Used;
    }
    if (n.kind == Number::Kind::Uint) {
        out = n.u;
        return Field::Used;
    }
    return Field::Ignored;
}

// A scalar leaf as the DOM classifies it: an integer that fits int64 is Int,
// one that needs uint64 is Uint, and one wider than 64 bits is Big, kept as its
// digits in `text`. A string keeps its decoded text there. A bool is `i`.
struct Leaf {
    enum class Kind : std::uint8_t {
        Null,
        Bool,
        Int,
        Uint,
        Double,
        Big,
        String
    };
    Kind kind = Kind::Null;
    union {
        std::int64_t i;
        std::uint64_t u;
        double d;
    };
    dstd::StringView text;
    Leaf() : i(0) {}
};

bool read_leaf_number(OnDemandValue& v, Leaf& out) {
    // A failed get_uint64 leaves the value unread for the general path.
    std::uint64_t plain;
    if (!v.get_uint64().get(plain)) {
        if (plain <= static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max())) {
            out.kind = Leaf::Kind::Int;
            out.i = static_cast<std::int64_t>(plain);
        } else {
            out.kind = Leaf::Kind::Uint;
            out.u = plain;
        }
        return true;
    }
    Number n;
    if (!read_number(v, n)) return false;
    switch (n.kind) {
        case Number::Kind::Int:
            out.kind = Leaf::Kind::Int;
            out.i = n.i;
            break;
        case Number::Kind::Uint:
            out.kind = Leaf::Kind::Uint;
            out.u = n.u;
            break;
        case Number::Kind::Double:
            out.kind = Leaf::Kind::Double;
            out.d = n.d;
            if (!std::isfinite(n.d))
                out.text = json::detail::trim_token(v.raw_json_token());
            break;
        case Number::Kind::Big:
            out.kind = Leaf::Kind::Big;
            out.text = json::detail::trim_token(v.raw_json_token());
            break;
    }
    return true;
}

// The scalar `v` of `type`; false on a read error or a text that is no JSON
// value.
bool read_leaf(OnDemandValue& v, OnDemandType type, dstd::StringArena& arena,
               Leaf& out) {
    switch (type) {
        case OnDemandType::string: {
            const auto text = json::detail::read_string(
                json::detail::trim_token(v.raw_json_token()), arena);
            if (!text) return false;
            out.kind = Leaf::Kind::String;
            out.text = *text;
            return true;
        }
        case OnDemandType::number:
            return read_leaf_number(v, out);
        case OnDemandType::boolean: {
            bool b;
            if (v.get_bool().get(b)) return false;
            out.kind = Leaf::Kind::Bool;
            out.i = b;
            return true;
        }
        case OnDemandType::null: {
            bool is_null;
            if (v.is_null().get(is_null) || !is_null) return false;
            out.kind = Leaf::Kind::Null;
            return true;
        }
        default:
            return false;
    }
}

// The scalar `leaf` as append_scalar_arg appends the DOM element it came from.
template <class Intern>
void append_leaf_arg(ArenaVec<FoldEvent::Arg>& into,
                     ArenaVec<FoldEvent::SpecialValue>& specials,
                     std::uint32_t key_id, const Leaf& leaf, Intern& intern,
                     bool top = false) {
    switch (leaf.kind) {
        case Leaf::Kind::String:
        case Leaf::Kind::Big:
            into.emplace_back(key_id, intern.get_or_insert(leaf.text));
            break;
        case Leaf::Kind::Int:
            into.emplace_back(key_id, leaf.i);
            break;
        case Leaf::Kind::Uint:
            append_uint_arg(into, key_id, leaf.u);
            break;
        case Leaf::Kind::Double:
            into.emplace_back(key_id, leaf.d);
            break;
        case Leaf::Kind::Bool:
            append_bool_arg(into, specials, key_id, leaf.i != 0, top);
            break;
        case Leaf::Kind::Null:
            specials.push_back({key_id, FoldEvent::Special::NullValue, top});
            break;
    }
}

// The PathType of `leaf`, as leaf_type_tag gives it for the DOM element.
std::uint8_t leaf_tag(const Leaf& leaf) {
    using dftracer::utils::index::store::PathType;
    PathType t = PathType::NullValue;
    switch (leaf.kind) {
        case Leaf::Kind::String:
            t = PathType::String;
            break;
        case Leaf::Kind::Int:
            t = PathType::Int;
            break;
        case Leaf::Kind::Uint:
            t = leaf.u <= static_cast<std::uint64_t>(
                              std::numeric_limits<std::int64_t>::max())
                    ? PathType::Int
                    : PathType::Uint;
            break;
        case Leaf::Kind::Double:
            t = PathType::Double;
            break;
        case Leaf::Kind::Bool:
            t = PathType::Bool;
            break;
        case Leaf::Kind::Null:
        case Leaf::Kind::Big:
            break;
    }
    return static_cast<std::uint8_t>(t);
}

std::uint8_t container_tag(OnDemandType type) {
    using dftracer::utils::index::store::PathType;
    return static_cast<std::uint8_t>(
        type == OnDemandType::object ? PathType::Object : PathType::Array);
}

// The PathType of the value `v` of `type` as leaf_type_tag gives the DOM
// element; false on a read error. A container is not read, a string is not
// decoded.
bool value_tag(OnDemandValue& v, OnDemandType type, std::uint8_t& tag) {
    using dftracer::utils::index::store::PathType;
    switch (type) {
        case OnDemandType::object:
        case OnDemandType::array:
            tag = container_tag(type);
            return true;
        case OnDemandType::string:
            tag = static_cast<std::uint8_t>(PathType::String);
            return true;
        case OnDemandType::number: {
            Leaf leaf;
            if (!read_leaf_number(v, leaf)) return false;
            tag = leaf_tag(leaf);
            return true;
        }
        case OnDemandType::boolean: {
            bool b;
            if (v.get_bool().get(b)) return false;
            tag = static_cast<std::uint8_t>(PathType::Bool);
            return true;
        }
        case OnDemandType::null: {
            bool is_null;
            if (v.is_null().get(is_null) || !is_null) return false;
            tag = static_cast<std::uint8_t>(PathType::NullValue);
            return true;
        }
        default:
            return false;
    }
}

// The phase `v` holds; false on a read error. A value that is no phase reads
// as Unknown.
bool read_phase_value(OnDemandValue& v, dstd::StringArena& arena,
                      RecordPhase& out) {
    OnDemandType type;
    if (v.type().get(type) || type == OnDemandType::unknown) return false;
    out = RecordPhase::Unknown;
    if (type == OnDemandType::number) {
        std::uint64_t plain;
        if (!v.get_uint64().get(plain)) {
            out = phase_from_int(plain);
            return true;
        }
        Number n;
        if (!read_number(v, n)) return false;
        if (n.kind == Number::Kind::Int && n.i >= 0)
            out = phase_from_int(static_cast<std::uint64_t>(n.i));
        else if (n.kind == Number::Kind::Uint)
            out = phase_from_int(n.u);
    } else if (type == OnDemandType::string) {
        const auto letter = json::detail::read_string(
            json::detail::trim_token(v.raw_json_token()), arena);
        if (!letter) return false;
        out = phase_from_letter(*letter);
    }
    return true;
}

// The most positions of an args object or a record whose keys are remembered.
constexpr std::size_t MAX_KEY_POSITIONS = 64;

// Two overlapping loads cover a key of up to 16 bytes without a library call.
bool same_bytes(const char* a, const char* b, std::size_t n) {
    if (n >= 8) {
        if (n > 16) return std::memcmp(a, b, n) == 0;
        std::uint64_t a0, a1, b0, b1;
        std::memcpy(&a0, a, 8);
        std::memcpy(&b0, b, 8);
        std::memcpy(&a1, a + n - 8, 8);
        std::memcpy(&b1, b + n - 8, 8);
        return ((a0 ^ b0) | (a1 ^ b1)) == 0;
    }
    if (n >= 4) {
        std::uint32_t a0, a1, b0, b1;
        std::memcpy(&a0, a, 4);
        std::memcpy(&b0, b, 4);
        std::memcpy(&a1, a + n - 4, 4);
        std::memcpy(&b1, b + n - 4, 4);
        return ((a0 ^ b0) | (a1 ^ b1)) == 0;
    }
    for (std::size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

// The id the previous record had for the same key at `pos`, else
// `resolve(key)`, which is remembered.
template <class Resolve>
std::uint32_t key_at(dftracer::utils::dstd::Vector<DecodeHints::KeyAt>& slots,
                     std::size_t pos, dstd::StringView key, Resolve&& resolve) {
    if (pos >= MAX_KEY_POSITIONS) return resolve(key);
    if (pos >= slots.size()) slots.resize(pos + 1);
    DecodeHints::KeyAt& slot = slots[pos];
    if (slot.id != dftracer::utils::dstd::StringIntern::NO_ID &&
        slot.key.size() == key.size() &&
        same_bytes(slot.key.data(), key.data(), key.size()))
        return slot.id;
    slot.id = resolve(key);
    slot.key.assign(key.data(), key.size());
    return slot.id;
}

// Where a reader without DecodeHints of its own keeps the leaves it defers.
DecodeHints& scratch_hints() {
    static thread_local DecodeHints scratch;
    return scratch;
}

// One event read in one forward walk of its text. Strings are views of the
// line or of the document's string arena, and live until the next parse.
class OnDemandReader {
   public:
    // With `capture_schema`, every leaf of the record also goes into
    // schema_leaves in the order the DOM form lists them: those of a non-empty
    // args object first when `hints` is given and the fold wants args, then
    // the other top-level ones.
    OnDemandReader(FoldEvent& ev, dftracer::utils::dstd::StringIntern& intern,
                   dftracer::utils::dstd::InternCache* cache, bool needs_args,
                   const char* end, dstd::StringArena& arena,
                   bool capture_schema = false, DecodeHints* hints = nullptr)
        : ev_(ev),
          out_(&ev),
          ids_{intern, cache},
          plain_{intern, nullptr},
          needs_args_(needs_args),
          end_(end),
          arena_(arena),
          schema_(capture_schema),
          walk_(capture_schema && needs_args && hints != nullptr),
          hints_(hints),
          scratch_(hints ? hints : &scratch_hints()) {}

    // Whether the walk found a phase, which makes the line an event.
    bool has_event() const { return has_event_; }

    // False on a read error. The last of a repeated top-level key wins, as in
    // the DOM form; so does the last args object, which discards what an
    // earlier one added.
    bool run(simdjson::ondemand::object& object) {
        FoldScalars s;
        bool has_ph = false;
        bool has_args = false;
        std::size_t top_pos = 0;
        if (schema_) {
            scratch_->deferred.clear();
            scratch_->deferred_text.clear();
        }
        for (auto member : object) {
            simdjson::ondemand::field field;
            if (std::move(member).get(field)) return false;
            const auto key = json::detail::read_key(field, arena_);
            if (!key) return false;
            OnDemandValue v = field.value();
            bool leaf = schema_;
            bool counted = schema_;
            bool in_args = false;
            switch (key->size()) {
                case 2:
                    if (is_key(*key, "ph")) {
                        if (!read_phase(v, s.phase)) return false;
                        has_ph = true;
                        leaf = counted = false;
                    } else if (is_key(*key, "ts")) {
                        if (read_unsigned(v, s.ts) == Field::Error)
                            return false;
                        leaf = counted = false;
                    } else if (is_key(*key, "id")) {
                        leaf = counted = false;
                    }
                    break;
                case 3:
                    if (is_key(*key, "pid")) {
                        if (read_unsigned(v, s.pid) == Field::Error)
                            return false;
                        leaf = counted = false;
                    } else if (is_key(*key, "tid")) {
                        if (read_unsigned(v, s.tid) == Field::Error)
                            return false;
                        leaf = counted = false;
                    } else if (is_key(*key, "cat")) {
                        if (!read_string_field(v, s.cat)) return false;
                    } else if (is_key(*key, "dur")) {
                        std::uint64_t dur;
                        const Field read = read_unsigned(v, dur);
                        if (read == Field::Error) return false;
                        if (read == Field::Used) {
                            s.dur = dur;
                            s.has_dur = true;
                        }
                        leaf = counted = false;
                    }
                    break;
                case 4:
                    if (is_key(*key, "name")) {
                        if (!read_string_field(v, s.name)) return false;
                    } else if (is_key(*key, "args")) {
                        OnDemandType type;
                        if (v.type().get(type)) return false;
                        in_args = true;
                        if (type != OnDemandType::object) break;
                        if (has_args) {
                            discard_args();
                        } else {
                            trace::internal::parse_walks::args_walk();
                        }
                        has_args = true;
                        if (has_ph) intern_head(s);
                        if (schema_)
                            args_deferred_at_ = scratch_->deferred.size();
                        if (!read_args(v)) return false;
                        leaf = false;
                    }
                    break;
                default:
                    break;
            }
            const std::size_t pos = counted ? top_pos++ : 0;
            if (leaf && !defer_top(*key, v, in_args, pos)) return false;
        }
        if (!has_ph) {
            reset_fold_event(ev_);
            return true;
        }
        set_fold_scalars(ev_, s, ids_, head_);
        has_event_ = true;
        if (schema_) emit_deferred();
        return true;
    }

    // Captures each of `names` as the DOM form does. The names resolve
    // together in one walk of the root and one of args; a name the walk cannot
    // settle on its own is resolved alone, reading `event` from its start
    // again. False on a read error.
    bool capture_all(json::Value& event,
                     const dstd::Vector<dstd::String>& names) {
        for (std::size_t from = 0; from < names.size(); from += BATCH) {
            const std::size_t count = std::min(BATCH, names.size() - from);
            if (!capture_batch(event, names.data() + from, count)) return false;
        }
        return true;
    }

   private:
    enum class Step : std::uint8_t { Found, Missing, Error };

    bool open(json::Value& event, OnDemandValue& root) {
        json::detail::ValueWalk walk;
        if (!json::detail::ValueAccess::begin_value(event, walk)) return false;
        root = walk.value;
        return true;
    }

    // The first member of `object` named `key`.
    Step find_key(simdjson::ondemand::object& object, dstd::StringView key,
                  OnDemandValue& out) {
        for (auto member : object) {
            simdjson::ondemand::field field;
            if (std::move(member).get(field)) return Step::Error;
            const auto name = json::detail::read_key(field, arena_);
            if (!name) return Step::Error;
            if (*name == key) {
                out = field.value();
                return Step::Found;
            }
        }
        return Step::Missing;
    }

    Step element_at(OnDemandValue& array_value, std::size_t index,
                    OnDemandValue& out) {
        simdjson::ondemand::array array;
        if (array_value.get_array().get(array)) return Step::Error;
        const simdjson::error_code code = array.at(index).get(out);
        if (code == simdjson::INDEX_OUT_OF_BOUNDS) return Step::Missing;
        return code ? Step::Error : Step::Found;
    }

    // The value at `path` below `start`, as JsonValue::at resolves it. With
    // `start_object`, the object cursor of `start` is kept there when the
    // first step opens it, and `opened` says so, so the caller can search it
    // again.
    Step resolve(OnDemandValue start, const char* path, OnDemandValue& out,
                 simdjson::ondemand::object* start_object = nullptr,
                 bool* opened = nullptr) {
        OnDemandValue current = start;
        bool at_start = true;
        const char* p = path;
        while (*p) {
            if (*p == '.') {
                ++p;
                continue;
            }
            OnDemandType type;
            if (current.type().get(type) || type == OnDemandType::unknown)
                return Step::Error;
            OnDemandValue next;
            if (*p == '[') {
                ++p;
                std::size_t index = 0;
                bool any = false;
                while (*p >= '0' && *p <= '9') {
                    index = index * 10 + static_cast<std::size_t>(*p - '0');
                    ++p;
                    any = true;
                }
                if (*p == ']') ++p;
                if (!any || type != OnDemandType::array) return Step::Missing;
                const Step step = element_at(current, index, next);
                if (step != Step::Found) return step;
                current = next;
                at_start = false;
                continue;
            }
            const char* const begin = p;
            while (*p && *p != '.' && *p != '[') ++p;
            const dstd::StringView key(begin,
                                       static_cast<std::size_t>(p - begin));
            Step step = Step::Missing;
            if (type == OnDemandType::object) {
                simdjson::ondemand::object local;
                simdjson::ondemand::object& object =
                    at_start && start_object ? *start_object : local;
                if (current.get_object().get(object)) return Step::Error;
                if (at_start && opened) *opened = true;
                step = find_key(object, key, next);
                if (step == Step::Missing && *p == '.') {
                    const char* run_end = p;
                    while (*run_end && *run_end != '[') ++run_end;
                    for (const char* end = run_end; end > p;) {
                        if (object.reset().error()) return Step::Error;
                        step = find_key(
                            object,
                            dstd::StringView(
                                begin, static_cast<std::size_t>(end - begin)),
                            next);
                        if (step == Step::Found) {
                            p = end;
                            break;
                        }
                        if (step == Step::Error) return step;
                        --end;
                        while (end > p && *end != '.') --end;
                    }
                }
            } else if (type == OnDemandType::array) {
                bool digits = !key.empty();
                std::size_t index = 0;
                for (char c : key) {
                    if (c < '0' || c > '9') {
                        digits = false;
                        break;
                    }
                    index = index * 10 + static_cast<std::size_t>(c - '0');
                }
                if (digits) step = element_at(current, index, next);
            }
            if (step != Step::Found) return step;
            current = next;
            at_start = false;
        }
        out = current;
        return Step::Found;
    }

    static constexpr std::size_t BATCH = 32;

    // A run of entries a capture added to the scratch event's lists.
    struct Chunk {
        std::uint32_t args0 = 0, args1 = 0;
        std::uint32_t top0 = 0, top1 = 0;
        std::uint32_t specials0 = 0, specials1 = 0;
        bool valid = false;
    };

    struct Slot {
        const dstd::String* name = nullptr;
        const char* path = nullptr;
        std::size_t seg_len = 0;
        std::size_t run_len = 0;
        bool schema_field = false;
        // The args fallback applies to the name.
        bool eligible = false;
        // Resolved by the merged walks; else alone.
        bool merged = false;
        bool root_done = false;
        bool root_found = false;
        bool args_done = false;
        bool root_flat = false;
        bool args_flat = false;
        bool in_args = false;
        Chunk root;
        Chunk args;
        Chunk whole;
    };

    static FoldEvent& capture_scratch() {
        static thread_local FoldEvent scratch;
        return scratch;
    }

    // The capture of `found` under `name`, appended to the sink.
    bool emit_found(OnDemandValue& found, const dstd::String& name,
                    bool schema_field) {
        const std::uint32_t key_id = ids_.get_or_insert(name);
        auto& into = schema_field ? out_->top_fields : out_->args;
        OnDemandType type;
        if (found.type().get(type) || type == OnDemandType::unknown)
            return false;
        const bool container =
            type == OnDemandType::object || type == OnDemandType::array;
        if (container && !schema_field) {
            path_.assign(name);
            return flatten(found, type);
        }
        if (container) {
            bool empty;
            if (type == OnDemandType::object) {
                simdjson::ondemand::object object;
                if (found.get_object().get(object) ||
                    object.is_empty().get(empty))
                    return false;
            } else {
                simdjson::ondemand::array array;
                if (found.get_array().get(array) || array.is_empty().get(empty))
                    return false;
            }
            if (empty)
                out_->specials.push_back({key_id,
                                          type == OnDemandType::array
                                              ? FoldEvent::Special::EmptyArray
                                              : FoldEvent::Special::EmptyObject,
                                          true});
            return true;
        }
        Leaf leaf;
        if (!read_leaf(found, type, arena_, leaf)) return false;
        append_leaf_arg(into, out_->specials, key_id, leaf, ids_, schema_field);
        return true;
    }

    // emit_found into the scratch event, remembering where.
    bool emit_chunk(OnDemandValue& found, Slot& slot, Chunk& chunk) {
        FoldEvent& scratch = capture_scratch();
        chunk.args0 = static_cast<std::uint32_t>(scratch.args.size());
        chunk.top0 = static_cast<std::uint32_t>(scratch.top_fields.size());
        chunk.specials0 = static_cast<std::uint32_t>(scratch.specials.size());
        FoldEvent* const saved = out_;
        out_ = &scratch;
        const bool ok = emit_found(found, *slot.name, slot.schema_field);
        out_ = saved;
        chunk.args1 = static_cast<std::uint32_t>(scratch.args.size());
        chunk.top1 = static_cast<std::uint32_t>(scratch.top_fields.size());
        chunk.specials1 = static_cast<std::uint32_t>(scratch.specials.size());
        chunk.valid = true;
        return ok;
    }

    // Where JsonValue::at and the args fallback put `name`, for a name resolved
    // alone, on the line read from its start.
    Step locate(json::Value& event, const dstd::String& name, bool schema_field,
                OnDemandValue& found) {
        OnDemandValue root;
        if (!open(event, root)) return Step::Error;
        simdjson::ondemand::object root_object;
        bool opened = false;
        Step step = resolve(root, name.c_str(), found, &root_object, &opened);
        if (step == Step::Missing && !schema_field &&
            !dftracer::utils::common::has_args_prefix(name)) {
            OnDemandValue args;
            if (opened) {
                if (root_object.reset().error()) return Step::Error;
            } else if (!open(event, root) ||
                       root.get_object().get(root_object)) {
                return Step::Error;
            }
            step = find_key(root_object, "args", args);
            if (step == Step::Found) step = resolve(args, name.c_str(), found);
        }
        return step;
    }

    // Whether `key` is a longer dotted key that JsonValue::at would try for
    // the name of `slot` when its first segment is missing.
    static bool dotted_candidate(const Slot& slot, dstd::StringView key) {
        return key.size() > slot.seg_len && key.size() <= slot.run_len &&
               std::memcmp(slot.path, key.data(), key.size()) == 0 &&
               (key.size() == slot.run_len || slot.path[key.size()] == '.');
    }

    static bool is_seg(const Slot& slot, dstd::StringView key) {
        return key.size() == slot.seg_len &&
               std::memcmp(slot.path, key.data(), key.size()) == 0;
    }

    // The members of args, for the slots that need it. A slot that cannot
    // settle here is left to resolve alone.
    bool walk_args(OnDemandValue& args, Slot* slots, std::size_t n) {
        OnDemandType type;
        if (args.type().get(type) || type == OnDemandType::unknown)
            return false;
        std::size_t open = 0;
        for (std::size_t i = 0; i < n; ++i) {
            Slot& slot = slots[i];
            if (!slot.merged || !slot.eligible ||
                (slot.root_done && slot.root_found))
                continue;
            if (type != OnDemandType::object) {
                slot.merged = false;
                continue;
            }
            slot.in_args = true;
            ++open;
        }
        if (open == 0) return true;
        simdjson::ondemand::object object;
        if (args.get_object().get(object)) return false;
        for (auto member : object) {
            simdjson::ondemand::field field;
            if (std::move(member).get(field)) return false;
            const auto key = json::detail::read_key(field, arena_);
            if (!key) return false;
            OnDemandValue child = field.value();
            for (std::size_t i = 0; i < n; ++i) {
                Slot& slot = slots[i];
                if (!slot.in_args || slot.args_done) continue;
                if (is_seg(slot, *key)) {
                    slot.args_done = true;
                    --open;
                    OnDemandValue found;
                    const Step step =
                        resolve(child, slot.path + slot.seg_len, found);
                    if (step == Step::Error) return false;
                    if (step == Step::Found &&
                        !emit_chunk(found, slot, slot.args))
                        return false;
                } else if (dotted_candidate(slot, *key)) {
                    slot.args_flat = true;
                }
            }
            if (open == 0) break;
        }
        return true;
    }

    // Whether the root walk has nothing left to decide.
    static bool settled(const Slot* slots, std::size_t n, bool args_seen) {
        for (std::size_t i = 0; i < n; ++i) {
            const Slot& slot = slots[i];
            if (!slot.merged) continue;
            if (!slot.root_done) return false;
            if (!slot.root_found && slot.eligible && !args_seen) return false;
        }
        return true;
    }

    bool capture_batch(json::Value& event, const dstd::String* names,
                       std::size_t n) {
        Slot slots[BATCH];
        FoldEvent& scratch = capture_scratch();
        scratch.args.clear();
        scratch.top_fields.clear();
        scratch.specials.clear();
        bool any_eligible = false;
        for (std::size_t i = 0; i < n; ++i) {
            Slot& slot = slots[i];
            slot.name = &names[i];
            slot.path = names[i].c_str();
            slot.schema_field =
                !is_nested_path(names[i]) && is_schema_field(names[i]);
            slot.eligible = !slot.schema_field &&
                            !dftracer::utils::common::has_args_prefix(names[i]);
            any_eligible = any_eligible || slot.eligible;
            const char* p = slot.path;
            while (*p && *p != '.' && *p != '[') ++p;
            slot.seg_len = static_cast<std::size_t>(p - slot.path);
            while (*p && *p != '[') ++p;
            slot.run_len = static_cast<std::size_t>(p - slot.path);
            slot.merged = slot.seg_len > 0;
        }
        std::size_t merged = 0;
        for (std::size_t i = 0; i < n; ++i) {
            Slot& slot = slots[i];
            if (!slot.merged) continue;
            // The args member is read by the args walk, and one value is read
            // once; a name with the same first segment as an earlier one
            // cannot share it either.
            bool taken = any_eligible && slot.seg_len == 4 &&
                         std::memcmp(slot.path, "args", 4) == 0;
            for (std::size_t j = 0; j < i && !taken; ++j)
                taken =
                    slots[j].merged && slots[j].seg_len == slot.seg_len &&
                    std::memcmp(slots[j].path, slot.path, slot.seg_len) == 0;
            slot.merged = !taken;
            merged += slot.merged ? 1 : 0;
        }
        if (merged > 0) {
            OnDemandValue root;
            simdjson::ondemand::object object;
            if (!open(event, root) || root.get_object().get(object))
                return false;
            FoldEvent* const saved = out_;
            bool args_seen = false;
            for (auto member : object) {
                simdjson::ondemand::field field;
                if (std::move(member).get(field)) return false;
                const auto key = json::detail::read_key(field, arena_);
                if (!key) return false;
                OnDemandValue child = field.value();
                for (std::size_t i = 0; i < n; ++i) {
                    Slot& slot = slots[i];
                    if (!slot.merged || slot.root_done) continue;
                    if (is_seg(slot, *key)) {
                        slot.root_done = true;
                        OnDemandValue found;
                        const Step step =
                            resolve(child, slot.path + slot.seg_len, found);
                        if (step == Step::Error) return false;
                        if (step == Step::Found) {
                            slot.root_found = true;
                            if (!emit_chunk(found, slot, slot.root))
                                return false;
                        }
                    } else if (dotted_candidate(slot, *key)) {
                        slot.root_flat = true;
                    }
                }
                if (!args_seen && any_eligible && key->size() == 4 &&
                    std::memcmp(key->data(), "args", 4) == 0) {
                    args_seen = true;
                    if (!walk_args(child, slots, n)) return false;
                }
                if (settled(slots, n, args_seen)) break;
            }
            out_ = saved;
        }
        for (std::size_t i = 0; i < n; ++i) {
            Slot& slot = slots[i];
            if (slot.merged &&
                ((!slot.root_done && slot.root_flat) ||
                 (!slot.root_found && slot.args_flat && !slot.args_done)))
                slot.merged = false;
            if (slot.merged) continue;
            OnDemandValue found;
            const Step step =
                locate(event, *slot.name, slot.schema_field, found);
            if (step == Step::Error) return false;
            if (step == Step::Found && !emit_chunk(found, slot, slot.whole))
                return false;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const Slot& slot = slots[i];
            const Chunk& chunk = !slot.merged      ? slot.whole
                                 : slot.root_found ? slot.root
                                                   : slot.args;
            if (!chunk.valid) continue;
            const std::uint32_t key_id = ids_.get_or_insert(*slot.name);
            auto& into = slot.schema_field ? ev_.top_fields : ev_.args;
            bool present = false;
            for (const auto& [k, existing] : into)
                present = present || k == key_id;
            for (const auto& special : ev_.specials)
                present = present || (special.key == key_id &&
                                      special.top == slot.schema_field);
            if (present) continue;
            for (std::uint32_t k = chunk.args0; k < chunk.args1; ++k)
                ev_.args.push_back(scratch.args[k]);
            for (std::uint32_t k = chunk.top0; k < chunk.top1; ++k)
                ev_.top_fields.push_back(scratch.top_fields[k]);
            for (std::uint32_t k = chunk.specials0; k < chunk.specials1; ++k)
                ev_.specials.push_back(scratch.specials[k]);
        }
        return true;
    }

    // The decoded string at `v`; false on an invalid escape.
    bool read_text(OnDemandValue& v, dstd::StringView& out) {
        const auto text = json::detail::read_string(
            json::detail::trim_token(v.raw_json_token()), arena_);
        if (!text) return false;
        out = *text;
        return true;
    }

    // A string field keeps its last string value; another type leaves it.
    bool read_string_field(OnDemandValue& v, dstd::StringView& out) {
        OnDemandType type;
        if (v.type().get(type) || type == OnDemandType::unknown) return false;
        return type != OnDemandType::string || read_text(v, out);
    }

    bool read_phase(OnDemandValue& v, RecordPhase& out) {
        return read_phase_value(v, arena_, out);
    }

    void discard_args() {
        // The DOM form lists the leaves of an args object that a later one
        // replaces among the top-level ones when the later one has none.
        if (!ev_.schema_leaves.empty()) {
            dstd::Vector<DecodeHints::DeferredLeaf> listed;
            for (const auto& [id, tag] : ev_.schema_leaves) {
                DecodeHints::DeferredLeaf leaf;
                leaf.id = id;
                leaf.tag = tag;
                leaf.resolved = true;
                leaf.in_args = true;
                listed.push_back(leaf);
            }
            auto& deferred = scratch_->deferred;
            deferred.insert(deferred.begin() + args_deferred_at_,
                            listed.begin(), listed.end());
        }
        ev_.args.clear();
        ev_.specials.clear();
        ev_.schema_leaves.clear();
        args_walked_ = false;
        ev_.fhash_id = dftracer::utils::dstd::StringIntern::NO_ID;
        ev_.hhash_id = dftracer::utils::dstd::StringIntern::NO_ID;
    }

    // fhash/hhash are group dimensions even for folds that want no other args,
    // so they are always harvested; the rest only when a fold asks.
    bool read_args(OnDemandValue& args) {
        simdjson::ondemand::object object;
        if (args.get_object().get(object)) return false;
        bool any = false;
        std::size_t at = 0;
        for (auto member : object) {
            any = true;
            const std::size_t pos = at++;
            simdjson::ondemand::field field;
            if (std::move(member).get(field)) return false;
            const auto key = json::detail::read_key(field, arena_);
            if (!key) return false;
            OnDemandValue v = field.value();
            OnDemandType type;
            if (v.type().get(type)) return false;
            if (key->size() == 5 &&
                (is_key(*key, "fhash") || is_key(*key, "hhash"))) {
                std::uint32_t id = dftracer::utils::dstd::StringIntern::NO_ID;
                if (type == OnDemandType::string) {
                    dstd::StringView text;
                    if (!read_text(v, text)) return false;
                    id = ids_.get_or_insert(text);
                }
                (is_key(*key, "fhash") ? ev_.fhash_id : ev_.hhash_id) = id;
                if (walk_) {
                    std::uint8_t tag;
                    if (!value_tag(v, type, tag)) return false;
                    ev_.schema_leaves.emplace_back(
                        arg_leaf_id(*hints_, plain_.intern, key_id(pos, *key),
                                    *key),
                        tag);
                } else if (schema_ && !enumerate_arg(*key, v, type)) {
                    return false;
                }
            } else if (needs_args_) {
                if (type == OnDemandType::object ||
                    type == OnDemandType::array) {
                    path_.assign(*key);
                    if (!flatten(v, type)) return false;
                } else {
                    const std::uint32_t id = key_id(pos, *key);
                    std::uint8_t tag;
                    if (!read_scalar(id, v, type, ids_, tag)) return false;
                    note_leaf(id, *key, tag);
                }
            } else if (schema_ && !enumerate_arg(*key, v, type)) {
                return false;
            }
        }
        if (!schema_) return true;
        if (!any)
            defer_path("args", container_tag(OnDemandType::object), true);
        else if (walk_)
            args_walked_ = true;
        return true;
    }

    // The id of the args key at position `pos`, which the previous record's key
    // at the same position gives without a lookup when it is the same.
    std::uint32_t key_id(std::size_t pos, dstd::StringView key) {
        if (!hints_) return ids_.get_or_insert(key);
        return key_at(hints_->arg_key_at, pos, key, [&](dstd::StringView k) {
            return ids_.get_or_insert(k);
        });
    }

    // A leaf of an argument at `key`, the key of its flattened value: listed
    // at once when the args walk lists the leaves, else deferred as the DOM
    // form lists "args.<key>" among the top-level leaves.
    void note_leaf(std::uint32_t key_id, dstd::StringView key,
                   std::uint8_t tag) {
        if (!schema_ || out_ != &ev_) return;
        if (walk_) {
            ev_.schema_leaves.emplace_back(
                arg_leaf_id(*hints_, plain_.intern, key_id, key), tag);
            return;
        }
        enum_path_.assign("args.");
        enum_path_.append(key);
        defer_path(enum_path_, tag, true);
    }

    void defer_path(dstd::StringView path, std::uint8_t tag, bool in_args) {
        DecodeHints::DeferredLeaf leaf;
        leaf.offset = scratch_->deferred_text.size();
        leaf.size = path.size();
        leaf.tag = tag;
        leaf.composed = true;
        leaf.in_args = in_args;
        scratch_->deferred_text.append(path);
        scratch_->deferred.push_back(leaf);
    }

    // The leaves of the top-level value `v` at `key`, deferred.
    bool defer_top(dstd::StringView key, OnDemandValue& v, bool in_args,
                   std::size_t pos) {
        OnDemandType type;
        if (v.type().get(type)) return false;
        if (type == OnDemandType::object || type == OnDemandType::array) {
            enum_path_.assign(key);
            return enumerate(v, type, 0, in_args);
        }
        std::uint8_t tag;
        if (!value_tag(v, type, tag)) return false;
        DecodeHints::DeferredLeaf leaf;
        leaf.key = key;
        leaf.pos = pos;
        leaf.tag = tag;
        leaf.in_args = in_args;
        scratch_->deferred.push_back(leaf);
        return true;
    }

    // The leaves of the argument `key`, deferred as "args.<key>...".
    bool enumerate_arg(dstd::StringView key, OnDemandValue& v,
                       OnDemandType type) {
        enum_path_.assign("args.");
        enum_path_.append(key);
        return enumerate(v, type, 0, true);
    }

    // Defers every leaf of `v` under enum_path_ as enumerate_leaves lists the
    // DOM element: a non-empty container by its members, anything else, an
    // empty container included, as a leaf of its own. A container below the
    // depth limit is a leaf.
    bool enumerate(OnDemandValue& v, OnDemandType type, std::size_t depth,
                   bool in_args) {
        if (type == OnDemandType::object || type == OnDemandType::array) {
            if (depth >= MAX_READ_DEPTH) {
                defer_path(enum_path_, container_tag(type), in_args);
                return true;
            }
            bool any = false;
            if (type == OnDemandType::object) {
                simdjson::ondemand::object object;
                if (v.get_object().get(object)) return false;
                for (auto member : object) {
                    any = true;
                    simdjson::ondemand::field field;
                    if (std::move(member).get(field)) return false;
                    const auto key = json::detail::read_key(field, arena_);
                    if (!key) return false;
                    OnDemandValue child = field.value();
                    OnDemandType child_type;
                    if (child.type().get(child_type)) return false;
                    const std::size_t base = enum_path_.size();
                    if (base) enum_path_.push_back('.');
                    enum_path_.append(*key);
                    if (!enumerate(child, child_type, depth + 1, in_args))
                        return false;
                    enum_path_.resize(base);
                }
            } else {
                simdjson::ondemand::array array;
                if (v.get_array().get(array)) return false;
                std::size_t index = 0;
                for (auto element : array) {
                    any = true;
                    OnDemandValue child;
                    if (element.get(child)) return false;
                    OnDemandType child_type;
                    if (child.type().get(child_type)) return false;
                    const std::size_t base = enum_path_.size();
                    if (base) enum_path_.push_back('.');
                    dftracer::utils::trace::detail::append_index(enum_path_,
                                                                 index++);
                    if (!enumerate(child, child_type, depth + 1, in_args))
                        return false;
                    enum_path_.resize(base);
                }
            }
            if (any) return true;
        }
        std::uint8_t tag;
        if (!value_tag(v, type, tag)) return false;
        if (!enum_path_.empty()) defer_path(enum_path_, tag, in_args);
        return true;
    }

    // Lists the deferred leaves, interning their paths after everything else
    // the event interned. The leaves under args are dropped when the args walk
    // listed them.
    void emit_deferred() {
        const bool args_listed = walk_ && args_walked_;
        for (const auto& leaf : scratch_->deferred) {
            if (leaf.in_args && args_listed) continue;
            std::uint32_t id;
            if (leaf.resolved) {
                id = leaf.id;
            } else if (leaf.composed) {
                id = intern_leaf_path(
                    plain_.intern,
                    dstd::StringView(
                        scratch_->deferred_text.data() + leaf.offset,
                        leaf.size));
            } else if (leaf.key.empty()) {
                continue;
            } else if (hints_) {
                id = key_at(hints_->top_key_at, leaf.pos, leaf.key,
                            [&](dstd::StringView k) {
                                return top_leaf_id(*hints_, plain_.intern, k);
                            });
            } else {
                id = intern_leaf_path(plain_.intern, leaf.key);
            }
            ev_.schema_leaves.emplace_back(id, leaf.tag);
        }
    }

    // Interns the cat and name of the event ahead of its args, as the DOM form
    // does, once the line is known to be an event.
    void intern_head(const FoldScalars& s) {
        if (head_.done) return;
        head_.done = true;
        head_.cat = s.cat;
        head_.name = s.name;
        if (!s.cat.empty()) ev_.cat_id = ids_.get_or_insert(s.cat);
        if (!s.name.empty()) ev_.name_id = ids_.get_or_insert(s.name);
    }

    // The members or elements of `container` under path_, one leaf each. An
    // empty array or object is a leaf of its own.
    bool flatten(OnDemandValue& container, OnDemandType type) {
        bool any = false;
        if (type == OnDemandType::object) {
            simdjson::ondemand::object object;
            if (container.get_object().get(object)) return false;
            for (auto member : object) {
                any = true;
                simdjson::ondemand::field field;
                if (std::move(member).get(field)) return false;
                const auto key = json::detail::read_key(field, arena_);
                if (!key) return false;
                const std::size_t base = enter_key(path_, *key);
                OnDemandValue v = field.value();
                if (!flatten_member(v)) return false;
                path_.resize(base);
            }
        } else {
            simdjson::ondemand::array array;
            if (container.get_array().get(array)) return false;
            std::size_t index = 0;
            for (auto element : array) {
                any = true;
                OnDemandValue v;
                if (element.get(v)) return false;
                const std::size_t base = enter_index(path_, index++);
                if (!flatten_member(v)) return false;
                path_.resize(base);
            }
        }
        if (!any) {
            const std::uint32_t id = plain_.get_or_insert(path_);
            out_->specials.push_back({id,
                                      type == OnDemandType::object
                                          ? FoldEvent::Special::EmptyObject
                                          : FoldEvent::Special::EmptyArray,
                                      false});
            note_leaf(id, path_, container_tag(type));
        }
        return true;
    }

    bool flatten_member(OnDemandValue& v) {
        OnDemandType type;
        if (v.type().get(type)) return false;
        if (type == OnDemandType::object || type == OnDemandType::array) {
            if (depth_ + 1 >= MAX_READ_DEPTH) return keep_raw(v, type);
            ++depth_;
            const bool ok = flatten(v, type);
            --depth_;
            return ok;
        }
        const std::uint32_t id = plain_.get_or_insert(path_);
        std::uint8_t tag;
        if (!read_scalar(id, v, type, plain_, tag)) return false;
        note_leaf(id, path_, tag);
        return true;
    }

    // A container below the depth limit stays under its path as its JSON text.
    bool keep_raw(OnDemandValue& v, OnDemandType type) {
        const std::uint32_t id = plain_.get_or_insert(path_);
        out_->args.emplace_back(id,
                                plain_.get_or_insert(json::detail::value_extent(
                                    v.raw_json_token().data(), end_)));
        note_leaf(id, path_, container_tag(type));
        return true;
    }

    // Appends the number `v` under `key_id`; false when it is not a number.
    // `tag` is the PathType the DOM gives the number.
    bool read_number_arg(std::uint32_t key_id, OnDemandValue& v, Ids& ids,
                         std::uint8_t& tag) {
        using dftracer::utils::index::store::PathType;
        // A failed get_uint64 leaves the value unread for the general path.
        std::uint64_t plain;
        if (!v.get_uint64().get(plain)) {
            tag = static_cast<std::uint8_t>(PathType::Int);
            if (plain <= static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max())) {
                out_->args.emplace_back(key_id,
                                        static_cast<std::int64_t>(plain));
            } else {
                tag = static_cast<std::uint8_t>(PathType::Uint);
                append_uint_arg(out_->args, key_id, plain);
            }
            return true;
        }
        simdjson::ondemand::number n;
        const simdjson::error_code code = v.get_number().get(n);
        if (code == simdjson::BIGINT_ERROR ||
            (code == simdjson::NUMBER_ERROR && refused_big_integer(v))) {
            // Wider than 64 bits: kept exact as its digits.
            tag = static_cast<std::uint8_t>(PathType::NullValue);
            out_->args.emplace_back(
                key_id, ids.get_or_insert(json::detail::value_extent(
                            v.raw_json_token().data(), end_)));
            return true;
        }
        if (code == simdjson::NUMBER_ERROR) {
            const auto refused = refused_real(v);
            if (!refused) return false;
            tag = static_cast<std::uint8_t>(PathType::Double);
            out_->args.emplace_back(key_id, *refused);
            return true;
        }
        if (code) return false;
        if (n.is_double()) {
            tag = static_cast<std::uint8_t>(PathType::Double);
            out_->args.emplace_back(key_id, n.get_double());
        } else if (n.is_int64()) {
            tag = static_cast<std::uint8_t>(PathType::Int);
            out_->args.emplace_back(key_id, n.get_int64());
        } else {
            tag = static_cast<std::uint8_t>(PathType::Uint);
            append_uint_arg(out_->args, key_id, n.get_uint64());
        }
        return true;
    }

    // Appends the scalar `v` under `key_id` as append_scalar_arg does; false
    // on a read error or a text that is no JSON value. `tag` is the PathType
    // of the leaf.
    bool read_scalar(std::uint32_t key_id, OnDemandValue& v, OnDemandType type,
                     Ids& ids, std::uint8_t& tag) {
        using dftracer::utils::index::store::PathType;
        switch (type) {
            case OnDemandType::string: {
                dstd::StringView text;
                if (!read_text(v, text)) return false;
                tag = static_cast<std::uint8_t>(PathType::String);
                out_->args.emplace_back(key_id, ids.get_or_insert(text));
                return true;
            }
            case OnDemandType::number:
                return read_number_arg(key_id, v, ids, tag);
            case OnDemandType::boolean: {
                bool b;
                if (v.get_bool().get(b)) return false;
                tag = static_cast<std::uint8_t>(PathType::Bool);
                append_bool_arg(out_->args, out_->specials, key_id, b, false);
                return true;
            }
            case OnDemandType::null: {
                bool is_null;
                if (v.is_null().get(is_null) || !is_null) return false;
                tag = static_cast<std::uint8_t>(PathType::NullValue);
                out_->specials.push_back(
                    {key_id, FoldEvent::Special::NullValue, false});
                return true;
            }
            default:
                return false;
        }
    }

    FoldEvent& ev_;
    // Where the argument readers append: ev_, or the scratch event of a
    // capture.
    FoldEvent* out_;
    Ids ids_;
    Ids plain_;
    bool needs_args_;
    bool has_event_ = false;
    const char* end_;
    dstd::StringArena& arena_;
    dstd::String path_;
    std::size_t depth_ = 0;
    bool schema_;
    // The leaves of a non-empty args object go straight into schema_leaves.
    bool walk_;
    bool args_walked_ = false;
    std::size_t args_deferred_at_ = 0;
    DecodeHints* hints_;
    DecodeHints* scratch_;
    InternedHead head_;
    dstd::String enum_path_;
};

}  // namespace

namespace {

enum class DftracerRead : std::uint8_t { Event, NotEvent, Broken };

DftracerRead read_dftracer(FoldEvent& ev, json::Value& event,
                           dftracer::utils::dstd::StringIntern& intern,
                           bool needs_args,
                           dftracer::utils::dstd::InternCache* cache,
                           const dstd::Vector<dstd::String>* captures,
                           bool capture_schema, DecodeHints* hints) {
    reset_fold_event(ev);
    json::detail::ObjectWalk walk;
    if (!json::detail::ValueAccess::begin_object(event, walk))
        return DftracerRead::NotEvent;
    trace::internal::parse_walks::record_walk();
    OnDemandReader reader(ev, intern, cache, needs_args, walk.end,
                          *walk.strings, capture_schema, hints);
    bool ok = reader.run(walk.object);
    if (ok && reader.has_event() && captures && !captures->empty())
        ok = reader.capture_all(event, *captures);
    if (!ok || !json::detail::ValueAccess::end_object(event)) {
        json::detail::ValueAccess::mark_broken(event);
        reset_fold_event(ev);
        return DftracerRead::Broken;
    }
    return reader.has_event() ? DftracerRead::Event : DftracerRead::NotEvent;
}

}  // namespace

void extract_fold_event_into(FoldEvent& ev, json::Value& event,
                             dftracer::utils::dstd::StringIntern& intern,
                             bool needs_args,
                             dftracer::utils::dstd::InternCache* cache,
                             const dstd::Vector<dstd::String>* nested_captures,
                             bool capture_schema, DecodeHints* hints) {
    read_dftracer(ev, event, intern, needs_args, cache, nested_captures,
                  capture_schema, hints);
}

bool append_dftracer_record(FoldEventBuffer& batch, json::Value& record,
                            dftracer::utils::dstd::StringIntern& intern,
                            bool needs_args, bool capture_schema,
                            DecodeHints& hints,
                            dftracer::utils::dstd::InternCache* cache,
                            const dstd::Vector<dstd::String>* extra_fields) {
    FoldEvent& ev = batch.append();
    if (read_dftracer(ev, record, intern, needs_args, cache, extra_fields,
                      capture_schema, &hints) == DftracerRead::Event)
        return true;
    batch.discard_last();
    return false;
}

namespace {

// The leaves of `v` under exact path `path`: scalars into ev.args and, with
// `schema`, every leaf, null included, into ev.schema_leaves. With `paths`
// (sorted), only those leaves, descending only toward them and stopping once
// `left` of them remain to find.
struct LeafDecode {
    dftracer::utils::dstd::StringIntern& intern;
    FoldEvent& ev;
    bool schema;
    const dstd::Vector<dstd::String>* paths;
    std::size_t left;
    const dftracer::utils::index::RecordSchema* record_schema;
    // With `paths`, the declared field at each path (null when undeclared).
    const dstd::Vector<const dftracer::utils::index::FieldSpec*>* path_fields;
    // Whether a json field can be decoded, so objects and arrays are checked.
    bool json;
    std::size_t max_children;
    DecodeHints* hints;
    bool skip_never;
    // Interns the paths and string values of a record read on demand.
    dftracer::utils::dstd::InternCache* cache;

    Ids ids() const { return Ids{intern, cache}; }

    // The declared field at `path`, found at `pos` in `paths` when set, or
    // remembered by the leaf's path id in `hints`.
    const dftracer::utils::index::FieldSpec* field(
        const dstd::String& path, std::size_t pos,
        std::uint32_t id = dftracer::utils::dstd::StringIntern::NO_ID) const {
        if (path_fields) return (*path_fields)[pos];
        if (!record_schema) return nullptr;
        if (!hints || id >= DecodeHints::FIELD_CACHE_IDS)
            return record_schema->field_at(path);
        if (hints->schema != record_schema) {
            hints->schema = record_schema;
            hints->fields.clear();
            hints->known.clear();
        }
        if (id >= hints->fields.size()) {
            hints->fields.resize(id + 1, nullptr);
            hints->known.resize(id + 1, 0);
        }
        if (!hints->known[id]) {
            hints->fields[id] = record_schema->field_at(path);
            hints->known[id] = 1;
        }
        return hints->fields[id];
    }
};

// The position of `path` in the sorted `paths`, or nullopt.
dstd::Optional<std::size_t> position(const dstd::Vector<dstd::String>& paths,
                                     const dstd::String& path) {
    const auto it = std::lower_bound(paths.begin(), paths.end(), path);
    if (it == paths.end() || *it != path) return dstd::nullopt;
    return static_cast<std::size_t>(it - paths.begin());
}

Leaf leaf_of(simdjson::dom::element v) {
    using T = simdjson::dom::element_type;
    Leaf leaf;
    switch (v.type()) {
        case T::STRING:
            leaf.kind = Leaf::Kind::String;
            leaf.text = v.get_string().value_unsafe();
            break;
        case T::INT64:
            leaf.kind = Leaf::Kind::Int;
            leaf.i = v.get_int64().value_unsafe();
            break;
        case T::UINT64:
            leaf.kind = Leaf::Kind::Uint;
            leaf.u = v.get_uint64().value_unsafe();
            break;
        case T::DOUBLE:
            leaf.kind = Leaf::Kind::Double;
            leaf.d = v.get_double().value_unsafe();
            break;
        case T::BOOL:
            leaf.kind = Leaf::Kind::Bool;
            leaf.i = v.get_bool().value_unsafe();
            break;
        case T::BIGINT:
            leaf.kind = Leaf::Kind::Big;
            leaf.text = v.get_bigint().value_unsafe();
            break;
        default:
            break;
    }
    return leaf;
}

// The canonical JSON text of the scalar `leaf`, as append_canonical_json
// writes it.
void append_leaf_canonical(dstd::String& out, const Leaf& leaf) {
    char buf[32];
    switch (leaf.kind) {
        case Leaf::Kind::String:
            out += '"';
            dftracer::utils::json::append_json_escaped(out, leaf.text);
            out += '"';
            break;
        case Leaf::Kind::Big:
            out += leaf.text;
            break;
        case Leaf::Kind::Int:
            if (char* end = dstd::to_chars_i64(buf, buf + sizeof(buf), leaf.i))
                out.append(buf, end);
            break;
        case Leaf::Kind::Uint:
            if (char* end = dstd::to_chars_u64(buf, buf + sizeof(buf), leaf.u))
                out.append(buf, end);
            break;
        case Leaf::Kind::Double:
            if (!leaf.text.empty())
                out += leaf.text;
            else if (char* end =
                         dstd::to_chars_double(buf, buf + sizeof(buf), leaf.d))
                out.append(buf, end);
            break;
        case Leaf::Kind::Bool:
            out += leaf.i ? "true" : "false";
            break;
        case Leaf::Kind::Null:
            out += "null";
            break;
    }
}

// A declared field's value as its type: integers stay integers, whole doubles
// and integer text become integers, numbers and number text become doubles
// for a float field, and any scalar becomes its JSON text for a string field.
// A value that does not convert is left out, so the column keeps its declared
// type and the value reads as null. Role fields also set the event's time and
// duration in microseconds, its pid (entity), tid (lane) and name; a time may
// also be ISO-8601 text.
void bind_leaf(const dftracer::utils::index::FieldSpec& f, std::uint32_t id,
               const Leaf& v, LeafDecode& d) {
    using K = Leaf::Kind;
    Ids ids = d.ids();
    const bool numeric =
        v.kind == K::Int || v.kind == K::Uint || v.kind == K::Double;
    dstd::Optional<double> num;
    if (numeric)
        num = v.kind == K::Int    ? static_cast<double>(v.i)
              : v.kind == K::Uint ? static_cast<double>(v.u)
                                  : v.d;
    dstd::Optional<duql::Number> text_num;
    if (v.kind == K::String &&
        (f.type == index::FieldType::Int || f.type == index::FieldType::Float ||
         f.role == index::Role::Time || f.role == index::Role::Duration))
        text_num = duql::parse_number(v.text);
    auto keep = [&] { append_leaf_arg(d.ev.args, d.ev.specials, id, v, ids); };
    // A null in the record is null in every type.
    auto unconverted = [&] {
        if (v.kind != K::Null) ++d.ev.unconverted;
    };
    // False when `n` is not a whole number.
    auto as_int = [&](const duql::Number& n) {
        if (const auto* i = std::get_if<std::int64_t>(&n))
            d.ev.args.emplace_back(id, *i);
        else if (const auto* u = std::get_if<std::uint64_t>(&n))
            d.ev.args.emplace_back(id, *u);
        else if (auto whole = index::whole_int64(std::get<double>(n)))
            d.ev.args.emplace_back(id, *whole);
        else
            return false;
        return true;
    };
    // An ISO-8601 time is its microseconds once decoded.
    if (f.role == index::Role::Time && f.type == index::FieldType::String &&
        v.kind == K::String) {
        if (const auto us = index::iso8601_micros(v.text)) {
            d.ev.args.emplace_back(id, *us);
            if (*us >= 0) d.ev.ts = static_cast<std::uint64_t>(*us);
        }
        return;
    }
    switch (f.type) {
        case index::FieldType::String:
            if (v.kind == K::String || v.kind == K::Null) {
                keep();
            } else {
                dstd::String text;
                append_leaf_canonical(text, v);
                d.ev.args.emplace_back(id, ids.get_or_insert(text));
            }
            break;
        case index::FieldType::Bool:
            if (v.kind != K::Bool) return unconverted();
            keep();
            break;
        case index::FieldType::Float:
            if (numeric) {
                d.ev.args.emplace_back(id, *num);
            } else if (text_num) {
                num = std::visit([](auto x) { return static_cast<double>(x); },
                                 *text_num);
                d.ev.args.emplace_back(id, *num);
            } else {
                return unconverted();
            }
            break;
        case index::FieldType::Json: {
            dstd::String text;
            append_leaf_canonical(text, v);
            d.ev.args.emplace_back(id, ids.get_or_insert(text));
            return;
        }
        case index::FieldType::Int:
            if (v.kind == K::Int || v.kind == K::Uint) {
                keep();
            } else if (const auto n = v.kind == K::Double
                                          ? dstd::Optional<duql::Number>(*num)
                                          : text_num;
                       n && as_int(*n)) {
                num = std::visit([](auto x) { return static_cast<double>(x); },
                                 *n);
            } else {
                return unconverted();
            }
            break;
    }
    if (!num && text_num)
        num = std::visit([](auto x) { return static_cast<double>(x); },
                         *text_num);
    dstd::Optional<double> micros;
    if (num)
        micros = index::micros_per(f.unit.value_or(index::TimeUnit::Us)) * *num;
    switch (f.role) {
        case index::Role::None:
            break;
        case index::Role::Time:
            if (micros && *micros >= 0)
                d.ev.ts = static_cast<std::uint64_t>(std::llround(*micros));
            break;
        case index::Role::Duration:
            if (micros && *micros >= 0) {
                d.ev.dur = static_cast<std::uint64_t>(std::llround(*micros));
                d.ev.has_dur = true;
            }
            break;
        case index::Role::Entity:
        case index::Role::Lane: {
            if (d.ev.args.empty() || d.ev.args.back().first != id) break;
            const auto& value = d.ev.args.back().second;
            std::uint64_t lane = 0;
            if (const auto* i = std::get_if<std::int64_t>(&value))
                lane = static_cast<std::uint64_t>(*i);
            else if (const auto* u = std::get_if<std::uint64_t>(&value))
                lane = *u;
            else if (const auto* sid = std::get_if<std::uint32_t>(&value))
                lane = static_cast<std::uint64_t>(
                    index::entity_id(d.intern.resolve(*sid)));
            (f.role == index::Role::Entity ? d.ev.pid : d.ev.tid) = lane;
            break;
        }
        case index::Role::Name:
            if (!d.ev.args.empty() && d.ev.args.back().first == id)
                if (const auto* sid =
                        std::get_if<std::uint32_t>(&d.ev.args.back().second))
                    d.ev.name_id = *sid;
            break;
    }
}

bool wanted_below(const dstd::Vector<dstd::String>& paths,
                  const dstd::String& path) {
    auto it = std::lower_bound(paths.begin(), paths.end(), path);
    for (; it != paths.end() && it->starts_with(path); ++it)
        if (it->size() > path.size() && (*it)[path.size()] == '.') return true;
    return false;
}

// A declared json field at an object or array: its canonical text, keyed by
// the field's path; the leaves below are decoded as well.
void capture_json(const dstd::String& path, simdjson::dom::element v,
                  LeafDecode& d) {
    std::size_t pos = 0;
    if (d.paths) {
        const auto at = position(*d.paths, path);
        if (!at) return;
        pos = *at;
    }
    const auto* f = d.field(path, pos);
    if (!f || f->type != dftracer::utils::index::FieldType::Json) return;
    if (d.paths) --d.left;
    dstd::String text;
    dftracer::utils::json::append_canonical_json(text, v);
    const std::uint32_t id = d.intern.get_or_insert(path);
    d.ev.args.emplace_back(id, d.intern.get_or_insert(text));
    d.ev.specials.push_back({id, v.is_array()
                                     ? FoldEvent::Special::JsonArray
                                     : FoldEvent::Special::JsonObject});
}

// An array or object past max_children: one leaf of its canonical text.
void collapse(const dstd::String& path, simdjson::dom::element v,
              LeafDecode& d) {
    const std::uint32_t id = d.intern.get_or_insert(path);
    // A declared json field already holds its text.
    if (!d.ev.args.empty() && d.ev.args.back().first == id) return;
    if (d.schema) d.ev.schema_leaves.emplace_back(id, leaf_type_tag(v));
    dstd::String text;
    dftracer::utils::json::append_canonical_json(text, v);
    d.ev.args.emplace_back(id, d.intern.get_or_insert(text));
    d.ev.specials.push_back({id, v.is_array()
                                     ? FoldEvent::Special::JsonArray
                                     : FoldEvent::Special::JsonObject});
}

// An empty array or object at a decoded path, for filters.
void note_empty(const dstd::String& path, simdjson::dom::element v,
                LeafDecode& d) {
    if (path.empty() || (d.paths && !position(*d.paths, path))) return;
    const std::uint32_t id = d.intern.get_or_insert(path);
    if (d.schema) d.ev.schema_leaves.emplace_back(id, leaf_type_tag(v));
    d.ev.specials.push_back({id, v.is_array()
                                     ? FoldEvent::Special::EmptyArray
                                     : FoldEvent::Special::EmptyObject});
}

void decode_leaves(dstd::String& path, simdjson::dom::element v,
                   LeafDecode& d) {
    if (d.paths && d.left == 0) return;
    const bool container = !path.empty() && (v.is_object() || v.is_array());
    // A listed path that holds an array or object: a filter reads all of it,
    // and the pass below captures its json text.
    const bool listed = d.paths && container && position(*d.paths, path);
    if (d.json && container && !listed) capture_json(path, v, d);
    if (listed) {
        const auto* paths = std::exchange(d.paths, nullptr);
        const auto* fields = std::exchange(d.path_fields, nullptr);
        decode_leaves(path, v, d);
        d.paths = paths;
        d.path_fields = fields;
        --d.left;
        return;
    }
    simdjson::dom::object obj;
    if (v.get_object().get(obj) == simdjson::SUCCESS) {
        if (obj.size() == 0) return note_empty(path, v, d);
        if (d.paths && !path.empty() && !wanted_below(*d.paths, path)) return;
        if (d.max_children && !path.empty() && obj.size() > d.max_children)
            return collapse(path, v, d);
        for (auto kv : obj) {
            const std::size_t base = path.size();
            if (base) path.push_back('.');
            path.append(kv.key);
            decode_leaves(path, kv.value, d);
            path.resize(base);
            if (d.paths && d.left == 0) return;
        }
        return;
    }
    simdjson::dom::array arr;
    if (v.get_array().get(arr) == simdjson::SUCCESS) {
        if (arr.size() == 0) return note_empty(path, v, d);
        if (d.paths && !path.empty() && !wanted_below(*d.paths, path)) return;
        if (d.max_children && !path.empty() && arr.size() > d.max_children)
            return collapse(path, v, d);
        std::size_t i = 0;
        for (auto el : arr) {
            const std::size_t base = path.size();
            if (base) path.push_back('.');
            dftracer::utils::trace::detail::append_index(path, i++);
            decode_leaves(path, el, d);
            path.resize(base);
            if (d.paths && d.left == 0) return;
        }
        return;
    }
    if (path.empty()) return;
    std::size_t pos = 0;
    if (d.paths) {
        const auto at = position(*d.paths, path);
        if (!at) return;
        pos = *at;
        --d.left;
    }
    const std::uint32_t id = d.intern.get_or_insert(path);
    if (d.schema) d.ev.schema_leaves.emplace_back(id, leaf_type_tag(v));
    if (v.is_null()) {
        d.ev.specials.push_back({id, FoldEvent::Special::NullValue});
        return;
    }
    if (d.skip_never && d.schema && d.record_schema &&
        std::binary_search(d.record_schema->never_index.begin(),
                           d.record_schema->never_index.end(),
                           dstd::StringView(path),
                           [](const auto& a, const auto& b) {
                               return dstd::StringView(a) < dstd::StringView(b);
                           }))
        return;
    if (d.record_schema)
        if (const auto* f = d.field(path, pos, id)) {
            bind_leaf(*f, id, leaf_of(v), d);
            return;
        }
    append_scalar_arg(d.ev.args, d.ev.specials, id, v, d.intern);
}

LeafDecode make_leaf_decode(
    FoldEvent& ev, dftracer::utils::dstd::StringIntern& intern,
    bool capture_schema, const dstd::Vector<dstd::String>* paths,
    const dftracer::utils::index::RecordSchema* record_schema,
    const dstd::Vector<const dftracer::utils::index::FieldSpec*>* path_fields,
    std::size_t max_children, DecodeHints* hints, bool skip_never_indexed,
    dftracer::utils::dstd::InternCache* cache) {
    const auto* fields = paths ? path_fields : nullptr;
    bool json = record_schema && record_schema->has_json;
    if (json && fields)
        json = std::any_of(fields->begin(), fields->end(),
                           [](const index::FieldSpec* f) {
                               return f && f->type == index::FieldType::Json;
                           });
    return LeafDecode{intern,
                      ev,
                      capture_schema,
                      paths,
                      paths ? paths->size() : 0,
                      record_schema,
                      fields,
                      json,
                      max_children,
                      hints,
                      skip_never_indexed,
                      cache};
}

// decode_leaves over a record that is read once, forward.
class PathReader {
   public:
    PathReader(LeafDecode& d, dstd::String& path, const char* end,
               dstd::StringArena& arena)
        : d_(d), ids_(d.ids()), path_(path), end_(end), arena_(arena) {}

    // False on a read error. `path_` holds the path of the container.
    bool members(simdjson::ondemand::object& object) {
        bool any = false;
        for (auto member : object) {
            any = true;
            simdjson::ondemand::field field;
            if (std::move(member).get(field)) return false;
            const auto key = json::detail::read_key(field, arena_);
            if (!key) return false;
            const std::size_t base = path_.size();
            if (base) path_.push_back('.');
            path_.append(*key);
            OnDemandValue child = field.value();
            if (!value(child)) return false;
            path_.resize(base);
            if (done()) return true;
        }
        if (!any) note_empty(false);
        return true;
    }

    bool elements(simdjson::ondemand::array& array) {
        bool any = false;
        std::size_t i = 0;
        for (auto element : array) {
            any = true;
            OnDemandValue child;
            if (element.get(child)) return false;
            const std::size_t base = path_.size();
            if (base) path_.push_back('.');
            dftracer::utils::trace::detail::append_index(path_, i++);
            if (!value(child)) return false;
            path_.resize(base);
            if (done()) return true;
        }
        if (!any) note_empty(true);
        return true;
    }

   private:
    bool done() const { return d_.paths && d_.left == 0; }

    bool value(OnDemandValue& v) {
        if (done()) return true;
        OnDemandType type;
        if (v.type().get(type) || type == OnDemandType::unknown) return false;
        if (type != OnDemandType::object && type != OnDemandType::array)
            return leaf(v, type);
        const bool is_array = type == OnDemandType::array;
        const bool json_field = d_.json && !path_.empty();
        const char* const start =
            json_field || d_.max_children ? v.raw_json_token().data() : nullptr;
        const bool listed =
            d_.paths && !path_.empty() && position(*d_.paths, path_);
        if (json_field && !listed) capture_json(start, is_array);
        if (listed) {
            const auto* paths = std::exchange(d_.paths, nullptr);
            const auto* fields = std::exchange(d_.path_fields, nullptr);
            const bool ok = value(v);
            d_.paths = paths;
            d_.path_fields = fields;
            --d_.left;
            return ok;
        }
        if (d_.paths && !path_.empty() && !wanted_below(*d_.paths, path_))
            return true;
        if (depth_ >= MAX_READ_DEPTH)
            return collapse(v.raw_json_token().data(), is_array, true);
        const bool counted = d_.max_children && !path_.empty();
        std::size_t count = 0;
        if (is_array) {
            simdjson::ondemand::array array;
            if (v.get_array().get(array)) return false;
            if (counted) {
                if (array.count_elements().get(count)) return false;
                if (count == 0) {
                    note_empty(true);
                    return true;
                }
                if (count > d_.max_children) return collapse(start, true);
            }
            ++depth_;
            const bool ok = elements(array);
            --depth_;
            return ok;
        }
        simdjson::ondemand::object object;
        if (v.get_object().get(object)) return false;
        if (counted) {
            if (object.count_fields().get(count)) return false;
            if (count == 0) {
                note_empty(false);
                return true;
            }
            if (count > d_.max_children) return collapse(start, false);
        }
        ++depth_;
        const bool ok = members(object);
        --depth_;
        return ok;
    }

    dstd::String canonical_text(const char* start) const {
        return dftracer::utils::json::canonical_json_text(
            json::detail::value_extent(start, end_));
    }

    // A declared json field at an object or array: its canonical text, keyed
    // by the field's path; the leaves below are decoded as well.
    void capture_json(const char* start, bool is_array) {
        std::size_t pos = 0;
        if (d_.paths) {
            const auto at = position(*d_.paths, path_);
            if (!at) return;
            pos = *at;
        }
        const auto* f = d_.field(path_, pos);
        if (!f || f->type != dftracer::utils::index::FieldType::Json) return;
        if (d_.paths) --d_.left;
        const dstd::String text = canonical_text(start);
        const std::uint32_t id = ids_.get_or_insert(path_);
        d_.ev.args.emplace_back(id, ids_.get_or_insert(text));
        d_.ev.specials.push_back({id, is_array
                                          ? FoldEvent::Special::JsonArray
                                          : FoldEvent::Special::JsonObject});
    }

    // An array or object past max_children, or below the depth limit: one
    // leaf of its canonical text, or of its text as written when `raw`.
    bool collapse(const char* start, bool is_array, bool raw = false) {
        using dftracer::utils::index::store::PathType;
        const std::uint32_t id = ids_.get_or_insert(path_);
        // A declared json field already holds its text.
        if (!d_.ev.args.empty() && d_.ev.args.back().first == id) return true;
        if (d_.schema)
            d_.ev.schema_leaves.emplace_back(
                id, static_cast<std::uint8_t>(is_array ? PathType::Array
                                                       : PathType::Object));
        const dstd::String text =
            raw ? dstd::String(json::detail::value_extent(start, end_))
                : canonical_text(start);
        d_.ev.args.emplace_back(id, ids_.get_or_insert(text));
        d_.ev.specials.push_back({id, is_array
                                          ? FoldEvent::Special::JsonArray
                                          : FoldEvent::Special::JsonObject});
        return true;
    }

    // An empty array or object at a decoded path, for filters.
    void note_empty(bool is_array) {
        using dftracer::utils::index::store::PathType;
        if (path_.empty() || (d_.paths && !position(*d_.paths, path_))) return;
        const std::uint32_t id = ids_.get_or_insert(path_);
        if (d_.schema)
            d_.ev.schema_leaves.emplace_back(
                id, static_cast<std::uint8_t>(is_array ? PathType::Array
                                                       : PathType::Object));
        d_.ev.specials.push_back({id, is_array
                                          ? FoldEvent::Special::EmptyArray
                                          : FoldEvent::Special::EmptyObject});
    }

    bool leaf(OnDemandValue& v, OnDemandType type) {
        if (path_.empty()) return true;
        std::size_t pos = 0;
        if (d_.paths) {
            const auto at = position(*d_.paths, path_);
            if (!at) return true;
            pos = *at;
            --d_.left;
        }
        Leaf scalar;
        if (!read_leaf(v, type, arena_, scalar)) return false;
        const std::uint32_t id = ids_.get_or_insert(path_);
        if (d_.schema) d_.ev.schema_leaves.emplace_back(id, leaf_tag(scalar));
        if (scalar.kind == Leaf::Kind::Null) {
            d_.ev.specials.push_back({id, FoldEvent::Special::NullValue});
            return true;
        }
        if (d_.skip_never && d_.schema && d_.record_schema &&
            std::binary_search(
                d_.record_schema->never_index.begin(),
                d_.record_schema->never_index.end(), dstd::StringView(path_),
                [](const auto& a, const auto& b) {
                    return dstd::StringView(a) < dstd::StringView(b);
                }))
            return true;
        if (d_.record_schema)
            if (const auto* f = d_.field(path_, pos, id)) {
                bind_leaf(*f, id, scalar, d_);
                return true;
            }
        append_leaf_arg(d_.ev.args, d_.ev.specials, id, scalar, ids_);
        return true;
    }

    LeafDecode& d_;
    Ids ids_;
    dstd::String& path_;
    const char* end_;
    dstd::StringArena& arena_;
    std::size_t depth_ = 0;
};

}  // namespace

void decode_record_into(
    FoldEvent& ev, simdjson::dom::element root,
    dftracer::utils::dstd::StringIntern& intern, bool capture_schema,
    const dstd::Vector<dstd::String>* paths,
    const dftracer::utils::index::RecordSchema* record_schema,
    const dstd::Vector<const dftracer::utils::index::FieldSpec*>* path_fields,
    std::size_t max_children, DecodeHints* hints, bool skip_never_indexed) {
    reset_fold_event(ev);
    ev.phase = RecordPhase::Complete;
    ev.by_path = true;
    LeafDecode d = make_leaf_decode(ev, intern, capture_schema, paths,
                                    record_schema, path_fields, max_children,
                                    hints, skip_never_indexed, nullptr);
    if (hints) ev.args.reserve(hints->args);
    dstd::String local_path;
    dstd::String& path = hints ? hints->path : local_path;
    path.clear();
    decode_leaves(path, root, d);
    if (hints) hints->args = ev.args.size();
}

void decode_record_into(
    FoldEvent& ev, json::Value& record,
    dftracer::utils::dstd::StringIntern& intern, bool capture_schema,
    const dstd::Vector<dstd::String>* paths,
    const dftracer::utils::index::RecordSchema* record_schema,
    const dstd::Vector<const dftracer::utils::index::FieldSpec*>* path_fields,
    std::size_t max_children, DecodeHints* hints, bool skip_never_indexed,
    dftracer::utils::dstd::InternCache* cache) {
    reset_fold_event(ev);
    json::detail::ValueWalk walk;
    if (!json::detail::ValueAccess::begin_value(record, walk)) return;
    ev.phase = RecordPhase::Complete;
    ev.by_path = true;
    LeafDecode d = make_leaf_decode(ev, intern, capture_schema, paths,
                                    record_schema, path_fields, max_children,
                                    hints, skip_never_indexed, cache);
    if (hints) ev.args.reserve(hints->args);
    dstd::String local_path;
    dstd::String& path = hints ? hints->path : local_path;
    path.clear();
    PathReader reader(d, path, walk.end, *walk.strings);
    bool ok = true;
    OnDemandType type;
    if (walk.value.type().get(type)) {
        ok = false;
    } else if (type == OnDemandType::object) {
        simdjson::ondemand::object object;
        ok = !walk.value.get_object().get(object) && reader.members(object);
    } else if (type == OnDemandType::array) {
        simdjson::ondemand::array array;
        ok = !walk.value.get_array().get(array) && reader.elements(array);
    }
    if (!ok || !json::detail::ValueAccess::end_object(record)) {
        json::detail::ValueAccess::mark_broken(record);
        reset_fold_event(ev);
        return;
    }
    if (hints) hints->args = ev.args.size();
}

void capture_schema_leaves(FoldEvent& ev, simdjson::dom::element root,
                           dftracer::utils::dstd::StringIntern& intern,
                           bool skip_args, DecodeHints* hints) {
    // The axis and structural keys are not catalog paths.
    simdjson::dom::object obj;
    if (root.get_object().get(obj) != simdjson::SUCCESS) return;
    dstd::String path;
    for (auto kv : obj) {
        const dstd::StringView k = kv.key;
        if (is_axis_key(k, skip_args)) continue;
        const auto type = kv.value.type();
        if (hints && !k.empty() &&
            type != simdjson::dom::element_type::OBJECT &&
            type != simdjson::dom::element_type::ARRAY) {
            ev.schema_leaves.emplace_back(top_leaf_id(*hints, intern, k),
                                          leaf_type_tag(kv.value));
            continue;
        }
        path.assign(k);
        enumerate_leaves(path, kv.value, intern, ev);
    }
}

namespace {

template <std::size_t N>
bool key_is(dstd::StringView key, const char (&literal)[N]) {
    return key.size() == N - 1 && std::memcmp(key.data(), literal, N - 1) == 0;
}

bool read_uint(simdjson::dom::element v, std::uint64_t& out) {
    std::uint64_t n = 0;
    if (v.get_uint64().get(n) != simdjson::SUCCESS) return false;
    out = n;
    return true;
}

bool read_text(simdjson::dom::element v, dstd::StringView& out) {
    dstd::StringView text;
    if (v.get_string().get(text) != simdjson::SUCCESS) return false;
    out = text;
    return true;
}

}  // namespace

bool counts_as_event(simdjson::dom::element root) {
    simdjson::dom::object top;
    if (root.get_object().get(top) != simdjson::SUCCESS) return false;
    RecordPhase phase = RecordPhase::Unknown;
    bool has_ph = false;
    for (auto field : top) {
        if (key_is(field.key, "ph")) {
            phase = trace::read_phase(field.value);
            has_ph = true;
        }
    }
    return has_ph && phase != RecordPhase::Metadata;
}

bool counts_as_event(json::Value& record) {
    json::detail::ObjectWalk walk;
    if (!json::detail::ValueAccess::begin_object(record, walk)) return false;
    RecordPhase phase = RecordPhase::Unknown;
    bool has_ph = false;
    bool ok = true;
    for (auto member : walk.object) {
        simdjson::ondemand::field field;
        if (std::move(member).get(field)) {
            ok = false;
            break;
        }
        const auto key = json::detail::read_key(field, *walk.strings);
        if (!key) {
            ok = false;
            break;
        }
        if (key->size() == 2 && key_is(*key, "ph")) {
            OnDemandValue v = field.value();
            if (!read_phase_value(v, *walk.strings, phase)) {
                ok = false;
                break;
            }
            has_ph = true;
        }
    }
    if (!ok || !json::detail::ValueAccess::end_object(record)) {
        json::detail::ValueAccess::mark_broken(record);
        return false;
    }
    return has_ph && phase != RecordPhase::Metadata;
}

bool append_dftracer_record(FoldEventBuffer& batch, simdjson::dom::element root,
                            dftracer::utils::dstd::StringIntern& intern,
                            bool needs_args, bool capture_schema,
                            DecodeHints& hints,
                            dftracer::utils::dstd::InternCache* cache) {
    simdjson::dom::object top;
    if (root.get_object().get(top) != simdjson::SUCCESS) return false;
    trace::internal::parse_walks::record_walk();

    RecordPhase phase = RecordPhase::Unknown;
    std::uint64_t pid = 0, tid = 0, ts = 0, dur = 0;
    dstd::StringView cat, name;
    bool has_ph = false;
    bool has_dur = false;
    bool has_args = false;
    simdjson::dom::object args;
    hints.walked.clear();

    // The axis keys are not catalog paths. Every other key is kept, in order,
    // for the leaf pass below.
    for (auto field : top) {
        const dstd::StringView key = field.key;
        const simdjson::dom::element val = field.value;
        bool leaf = capture_schema;
        switch (key.size()) {
            case 2:
                if (key_is(key, "ph")) {
                    phase = trace::read_phase(val);
                    has_ph = true;
                    leaf = false;
                } else if (key_is(key, "ts")) {
                    read_uint(val, ts);
                    leaf = false;
                } else if (key_is(key, "id")) {
                    leaf = false;
                }
                break;
            case 3:
                if (key_is(key, "pid")) {
                    read_uint(val, pid);
                    leaf = false;
                } else if (key_is(key, "tid")) {
                    read_uint(val, tid);
                    leaf = false;
                } else if (key_is(key, "dur")) {
                    if (read_uint(val, dur)) has_dur = true;
                    leaf = false;
                } else if (key_is(key, "cat")) {
                    read_text(val, cat);
                }
                break;
            case 4:
                if (key_is(key, "name")) {
                    read_text(val, name);
                } else if (key_is(key, "args")) {
                    if (val.get_object().get(args) == simdjson::SUCCESS)
                        has_args = true;
                }
                break;
            default:
                break;
        }
        if (leaf) hints.walked.push_back({key, val});
    }
    if (!has_ph) return false;

    FoldEvent& ev = batch.append();
    Ids ids{intern, cache};
    reset_fold_event(ev);
    ev.phase = phase;
    ev.pid = pid;
    ev.tid = tid;
    ev.ts = ts;
    ev.dur = dur;
    ev.has_dur = has_dur;
    if (!cat.empty()) ev.cat_id = ids.get_or_insert(cat);
    if (!name.empty()) ev.name_id = ids.get_or_insert(name);

    // A non-empty args object is walked once, for the event and its leaves.
    const bool args_walked = has_args && args.size() > 0;
    if (has_args) {
        trace::internal::parse_walks::args_walk();
        DecodeHints* const leaves =
            needs_args && args_walked && capture_schema ? &hints : nullptr;
        const auto key_id = [&](dstd::StringView key) {
            return ids.get_or_insert(key);
        };
        std::size_t pos = 0;
        for (auto field : args) {
            const dstd::StringView key = field.key;
            const simdjson::dom::element v = field.value;
            const std::size_t at = pos++;
            if (key_is(key, "fhash") || key_is(key, "hhash")) {
                const std::uint32_t id = intern_string(ids, v);
                (key[0] == 'f' ? ev.fhash_id : ev.hhash_id) = id;
                if (leaves)
                    ev.schema_leaves.emplace_back(
                        arg_leaf_id(*leaves, intern,
                                    key_at(hints.arg_key_at, at, key, key_id),
                                    key),
                        leaf_type_tag(v));
            } else if (needs_args) {
                switch (v.type()) {
                    case simdjson::dom::element_type::OBJECT:
                    case simdjson::dom::element_type::ARRAY: {
                        dstd::String nested(key);
                        flatten_nested_arg(nested, v, intern, ev, leaves);
                        break;
                    }
                    default:
                        append_arg_leaf(
                            ev, key_at(hints.arg_key_at, at, key, key_id), key,
                            v, ids, leaves);
                }
            }
        }
    }

    if (!capture_schema) return true;
    // Like capture_schema_leaves with skip_args. Every args key is skipped
    // when the args walk emitted the leaves, else args is a leaf in place.
    const bool args_in_walk = needs_args && args_walked;
    std::size_t at = 0;
    for (const auto& [key, val] : hints.walked) {
        const std::size_t pos = at++;
        if (args_in_walk && key_is(key, "args")) continue;
        const auto type = val.type();
        if (!key.empty() && type != simdjson::dom::element_type::OBJECT &&
            type != simdjson::dom::element_type::ARRAY) {
            ev.schema_leaves.emplace_back(key_at(hints.top_key_at, pos, key,
                                                 [&](dstd::StringView k) {
                                                     return top_leaf_id(
                                                         hints, intern, k);
                                                 }),
                                          leaf_type_tag(val));
            continue;
        }
        hints.path.assign(key);
        enumerate_leaves(hints.path, val, intern, ev);
    }
    return true;
}

dstd::Vector<dstd::String> extra_capture_fields(const ViewPlan& plan) {
    // Args come from needs_args, nested ones flattened; only top-level fields
    // the POD does not carry (anything but the six scalars) and nested paths
    // need capture, since a fold may not set needs_args.
    auto is_pod_scalar = [](const dstd::String& f) {
        return f == "name" || f == "cat" || f == "pid" || f == "tid" ||
               f == "ts" || f == "dur";
    };
    dstd::Vector<dstd::String> out;
    auto add = [&](const dstd::String& f) {
        if (f.empty()) return;
        for (const auto& e : out)
            if (e == f) return;
        out.push_back(f);
    };
    for (const auto& gk : plan.group_by)
        if (gk.kind == GroupKey::Kind::Field && !is_pod_scalar(gk.arg))
            add(gk.arg);
    for (const auto& c : plan.computed)
        for (const auto& in : c.inputs)
            if (is_nested_path(in)) add(in);
    for (const auto& spec : plan.agg) {
        if (is_nested_path(spec.field)) add(spec.field);
        if (spec.op == AggOp::ArgMax && is_nested_path(spec.by)) add(spec.by);
    }
    return out;
}

namespace {

// Shared, worker-agnostic state for one fuse() run. Reference members point at
// fuse()'s locals, which outlive every worker (with_scope joins before
// fuse returns). Passed by value into fuse_worker so the coroutine frame owns
// its copy of the (trivially copyable) handles.
struct FuseWorkerCtx {
    dstd::Vector<dstd::Vector<dstd::UniquePtr<Fold>>>& fslice;
    const dstd::Vector<ScanUnit>& units;
    std::atomic<std::size_t>& next_unit;
    std::atomic<std::uint64_t>& produced;
    std::uint64_t cap;
    const ViewPlan& plan;
    const ViewDefinition& vdef;
    dftracer::utils::dstd::StringIntern& intern;
    const CoverageSet* covered;
    coro::CoroSemaphore& budget_sem;
    dstd::Vector<ScanCounts>& counts_v;
    dstd::Vector<CoverageSet>& covered_v;
    const dstd::Vector<dstd::String>& extra_fields;
    bool any_needs_args;
    bool any_wants_raw;
    bool any_wants_fold_event;
    bool any_wants_schema;
    bool skip_never_indexed;
    DynamicPrune* dyn_prune;
    coro::CoroSemaphore* gate;
};

// Runs `hook` on every slice, awaiting the task each one leaves.
template <class Hook>
coro::CoroTask<void> for_slices(dstd::Vector<dstd::UniquePtr<Fold>>& slices,
                                Hook hook) {
    for (auto& s : slices) {
        hook(*s);
        if (auto* t = s->take_pending())
            co_await *reinterpret_cast<coro::CoroTask<void>*>(t);
    }
}

// One fuse worker: drains the shared unit queue, decodes each unit, and pushes
// its batches into worker w's fold slices. Named (not a capturing-lambda
// coroutine) to avoid the GCC 12/13 coroutine-frame miscompile on non-trivial
// frame locals (the scanner/generator/batch below). Behavior is identical to
// the previous inline lambda.
coro::CoroTask<void> fuse_worker(FuseWorkerCtx ctx, std::size_t w) {
    auto& slices = ctx.fslice[w];
    FoldPortBus bus;
    FoldEventBuffer fold_buffer;
    dftracer::utils::dstd::InternCache fold_cache(ctx.intern);
    for (auto& s : slices) s->bind_port_bus(&bus);
    PendingCoverage pending(ctx.covered_v[w]);

    for (;;) {
        if (is_cancelled(ctx.plan)) break;
        if (ctx.produced.load(std::memory_order_relaxed) >= ctx.cap) break;
        if (ctx.gate) co_await ctx.gate->acquire(1);
        std::size_t i = ctx.next_unit.fetch_add(1, std::memory_order_relaxed);
        if (i >= ctx.units.size()) {
            if (ctx.gate) ctx.gate->release(1);
            break;
        }
        const ScanUnit& unit = ctx.units[i];
        // A materialized aggregate already answered this chunk, or a narrow()
        // call mid-scan ruled it out after gather_units listed it.
        const bool covered =
            ctx.covered &&
            ctx.covered->covers(unit.file_path, unit.checkpoint_idx);
        const bool pruned =
            !covered && ctx.dyn_prune &&
            ctx.dyn_prune->is_excluded(unit.file_path, unit.checkpoint_idx);
        if (pruned) ctx.dyn_prune->record_skip();
        if (covered || pruned) {
            co_await for_slices(slices, [&](Fold& s) { s.skip_unit(unit); });
            continue;
        }

        const std::uint64_t reserve =
            ctx.units[i].end_byte > ctx.units[i].start_byte
                ? ctx.units[i].end_byte - ctx.units[i].start_byte
                : 0;
        co_await ctx.budget_sem.acquire(reserve);
        ScanPermit permit{ctx.budget_sem, reserve};
        // A narrow() may land while this unit waits for the budget, and with
        // many workers every unit is claimed before the first one runs.
        if (ctx.dyn_prune &&
            ctx.dyn_prune->is_excluded(unit.file_path, unit.checkpoint_idx)) {
            ctx.dyn_prune->record_skip();
            co_await for_slices(slices, [&](Fold& s) { s.skip_unit(unit); });
            continue;
        }

        ViewScannerInput sin =
            make_scanner_input(ctx.units[i], ctx.vdef, ctx.vdef.query);
        // A FoldEvent fold gets the parsed stream; a raw fold parses the lines
        // itself. When both are present, keep both.
        sin.fold_intern = ctx.any_wants_fold_event ? &ctx.intern : nullptr;
        sin.fold_buffer = &fold_buffer;
        sin.fold_cache = &fold_cache;
        sin.fold_needs_args = ctx.any_needs_args;
        sin.fold_keep_raw = ctx.any_wants_raw && ctx.any_wants_fold_event;
        sin.fold_capture_schema = ctx.any_wants_schema;
        sin.fold_skip_never_indexed = ctx.skip_never_indexed;
        if (!ctx.extra_fields.empty())
            sin.fold_extra_fields = &ctx.extra_fields;
        ViewScannerUtility scanner;
        auto gen = scanner(sin);
        bool complete = true;
        for (;;) {
            auto b = co_await gen.next();
            if (!b) break;
            if (is_cancelled(ctx.plan)) {
                complete = false;
                break;
            }
            if (ctx.produced.load(std::memory_order_relaxed) >= ctx.cap) {
                complete = false;
                break;
            }
            ctx.counts_v[w].add(*b);
            if (b->fold_events.empty() && b->events.empty()) continue;
            ctx.produced.fetch_add(b->fold_events.size() + b->events.size(),
                                   std::memory_order_relaxed);
            FoldBatch fb{dstd::Span<const FoldEvent>(b->fold_events),
                         ctx.units[i],
                         dstd::Span<const dstd::StringView>(b->events)};
            bus.clear();
            for (auto& s : slices) {
                s->step(fb);
                // Await an async plugin fold's task before the next step
                // recycles the batch; null for sync folds.
                if (auto* t = s->take_pending())
                    co_await *reinterpret_cast<coro::CoroTask<void>*>(t);
            }
        }
        if (!complete) {
            co_await for_slices(slices, [&](Fold& s) { s.drop_unit(unit); });
            break;
        }
        co_await for_slices(slices, [&](Fold& s) { s.seal_unit(unit); });
        pending.mark_complete(ctx.units[i].file_path,
                              ctx.units[i].checkpoint_idx);
        pending.seal();
    }
    co_return;
}

}  // namespace

coro::CoroTask<ExportStats> fuse(const ViewPlan& plan,
                                 const ViewDefinition& vdef_in,
                                 dstd::Span<Fold* const> folds,
                                 dftracer::utils::dstd::StringIntern& intern,
                                 const CoverageSet* covered,
                                 std::uint64_t limit, DynamicPrune* dyn_prune,
                                 coro::CoroSemaphore* gate) {
    // Folds that all drop metadata records need no chunk read for them.
    ViewDefinition vdef = vdef_in;
    if (std::none_of(folds.begin(), folds.end(),
                     [](const Fold* f) { return f->wants_metadata(); })) {
        vdef.include_metadata = false;
    }
    if (vdef.by_path &&
        std::any_of(folds.begin(), folds.end(),
                    [](const Fold* f) { return f->reads_every_field(); })) {
        vdef.paths.clear();
        vdef.path_fields.clear();
    }
    std::uint64_t skipped = 0;
    auto units = co_await gather_units(plan, vdef, skipped);
    for (std::size_t i = 0; i < units.size(); ++i) units[i].seq = i;
    const std::uint64_t cap =
        limit > 0 ? limit : std::numeric_limits<std::uint64_t>::max();
    std::atomic<std::uint64_t> produced{0};

    ExportStats st;
    st.chunks_scanned = units.size();
    if (units.empty() || folds.empty()) {
        st.chunks_skipped = skipped;
        const CoverageSet none;
        for (auto* f : folds) co_await f->finalize(none);
        co_return counted(plan, st);
    }

    bool any_needs_args = false;
    bool any_wants_raw = false;
    bool any_wants_fold_event = false;
    bool any_wants_schema = false;
    bool skip_never_indexed = true;
    for (auto* f : folds) {
        any_needs_args |= f->needs_args();
        any_wants_schema |= f->wants_schema();
        skip_never_indexed &= !f->reads_never_indexed();
        if (f->wants_raw())
            any_wants_raw = true;
        else
            any_wants_fold_event = true;
    }

    dstd::Vector<dstd::String> extra_fields = extra_capture_fields(plan);
    // Fold-declared captures (nested fields a fold resolves itself) merged in.
    for (auto* f : folds)
        for (const dstd::String& c : f->extra_captures()) {
            bool seen = false;
            for (const dstd::String& e : extra_fields)
                if (e == c) {
                    seen = true;
                    break;
                }
            if (!seen) extra_fields.push_back(c);
        }

    // Coarse fan-out: one worker coroutine per runtime slot draining the shared
    // unit queue, so under the elastic runtime live threads grow toward the
    // cap.
    const std::size_t nworkers =
        std::min<std::size_t>(units.size(), common::available_parallelism());
    // Bound bytes decoded concurrently so a scan cannot OOM the box. Always on:
    // an explicit memory_budget() or a RAM-fraction default. Workers park when
    // full, so a generous budget never throttles.
    coro::CoroSemaphore budget_sem(
        common::compute_memory_budget(plan.memory_budget));
    std::atomic<std::size_t> next_unit{0};
    dstd::Vector<ScanCounts> counts_v(nworkers);
    dstd::Vector<CoverageSet> covered_v(nworkers);
    // Per-worker slice of every fold, owned here so a slice outlives the
    // coroutine that filled it; merged into the shared folds after the join.
    dstd::Vector<dstd::Vector<dstd::UniquePtr<Fold>>> fslice(nworkers);
    for (std::size_t w = 0; w < nworkers; ++w)
        for (auto* f : folds) fslice[w].push_back(f->slice());

    FuseWorkerCtx ctx{fslice,
                      units,
                      next_unit,
                      produced,
                      cap,
                      plan,
                      vdef,
                      intern,
                      covered,
                      budget_sem,
                      counts_v,
                      covered_v,
                      extra_fields,
                      any_needs_args,
                      any_wants_raw,
                      any_wants_fold_event,
                      any_wants_schema,
                      skip_never_indexed,
                      dyn_prune,
                      gate};
    co_await coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        for (std::size_t w = 0; w < nworkers; ++w)
            scope.spawn([&ctx, w]() -> coro::CoroTask<void> {
                return fuse_worker(ctx, w);
            });
        co_return;
    });

    for (std::size_t w = 0; w < nworkers; ++w)
        for (std::size_t k = 0; k < folds.size(); ++k)
            folds[k]->merge(*fslice[w][k]);

    const std::uint64_t dyn_skipped = dyn_prune ? dyn_prune->skipped() : 0;
    st.chunks_skipped = skipped + dyn_skipped;

    CoverageSet scanned;
    for (auto& c : covered_v) scanned.absorb(std::move(c));

    // A file is whole only if nothing was pruned (statically or by a
    // narrow() call mid-scan) and every unit of it sealed.
    if (skipped == 0 && dyn_skipped == 0) {
        dstd::HashMap<dstd::StringView, dstd::Pair<std::size_t, std::size_t>>
            per_file;
        for (const auto& u : units) {
            auto& c = per_file[u.file_path];
            ++c.first;
            if (scanned.covers(u.file_path, u.checkpoint_idx)) ++c.second;
        }
        for (const auto& [file, c] : per_file)
            if (c.first == c.second) scanned.add_file(file);
    }

    for (auto* f : folds) co_await f->finalize(scanned);

    for (const auto& c : counts_v) c.add_to(st);
    st.truncated = produced.load(std::memory_order_relaxed) >= cap;
    co_return counted(plan, st);
}

}  // namespace dftracer::utils::trace::views::detail
