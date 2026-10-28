#ifndef DFTRACER_UTILS_TRACE_VIEWS_FOLD_EVENT_H
#define DFTRACER_UTILS_TRACE_VIEWS_FOLD_EVENT_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/common/field_ref.h>
#include <dftracer/utils/dstd/intern_cache.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/trace/event.h>
#include <simdjson.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// The owned event the fold-fusion scan core batches, kept in its own header so
// both the Fold interface and the event accessor depend on the data, not on
// each other.
namespace dftracer::utils::trace::views::detail {

/// Fixed-size chunks that the lists of many events take their storage from, so
/// a batch holds its entries in a few blocks rather than one block per event.
/// A block it hands out never moves; reset() hands every block out again.
template <class T>
class ChunkArena {
    static_assert(std::is_trivially_destructible_v<T>);

   public:
    /// The first chunk holds MIN_CHUNK_ENTRIES and each new one twice the one
    /// before, up to CHUNK_ENTRIES, so a buffer of a few events stays small.
    static constexpr std::size_t MIN_CHUNK_ENTRIES = 64;
    static constexpr std::size_t CHUNK_ENTRIES = 4096;
    /// Chunks reset() keeps for the next batch; the rest are freed.
    static constexpr std::size_t RETAIN_CHUNKS = 16;

    T* allocate(std::size_t n) {
        while (current_ < chunks_.size()) {
            Chunk& chunk = chunks_[current_];
            if (chunk.capacity - used_ >= n) {
                T* out = chunk.data.get() + used_;
                used_ += n;
                return out;
            }
            ++current_;
            used_ = 0;
        }
        const std::size_t step =
            chunks_.empty()
                ? MIN_CHUNK_ENTRIES
                : std::min(CHUNK_ENTRIES, chunks_.back().capacity * 2);
        const std::size_t capacity = std::max(n, step);
        chunks_.push_back(Chunk{std::make_unique<T[]>(capacity), capacity});
        current_ = chunks_.size() - 1;
        used_ = n;
        return chunks_.back().data.get();
    }
    /// Grows the block `allocate` handed out last from `capacity` to `wanted`
    /// entries in place; false when it is not the last block or the chunk has
    /// no room, and nothing changes.
    bool extend(const T* block, std::size_t capacity, std::size_t wanted) {
        if (current_ >= chunks_.size()) return false;
        Chunk& chunk = chunks_[current_];
        if (block + capacity != chunk.data.get() + used_) return false;
        if (chunk.capacity - used_ < wanted - capacity) return false;
        used_ += wanted - capacity;
        return true;
    }
    void reset() {
        if (chunks_.size() > RETAIN_CHUNKS) chunks_.resize(RETAIN_CHUNKS);
        current_ = 0;
        used_ = 0;
    }
    std::size_t bytes() const {
        std::size_t total = 0;
        for (const Chunk& chunk : chunks_) total += chunk.capacity * sizeof(T);
        return total;
    }

   private:
    struct Chunk {
        std::unique_ptr<T[]> data;
        std::size_t capacity = 0;
    };
    std::vector<Chunk> chunks_;
    std::size_t current_ = 0;
    std::size_t used_ = 0;
};

/// A vector of trivially copyable entries that takes its storage from a
/// ChunkArena while bound to one, and from the heap otherwise. A copy owns its
/// entries on the heap, so an event copied out of a batch outlives the batch;
/// a bound list is valid until its arena resets.
template <class T>
class ArenaVec {
    static_assert(std::is_trivially_destructible_v<T>);

   public:
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;

    ArenaVec() = default;
    ArenaVec(const ArenaVec& other) { assign(other.data_, other.size_); }
    ArenaVec(ArenaVec&& other) noexcept
        : data_(other.data_),
          size_(other.size_),
          capacity_(other.capacity_),
          arena_(other.arena_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
        other.arena_ = nullptr;
    }
    ArenaVec& operator=(const ArenaVec& other) {
        if (this != &other) {
            size_ = 0;
            assign(other.data_, other.size_);
        }
        return *this;
    }
    ArenaVec& operator=(ArenaVec&& other) noexcept {
        if (this != &other) {
            free_owned();
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            arena_ = other.arena_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
            other.arena_ = nullptr;
        }
        return *this;
    }
    ~ArenaVec() { free_owned(); }

    /// Takes storage from `arena` from here on. Drops what the list held, so
    /// a slot whose arena reset starts empty.
    void bind(ChunkArena<T>* arena) {
        free_owned();
        data_ = nullptr;
        size_ = 0;
        capacity_ = 0;
        arena_ = arena;
    }

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    std::size_t capacity() const { return capacity_; }
    T* data() { return data_; }
    const T* data() const { return data_; }
    T* begin() { return data_; }
    T* end() { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end() const { return data_ + size_; }
    T& operator[](std::size_t i) { return data_[i]; }
    const T& operator[](std::size_t i) const { return data_[i]; }
    T& front() { return data_[0]; }
    const T& front() const { return data_[0]; }
    T& back() { return data_[size_ - 1]; }
    const T& back() const { return data_[size_ - 1]; }

    void clear() { size_ = 0; }
    void pop_back() { --size_; }
    void reserve(std::size_t n) {
        if (n > capacity_) grow(n);
    }
    void resize(std::size_t n) {
        reserve(n);
        for (std::size_t i = size_; i < n; ++i) data_[i] = T{};
        size_ = static_cast<std::uint32_t>(n);
    }
    void push_back(const T& value) {
        if (size_ == capacity_) grow(size_ + 1);
        data_[size_++] = value;
    }
    template <class... Args>
    T& emplace_back(Args&&... args) {
        if (size_ == capacity_) grow(size_ + 1);
        data_[size_] = T{std::forward<Args>(args)...};
        return data_[size_++];
    }
    T* erase(T* at) {
        std::copy(at + 1, end(), at);
        --size_;
        return at;
    }

    template <class It>
    void assign(It first, It last) {
        size_ = 0;
        reserve(static_cast<std::size_t>(std::distance(first, last)));
        for (; first != last; ++first) data_[size_++] = *first;
    }

    friend bool operator==(const ArenaVec& a, const ArenaVec& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end());
    }

   private:
    void grow(std::size_t need) {
        const std::size_t capacity =
            std::max<std::size_t>({need, std::size_t{capacity_} * 2, 4});
        // The list a decoder fills is the last block of its arena, so it grows
        // where it is.
        if (arena_ != nullptr && data_ != nullptr &&
            arena_->extend(data_, capacity_, capacity)) {
            capacity_ = static_cast<std::uint32_t>(capacity);
            return;
        }
        T* fresh =
            arena_ != nullptr ? arena_->allocate(capacity) : new T[capacity];
        std::copy(data_, data_ + size_, fresh);
        free_owned();
        data_ = fresh;
        capacity_ = static_cast<std::uint32_t>(capacity);
    }
    void assign(const T* from, std::size_t n) {
        reserve(n);
        std::copy(from, from + n, data_);
        size_ = static_cast<std::uint32_t>(n);
    }
    void free_owned() {
        if (arena_ == nullptr) delete[] data_;
    }

    T* data_ = nullptr;
    std::uint32_t size_ = 0;
    std::uint32_t capacity_ = 0;
    ChunkArena<T>* arena_ = nullptr;
};

/// An event owned independently of the simdjson parse that produced it: strings
/// are interned to ids, never views into the reused parser buffer, so a whole
/// batch stays valid at once. `args` is filled only for folds that need it,
/// with nested object and array args flattened to dotted keys (`a.b`, `a.0`).
struct FoldEvent {
    std::uint32_t cat_id = 0xFFFFFFFF;
    std::uint32_t name_id = 0xFFFFFFFF;
    std::uint32_t fhash_id = 0xFFFFFFFF;
    std::uint32_t hhash_id = 0xFFFFFFFF;
    std::uint64_t pid = 0;
    std::uint64_t tid = 0;
    std::uint64_t ts = 0;
    std::uint64_t dur = 0;
    RecordPhase phase = RecordPhase::Unknown;
    bool has_dur = false;
    /// A record decoded by exact path (decode_record): every field is in
    /// `args` under its path, and the fixed fields above are unset.
    bool by_path = false;

    /// A metric (double for reals, int64 for exact integers so values above
    /// 2^53 survive, uint64 only above int64's range) or an interned string
    /// id (group dimensions), keyed by the interned arg-name id.
    /// Declared-field values that did not convert and read as null.
    std::uint16_t unconverted = 0;
    using ArgValue =
        std::variant<double, std::int64_t, std::uint32_t, std::uint64_t>;
    using Arg = std::pair<std::uint32_t, ArgValue>;
    ArenaVec<Arg> args;
    /// Top-level values for schema fields the POD does not carry as a scalar
    /// (type/ph/id), kept apart from `args` so a same-named args key cannot
    /// shadow the real top-level field. Same encoding as `args`.
    ArenaVec<Arg> top_fields;

    /// What `args` and `top_fields` leave out or widen, for filters: a JSON
    /// null, a bool (also stored there as 0/1), an empty array or object, or
    /// an array or object whose `args` entry is its canonical JSON text (a
    /// declared json field).
    enum class Special : std::uint8_t {
        NullValue,
        FalseValue,
        TrueValue,
        EmptyArray,
        EmptyObject,
        JsonArray,
        JsonObject,
    };
    struct SpecialValue {
        std::uint32_t key;
        Special kind;
        /// Keyed like `top_fields` rather than `args`.
        bool top = false;
    };
    ArenaVec<SpecialValue> specials;

    /// Every scalar or null leaf of the record, arbitrarily nested, as
    /// (interned exact path id -> index::store::PathType): "args.io.off",
    /// "args.hosts.1", a top-level "type". Filled only when the scan captures
    /// schema (the index build); empty otherwise.
    using SchemaLeaf = std::pair<std::uint32_t, std::uint8_t>;
    ArenaVec<SchemaLeaf> schema_leaves;
};

/// Rewrite every interned id of `ev` through `map`, indexed by the old id.
inline void remap_ids(FoldEvent& ev, std::span<const std::uint32_t> map) {
    constexpr std::uint32_t NO_ID = dftracer::utils::dstd::StringIntern::NO_ID;
    const auto at = [&](std::uint32_t& id) {
        if (id != NO_ID) id = map[id];
    };
    at(ev.cat_id);
    at(ev.name_id);
    at(ev.fhash_id);
    at(ev.hhash_id);
    for (auto* list : {&ev.args, &ev.top_fields})
        for (auto& [key, value] : *list) {
            key = map[key];
            if (auto* text = std::get_if<std::uint32_t>(&value))
                *text = map[*text];
        }
    for (auto& s : ev.specials) s.key = map[s.key];
    for (auto& leaf : ev.schema_leaves) leaf.first = map[leaf.first];
}

/// Reset `ev` to a default event, keeping the capacity of its vectors so a
/// reused slot decodes the next record without allocating.
inline void reset_fold_event(FoldEvent& ev) {
    auto args = std::move(ev.args);
    auto top_fields = std::move(ev.top_fields);
    auto specials = std::move(ev.specials);
    auto schema_leaves = std::move(ev.schema_leaves);
    ev = FoldEvent{};
    args.clear();
    top_fields.clear();
    specials.clear();
    schema_leaves.clear();
    ev.args = std::move(args);
    ev.top_fields = std::move(top_fields);
    ev.specials = std::move(specials);
    ev.schema_leaves = std::move(schema_leaves);
}

/// FoldEvents a decoding worker reuses from batch to batch. The lists of every
/// event take their entries from arenas of the buffer, so a batch holds its
/// entries in a few chunks and steady-state decode allocates nothing per
/// event. clear() hands the chunks out again; a batch of more than
/// RETAIN_SLOTS events is cut back to that many slots.
class FoldEventBuffer {
   public:
    static constexpr std::size_t RETAIN_SLOTS = 16384;

    FoldEventBuffer() = default;
    FoldEventBuffer(const FoldEventBuffer&) = delete;
    FoldEventBuffer& operator=(const FoldEventBuffer&) = delete;

    /// The next slot, in whatever state its last use left it; the decoder
    /// resets it.
    FoldEvent& append() {
        if (live_ == slots_.size()) {
            FoldEvent& ev = slots_.emplace_back();
            bind(ev);
        }
        return slots_[live_++];
    }
    /// Drop the slot append() returned last (a record the caller rejects).
    void discard_last() { --live_; }
    void clear() {
        live_ = 0;
        if (slots_.size() > RETAIN_SLOTS) {
            slots_.resize(RETAIN_SLOTS);
            slots_.shrink_to_fit();
        }
        for (FoldEvent& ev : slots_) bind(ev);
        args_.reset();
        specials_.reset();
        leaves_.reset();
    }
    bool empty() const { return live_ == 0; }
    std::size_t size() const { return live_; }
    /// Slots allocated, live or idle.
    std::size_t slot_capacity() const { return slots_.capacity(); }
    /// Bytes the buffer holds: its slots and the arenas of their lists.
    std::size_t bytes() const {
        return slots_.capacity() * sizeof(FoldEvent) + args_.bytes() +
               specials_.bytes() + leaves_.bytes();
    }
    const FoldEvent& back() const { return slots_[live_ - 1]; }
    std::span<const FoldEvent> events() const { return {slots_.data(), live_}; }
    std::span<FoldEvent> events_mut() { return {slots_.data(), live_}; }

   private:
    void bind(FoldEvent& ev) {
        ev.args.bind(&args_);
        ev.top_fields.bind(&args_);
        ev.specials.bind(&specials_);
        ev.schema_leaves.bind(&leaves_);
    }

    std::vector<FoldEvent> slots_;
    std::size_t live_ = 0;
    ChunkArena<FoldEvent::Arg> args_;
    ChunkArena<FoldEvent::SpecialValue> specials_;
    ChunkArena<FoldEvent::SchemaLeaf> leaves_;
};

/// A read-only sequence of FoldEvents: all of a span, or the ones a list of
/// indices picks, so a filter can hand the column builders its kept rows
/// without copying a FoldEvent. Borrows both; they must outlive the view.
class EventRows {
   public:
    EventRows(std::span<const FoldEvent> all)
        : base_(all.data()), n_(all.size()) {}
    EventRows(const std::vector<FoldEvent>& all)
        : base_(all.data()), n_(all.size()) {}
    EventRows(std::span<const FoldEvent> all,
              std::span<const std::uint32_t> pick)
        : base_(all.data()), pick_(pick.data()), n_(pick.size()) {}

    std::size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    const FoldEvent& operator[](std::size_t i) const {
        return pick_ ? base_[pick_[i]] : base_[i];
    }

    class iterator {
       public:
        iterator(const EventRows* rows, std::size_t at)
            : rows_(rows), at_(at) {}
        const FoldEvent& operator*() const { return (*rows_)[at_]; }
        iterator& operator++() {
            ++at_;
            return *this;
        }
        bool operator!=(const iterator& o) const { return at_ != o.at_; }

       private:
        const EventRows* rows_;
        std::size_t at_;
    };
    iterator begin() const { return {this, 0}; }
    iterator end() const { return {this, n_}; }

   private:
    const FoldEvent* base_;
    const std::uint32_t* pick_ = nullptr;
    std::size_t n_;
};

/// The fixed top-level fields of the trace schema. A bare reference to one of
/// these resolves to the top-level value, never a same-named args key; reach an
/// args field with the same name through the explicit `args.<name>` path.
inline bool is_schema_field(std::string_view f) {
    return f == "name" || f == "cat" || f == "pid" || f == "tid" || f == "ts" ||
           f == "dur" || f == "ph" || f == "id" || f == "type";
}

/// True when `field` addresses a nested value (`a.b`, `a[0]`, `a.0.b`), so it
/// needs path resolution rather than a single object-key lookup.
inline bool is_nested_path(std::string_view field) {
    return field.find_first_of(".[") != std::string_view::npos;
}

/// Resolve a dotted/bracketed path from `root` (`a.b[0].c`, `a.b.0`); `ok` is
/// false if any segment is missing. Rooted at `root` with no args fallback
/// (capture_extra_field adds it). Delegates to the
/// single path walker (JsonValue::at) so flat dotted member keys resolve here
/// too; on failure `ok` is false and `root` is returned unchanged.
inline simdjson::dom::element resolve_json_path(simdjson::dom::element root,
                                                std::string_view path,
                                                bool& ok) {
    json::JsonValue v = json::JsonValue(root).at(path);
    ok = v.exists();
    return ok ? v.element() : root;
}

/// Append `v` under `key_id` when it is a scalar: a string interned, an integer
/// exact as int64 (a uint64 above INT64_MAX as double), a bool as 0/1. A null,
/// a bool and an empty array or object also go to `specials`; a non-empty
/// array or object appends nothing.
template <class Intern>
inline void append_scalar_arg(ArenaVec<FoldEvent::Arg>& into,
                              ArenaVec<FoldEvent::SpecialValue>& specials,
                              std::uint32_t key_id, simdjson::dom::element v,
                              Intern& intern, bool top = false) {
    using T = simdjson::dom::element_type;
    using S = FoldEvent::Special;
    switch (v.type()) {
        case T::STRING:
            into.emplace_back(
                key_id, intern.get_or_insert(v.get_string().value_unsafe()));
            break;
        case T::INT64:
            into.emplace_back(key_id, v.get_int64().value_unsafe());
            break;
        case T::UINT64: {
            const std::uint64_t u = v.get_uint64().value_unsafe();
            if (u <= static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max()))
                into.emplace_back(key_id, static_cast<std::int64_t>(u));
            else
                into.emplace_back(key_id, u);
            break;
        }
        case T::DOUBLE:
            into.emplace_back(key_id, v.get_double().value_unsafe());
            break;
        case T::BOOL: {
            const bool b = v.get_bool().value_unsafe();
            into.emplace_back(key_id, static_cast<std::int64_t>(b));
            specials.push_back({key_id, b ? S::TrueValue : S::FalseValue, top});
            break;
        }
        case T::NULL_VALUE:
            specials.push_back({key_id, S::NullValue, top});
            break;
        case T::ARRAY:
            if (v.get_array().value_unsafe().size() == 0)
                specials.push_back({key_id, S::EmptyArray, top});
            break;
        case T::OBJECT:
            if (v.get_object().value_unsafe().size() == 0)
                specials.push_back({key_id, S::EmptyObject, top});
            break;
        case T::BIGINT:
            // Wider than 64 bits: kept exact as its digits.
            into.emplace_back(
                key_id, intern.get_or_insert(v.get_bigint().value_unsafe()));
            break;
    }
}

/// Flatten the array or object `v` into `ev.args` under `path` the way
/// dftracer args are flattened (`path.0`, `path.key`).
void flatten_container(FoldEvent& ev, std::string path,
                       simdjson::dom::element v,
                       dftracer::utils::dstd::StringIntern& intern);

/// Capture a field the POD does not natively carry (top-level type/ph/id, or a
/// nested a.b/a[0]), JSON type preserved. A name that misses at the event root
/// and is neither `args.`-prefixed nor a bare schema field resolves under
/// `args`, as the query evaluator does, and is stored under `name`. A bare
/// schema field goes to `top_fields` so a same-named args key cannot shadow it;
/// everything else to `args`. No-op when the path is absent.
inline void capture_extra_field(FoldEvent& ev, simdjson::dom::element root,
                                dftracer::utils::dstd::StringIntern& intern,
                                const std::string& name) {
    bool ok = false;
    simdjson::dom::element v = resolve_json_path(root, name, ok);
    const bool schema_field = !is_nested_path(name) && is_schema_field(name);
    if (!ok && !schema_field && !dftracer::utils::has_args_prefix(name)) {
        simdjson::dom::element args;
        if (root["args"].get(args) == simdjson::SUCCESS)
            v = resolve_json_path(args, name, ok);
    }
    if (!ok) return;
    const std::uint32_t key_id = intern.get_or_insert(name);
    auto& into = schema_field ? ev.top_fields : ev.args;
    for (const auto& [k, existing] : into)
        if (k == key_id) return;
    for (const auto& s : ev.specials)
        if (s.key == key_id && s.top == schema_field) return;
    if (!schema_field && (v.is_array() || v.is_object()))
        flatten_container(ev, name, v, intern);
    else
        append_scalar_arg(into, ev.specials, key_id, v, intern, schema_field);
}

struct DecodeHints;

/// Enumerate every scalar or null leaf of `root` (arbitrarily nested) into
/// ev.schema_leaves by exact path for the index build's path catalog; the
/// axis and structural keys (pid/tid/ts/dur/ph/id) are excluded.
///
/// With `hints`, the path id of a top-level scalar is remembered in `hints`, so
/// a record interns no path string after its first of that key.
void capture_schema_leaves(FoldEvent& ev, simdjson::dom::element root,
                           dftracer::utils::dstd::StringIntern& intern,
                           bool skip_args = false,
                           DecodeHints* hints = nullptr);

/// What one decoding worker learns from its records, valid for one intern and
/// one record schema: the argument count of the previous record, as the next
/// record's reserve, and the declared field at each leaf path id.
struct DecodeHints {
    static constexpr std::size_t FIELD_CACHE_IDS = std::size_t{1} << 16;

    std::size_t args = 0;
    /// The path being walked by decode_record, reused so a record's walk
    /// allocates no path string.
    std::string path;
    const dftracer::utils::index::RecordSchema* schema = nullptr;
    std::vector<const dftracer::utils::index::FieldSpec*> fields;
    std::vector<std::uint8_t> known;
    /// The catalog path id ("args." + key) of each argument key id, so a
    /// record's args are walked once for both the event and its leaves: by
    /// index below FIELD_CACHE_IDS, by map above.
    std::vector<std::uint32_t> arg_leaf;
    ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> arg_leaf_far;
    /// The path id of each top-level key whose value is a scalar. `packed`
    /// holds a key of up to 8 bytes, zero padded, so it compares as a word.
    struct TopLeaf {
        std::string key;
        std::uint64_t packed;
        std::uint32_t id;
    };
    std::vector<TopLeaf> top_leaf;
    /// The keys of the record being decoded by append_dftracer_record that are
    /// catalog paths, in order, kept from its one walk of the record.
    struct WalkedKey {
        std::string_view key;
        simdjson::dom::element value;
    };
    dftracer::utils::dstd::Vector<WalkedKey> walked;
    /// The key at each position of the previous record's args object, with its
    /// interned id, and of its leaf keys, with their path id. A record of the
    /// same shape finds its keys by one compare, not by hashing them.
    struct KeyAt {
        dftracer::utils::dstd::String key;
        std::uint32_t id = dftracer::utils::dstd::StringIntern::NO_ID;
    };
    dftracer::utils::dstd::Vector<KeyAt> arg_key_at;
    dftracer::utils::dstd::Vector<KeyAt> top_key_at;
};

/// Whether build_fold_event_into, given DecodeHints, walks the record's args
/// into `schema_leaves` itself: only a non-empty args object, which the leaf
/// walk would otherwise visit a second time. An empty or non-object args is
/// left to capture_schema_leaves, which records it.
inline bool args_leaves_in_walk(bool needs_args, bool has_args,
                                simdjson::dom::element args) {
    simdjson::dom::object obj;
    return needs_args && has_args &&
           args.get_object().get(obj) == simdjson::SUCCESS && obj.size() > 0;
}

/// Decode a record of no known trace format into `ev` (reset first), as an
/// owned data event (by_path): every
/// scalar leaf, by exact path ("op", "io.off", "hosts.1"), in `args`, and,
/// with `capture_schema`, every leaf in `schema_leaves`. With `paths` (sorted,
/// borrowed), only those leaves. With `record_schema`, a declared field's
/// value is converted to its type (null when it does not convert) and role
/// fields set ts, dur, pid, tid and name; otherwise the fixed fields are
/// unset. `path_fields`, aligned with `paths`, gives the declared field at
/// each path so no lookup runs per leaf. With `max_children`, an array or
/// object below the root with more children is one leaf: its canonical JSON
/// text at its path. `hints`, when given, is the calling worker's own and is
/// not shared between threads. With `capture_schema` and `skip_never_indexed`,
/// a scalar at a `never_index` path of `record_schema` is listed in
/// `schema_leaves` and its value is not decoded.
void decode_record_into(
    FoldEvent& ev, simdjson::dom::element root,
    dftracer::utils::dstd::StringIntern& intern, bool capture_schema = true,
    const std::vector<std::string>* paths = nullptr,
    const dftracer::utils::index::RecordSchema* record_schema = nullptr,
    const std::vector<const dftracer::utils::index::FieldSpec*>* path_fields =
        nullptr,
    std::size_t max_children = 0, DecodeHints* hints = nullptr,
    bool skip_never_indexed = false);

/// The max_children an index build decodes with, so a record of huge arrays
/// or maps adds a bounded number of catalog paths and zone keys.
inline constexpr std::size_t INDEX_MAX_CHILDREN = 256;

/// Build `ev` (reset first) as an owned event from already-parsed scalars + the
/// args element, for callers (like the index parse) that have run
/// DFTracerEvent::parse_scalars already. Every string is interned, so the
/// result outlives `args`'s parser. With `leaves` (and `needs_args`), each
/// argument leaf also goes into `schema_leaves` as "args.<key>", in the same
/// walk. With `cache`, bound to `intern`, a string the cache has seen is not
/// looked up in `intern` again.
void build_fold_event_into(FoldEvent& ev, const DFTracerEvent& scalars,
                           simdjson::dom::element args, bool has_args,
                           dftracer::utils::dstd::StringIntern& intern,
                           bool needs_args, DecodeHints* leaves = nullptr,
                           dftracer::utils::dstd::InternCache* cache = nullptr);

/// Decode a DFTracer record into the next slot of `batch` in one walk of its
/// keys and one walk of its args: the scalars, the args (when `needs_args`, and
/// the fhash and hhash ids always) and, with `capture_schema`, the schema
/// leaves. The result equals build_fold_event_into and capture_schema_leaves
/// run on DFTracerEvent::parse_scalars. Returns false, appending no slot, for a
/// record that is not an object or has no phase. `hints` is the calling
/// worker's own and is not shared between threads. `cache`, bound to `intern`,
/// answers a string it has seen before.
bool append_dftracer_record(FoldEventBuffer& batch, simdjson::dom::element root,
                            dftracer::utils::dstd::StringIntern& intern,
                            bool needs_args, bool capture_schema,
                            DecodeHints& hints,
                            dftracer::utils::dstd::InternCache* cache);

/// Parse a DOM object into `ev` (reset first) as an owned event. Every string
/// is interned, so the result stays valid after the parser that produced `root`
/// is reused. Args are captured only when `needs_args`. `extra_fields`, if
/// given, names fields the POD does not natively carry (type/ph, a nested
/// a.b/a[0]) to capture into the event. When `capture_schema`, every scalar
/// leaf (arbitrarily nested) is enumerated into `schema_leaves` for the index
/// build's column harvest.
void extract_fold_event_into(
    FoldEvent& ev, simdjson::dom::element root,
    dftracer::utils::dstd::StringIntern& intern, bool needs_args,
    const std::vector<std::string>* extra_fields = nullptr,
    bool capture_schema = false, DecodeHints* hints = nullptr);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_FOLD_EVENT_H
