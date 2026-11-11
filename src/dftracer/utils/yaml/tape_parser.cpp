// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/export.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/allocator.h>
#include <dftracer/utils/yaml/tape.h>

#include <algorithm>
#include <bit>
#include <csetjmp>
#include <cstddef>
#include <cstring>
#include <utility>

// The C headers rapidyaml includes come first: libc declarations carry no
// visibility attribute, so one first seen inside the hidden region below is
// hidden, and the shared library fails to link (undefined hidden `open`).
#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if __has_include(<alloca.h>)
#include <alloca.h>
#endif
#if __has_include(<endian.h>)
#include <endian.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

// The error callbacks of rapidyaml never return: they jump back to the parse.
// Its templates and inline functions stay out of this library's symbols. The
// handler below is a ParseEngine template argument, and the member functions
// of that template are defined in the source file of rapidyaml, so the source
// is compiled in this unit, after the handler.
#pragma GCC visibility push(hidden)
#include <rapidyaml.v0.16.0.singlesrc.hpp>

namespace dftracer::utils::yaml {

namespace {

constexpr std::size_t ERROR_TEXT_CAPACITY = 512;
// A map with fewer keys than this keeps a 32-bit hash for each key and a set
// of bits over them, and looks at the earlier keys only when the bit of a new
// key is set; a map with more keys uses a hash set of key texts.
constexpr std::size_t LINEAR_KEY_LIMIT = 64;
/// A text this long gets its tape sized from its line count.
constexpr std::size_t MIN_SIZED_TEXT = 4096;
/// The nodes that the aliases of one text may stand for, summed over every
/// alias. A walk that follows aliases visits them again, so this bounds the
/// work of an expansion bomb (an anchor that holds aliases of an anchor that
/// holds aliases) without copying anything.
constexpr std::uint64_t ALIAS_EXPANSION_LIMIT = std::uint64_t{1} << 27;

struct Trap {
    std::jmp_buf env;
    dstd::StringView source_name;
    std::uint64_t line_base = 1;
    char text[ERROR_TEXT_CAPACITY];
    std::size_t text_size = 0;
};

using Block = std::max_align_t;

std::size_t block_count(std::size_t bytes) {
    return (bytes + sizeof(Block) - 1) / sizeof(Block);
}

void* ryml_allocate(std::size_t bytes, void*, void*) {
    return memory::Allocator<Block>().allocate(block_count(bytes));
}

void ryml_free(void* mem, std::size_t bytes, void*) {
    memory::Allocator<Block>().deallocate(static_cast<Block*>(mem),
                                          block_count(bytes));
}

dstd::StringView view_of(ryml::csubstr s) {
    return dstd::StringView(s.str, s.len);
}

// Writes `line:column: reason` (with the source name in front when there is
// one), or only the reason without a location, and jumps to the parse.
[[noreturn]] void leave(Trap* trap, bool located, std::uint64_t line,
                        std::uint64_t col, dstd::StringView reason) {
    dstd::FmtResult written;
    if (located && !trap->source_name.empty()) {
        written =
            dstd::fmt_to<"{}:{}:{}: {}">(trap->text, sizeof(trap->text),
                                         trap->source_name, line, col, reason);
    } else if (located) {
        written = dstd::fmt_to<"{}:{}: {}">(trap->text, sizeof(trap->text),
                                            line, col, reason);
    } else if (!trap->source_name.empty()) {
        written = dstd::fmt_to<"{}: {}">(trap->text, sizeof(trap->text),
                                         trap->source_name, reason);
    } else {
        written = dstd::fmt_to<"{}">(trap->text, sizeof(trap->text), reason);
    }
    trap->text_size = written.size;
    std::longjmp(trap->env, 1);
}

[[noreturn]] void on_basic_error(ryml::csubstr msg,
                                 const ryml::ErrorDataBasic& data,
                                 void* user_data) {
    auto* trap = static_cast<Trap*>(user_data);
    const bool located =
        data.location.line != ryml::npos && data.location.col != ryml::npos;
    leave(trap, located, data.location.line + trap->line_base,
          data.location.col + 1, view_of(msg));
}

[[noreturn]] void on_parse_error(ryml::csubstr msg,
                                 const ryml::ErrorDataParse& data,
                                 void* user_data) {
    auto* trap = static_cast<Trap*>(user_data);
    const bool located =
        data.ymlloc.line != ryml::npos && data.ymlloc.col != ryml::npos;
    leave(trap, located, data.ymlloc.line + trap->line_base,
          data.ymlloc.col + 1, view_of(msg));
}

[[noreturn]] void on_visit_error(ryml::csubstr msg, const ryml::ErrorDataVisit&,
                                 void* user_data) {
    leave(static_cast<Trap*>(user_data), false, 0, 0, view_of(msg));
}

ryml::Callbacks make_callbacks(Trap* trap) {
    ryml::Callbacks callbacks;
    callbacks.set_user_data(trap)
        .set_allocate(ryml_allocate)
        .set_free(ryml_free)
        .set_error_basic(on_basic_error)
        .set_error_parse(on_parse_error)
        .set_error_visit(on_visit_error);
    return callbacks;
}

enum class Level : std::uint8_t { Top, Map, Seq };

struct TapeState : ryml::ParserState {
    ryml::type_bits bits;
    /// The tape index of the begin entry of a Map or Seq level.
    std::size_t begin;
    /// The nodes this level adds to a walk, itself included.
    std::uint64_t expanded;
    /// Where the key hashes of this level start in the shared list, and how
    /// many keys the level holds.
    std::size_t key_base;
    std::size_t key_count;
    /// A bit for each key hash seen in the level: a key whose bit is clear is
    /// new, and only a set bit calls for a look at the earlier keys.
    std::uint64_t seen_bits[4];
    Level kind;
    bool has_key;
    bool has_val;
    bool has_set;
};

// The text of a key by its tape index, for the tables of keys. The storage
// moves from parse to parse, so the table reads it through `current`.
struct KeyHash {
    const DocumentStorage* const* current;
    std::size_t operator()(std::uint64_t index) const noexcept {
        return dstd::StringHash{}(key_text(**current, index));
    }
};

struct KeyEqual {
    const DocumentStorage* const* current;
    bool operator()(std::uint64_t a, std::uint64_t b) const noexcept {
        return key_text(**current, a) == key_text(**current, b);
    }
};

using KeySet = dstd::HashSet<std::uint64_t, KeyHash, KeyEqual>;

// A 32-bit hash of a key from its first and last 8 bytes and its length, which
// together are the whole key up to 16 bytes. `limit` is the end of the buffer
// the key is in: the 8-byte loads stay inside it.
std::uint32_t key_hash(const char* p, std::size_t len, const char* limit) {
    static_assert(std::endian::native == std::endian::little);
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    if (len >= 8) {
        std::memcpy(&first, p, 8);
        std::memcpy(&last, p + len - 8, 8);
    } else if (len != 0) {
        if (p + 8 <= limit) {
            std::memcpy(&first, p, 8);
            first &= (std::uint64_t{1} << (8 * len)) - 1;
        } else {
            std::memcpy(&first, p, len);
        }
        last = first;
    }
    std::uint64_t h = first * 0x9E3779B97F4A7C15ull;
    h ^= (last + len) * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 32;
    return static_cast<std::uint32_t>(h);
}

// The newlines in `text`, in blocks the compiler vectorizes.
std::size_t count_newlines(const char* text, std::size_t size) {
    constexpr std::size_t BLOCK = 256;
    std::size_t total = 0;
    std::size_t i = 0;
    for (; i + BLOCK <= size; i += BLOCK) {
        std::uint32_t block = 0;
        for (std::size_t j = 0; j < BLOCK; ++j)
            block += text[i + j] == '\n' ? 1u : 0u;
        total += block;
    }
    for (; i < size; ++i) total += text[i] == '\n' ? 1u : 0u;
    return total;
}

bool is_null_text(ryml::csubstr s) {
    if (s.len == 0) return true;
    if (s.len == 1) return s.str[0] == '~';
    return s.len == 4 && (std::memcmp(s.str, "null", 4) == 0 ||
                          std::memcmp(s.str, "Null", 4) == 0 ||
                          std::memcmp(s.str, "NULL", 4) == 0);
}

// Builds the tape from the events of the rapidyaml parser. Everything here
// runs under the parse, so nothing with a destructor is alive in a function
// that ends in leave().
class TapeHandler : public ryml::EventHandlerStack<TapeHandler, TapeState> {
   public:
    using state = TapeState;
    static constexpr bool requires_strings_on_buffers = false;

    TapeHandler(const ryml::Callbacks& callbacks, Trap* trap)
        : EventHandlerStack(callbacks), trap_(trap) {
        _stack_reset_root();
    }

    void prepare(DocumentStorage* storage) {
        storage_ = storage;
        current_ = storage;
        text_begin_ = storage->base;
        text_end_ = storage->base + storage->size;
        hashes_.clear();
        for (KeySet& set : sets_) set.clear();
        sets_in_use_ = 0;
        anchors_.clear();
        has_pending_anchor_ = false;
        last_anchor_index_ = NONE;
        alias_total_ = 0;
        _stack_reset_root();
        init_level(*m_curr, Level::Top, 0);
        m_curr->flags |= ryml::RUNK | ryml::RTOP;
    }

    ryml::Callbacks const& callbacks() const { return m_stack.m_callbacks; }
    ryml::TagDirectives& tag_directives() { return tag_directives_; }
    ryml::TagCache& tag_cache() { return tag_cache_; }

    template <ryml::type_bits B>
    bool has_any_() const noexcept {
        return (m_curr->bits & B) != 0;
    }

    void start_parse(const char* filename, ryml::substr src) {
        _stack_start_parse(filename, src);
    }
    void finish_parse() {}
    void cancel_parse() {}
    void begin_stream() const noexcept {}
    void end_stream() const noexcept {}

    void begin_doc() {
        if (_stack_should_push_on_begin_doc()) push_doc();
    }
    void end_doc() {
        if (_stack_should_pop_on_end_doc()) _stack_pop();
    }
    void begin_doc_expl() { push_doc(); }
    void end_doc_expl() {
        if (_stack_should_pop_on_end_doc()) _stack_pop();
    }

    void begin_map_key_flow() { complex_key(); }
    void begin_map_key_block() { complex_key(); }
    void begin_seq_key_flow() { complex_key(); }
    void begin_seq_key_block() { complex_key(); }
    void actually_val_is_first_key_of_new_map_block() { complex_key(); }

    void begin_map_val_flow() { begin_container(Level::Map); }
    void begin_map_val_block() { begin_container(Level::Map); }
    void begin_seq_val_flow() { begin_container(Level::Seq); }
    void begin_seq_val_block() { begin_container(Level::Seq); }
    void end_map_block() { end_container(); }
    void end_map_flow(bool, ryml::type_bits = ryml::FLOW_ML1) {
        end_container();
    }
    void end_seq_block() { end_container(); }
    void end_seq_flow(bool, ryml::type_bits = ryml::FLOW_ML1) {
        end_container();
    }

    void add_sibling() { close_slot(*m_curr); }

    // `[a: 1]`: the scalar just read is the first key of a map that is the
    // element. The tape grows only at its end, so the scalar is the last
    // entry and the map begin goes in front of it.
    void actually_val_is_first_key_of_new_map_flow() {
        TapeState& level = *m_curr;
        if (level.kind != Level::Seq || !level.has_val ||
            storage_->tape.empty())
            complex_key();
        const std::size_t scalar_index = storage_->tape.size() - 1;
        if (storage_->tape[scalar_index].is_container() ||
            (storage_->tape[scalar_index].flags & tape_flag::KEY) != 0)
            complex_key();
        TapeEntry key = storage_->tape[scalar_index];
        storage_->tape.resize(scalar_index);
        key.flags |= tape_flag::KEY;
        TapeEntry begin{};
        begin.kind = TapeKind::Map;
        if (key.kind == TapeKind::Scalar &&
            (key.flags & (tape_flag::QUOTED | tape_flag::BLOCK)) == 0 &&
            key.size() == 2 &&
            entry_text(*storage_, key) == dstd::StringView("<<")) {
            key.flags |= tape_flag::MERGE;
            begin.flags |= tape_flag::MERGE;
        }
        const std::size_t begin_index = storage_->tape.size();
        storage_->tape.push_back(begin);
        storage_->tape.push_back(key);
        if (last_anchor_index_ == scalar_index) {
            last_anchor_index_ = scalar_index + 1;
            anchors_[last_anchor_name_] = scalar_index + 1;
        }
        level.bits |= ryml::MAP;
        level.expanded -= 1;
        push_level(Level::Map, begin_index);
        m_curr->expanded = 2;
        m_curr->has_key = true;
        note_key_of(*m_curr, scalar_index + 1);
    }

    void set_key_scalar_plain_empty() noexcept { add_key({}, 0, true); }
    void set_val_scalar_plain_empty() noexcept { add_val({}, 0, true); }
    void set_key_scalar_plain(ryml::csubstr s) noexcept { add_key(s, 0, true); }
    void set_key_scalar_dquoted(ryml::csubstr s) noexcept {
        add_key(s, tape_flag::QUOTED, false);
    }
    void set_key_scalar_squoted(ryml::csubstr s) noexcept {
        add_key(s, tape_flag::QUOTED, false);
    }
    void set_key_scalar_literal(ryml::csubstr s) noexcept {
        add_key(s, tape_flag::BLOCK, false);
    }
    void set_key_scalar_folded(ryml::csubstr s) noexcept {
        add_key(s, tape_flag::BLOCK, false);
    }
    void set_val_scalar_plain(ryml::csubstr s) noexcept { add_val(s, 0, true); }
    void set_val_scalar_dquoted(ryml::csubstr s) noexcept {
        add_val(s, tape_flag::QUOTED, false);
    }
    void set_val_scalar_squoted(ryml::csubstr s) noexcept {
        add_val(s, tape_flag::QUOTED, false);
    }
    void set_val_scalar_literal(ryml::csubstr s) noexcept {
        add_val(s, tape_flag::BLOCK, false);
    }
    void set_val_scalar_folded(ryml::csubstr s) noexcept {
        add_val(s, tape_flag::BLOCK, false);
    }
    void mark_key_scalar_unfiltered() noexcept {}
    void mark_val_scalar_unfiltered() noexcept {}

    void set_key_anchor(ryml::csubstr name) { remember_anchor(name); }
    void set_val_anchor(ryml::csubstr name) { remember_anchor(name); }
    void set_key_ref(ryml::csubstr ref) { add_alias(ref, true); }
    void set_val_ref(ryml::csubstr ref) { add_alias(ref, false); }

    void set_key_tag(ryml::csubstr) {}
    void set_val_tag(ryml::csubstr) {}
    void add_directive_yaml(ryml::csubstr) {}
    void add_directive_tag(ryml::csubstr, ryml::csubstr) {}

    ryml::substr arena() {
        return ryml::substr(storage_->arena.data(), storage_->arena_used);
    }
    ryml::substr arena_rem() {
        return ryml::substr(storage_->arena.data() + storage_->arena_used,
                            storage_->arena.size() - storage_->arena_used);
    }
    ryml::substr alloc_arena(std::size_t len) {
        const std::size_t used = storage_->arena_used;
        if (used + len > storage_->arena.size())
            storage_->arena.resize(std::max(
                {storage_->arena.size() * 2, used + len, std::size_t{256}}));
        storage_->arena_used = used + len;
        return ryml::substr(storage_->arena.data() + used, len);
    }

    // The engine calls these only when it parses into a node that already
    // exists, which a parse from the root never does.
    void _push() { unsupported_mode(); }
    void _pop() { unsupported_mode(); }

    ryml::id_type m_curr_doc = 0;

   private:
    static constexpr std::size_t NONE = static_cast<std::size_t>(-1);

    static void init_level(TapeState& level, Level kind, std::size_t begin) {
        level.bits = 0;
        level.kind = kind;
        level.begin = begin;
        level.expanded = 1;
        level.has_key = false;
        level.has_val = false;
        level.has_set = false;
    }

    void push_level(Level kind, std::size_t begin) {
        _stack_push();
        init_level(*m_curr, kind, begin);
        m_curr->key_base = hashes_.size();
        m_curr->key_count = 0;
        m_curr->seen_bits[0] = m_curr->seen_bits[1] = 0;
        m_curr->seen_bits[2] = m_curr->seen_bits[3] = 0;
    }

    void push_doc() {
        anchors_.clear();
        has_pending_anchor_ = false;
        last_anchor_index_ = NONE;
        push_level(Level::Top, 0);
        m_curr->bits = ryml::DOC;
    }

    [[noreturn]] void fail_here(dstd::StringView reason) {
        leave(trap_, true, m_curr->pos.line + trap_->line_base - 1,
              m_curr->pos.col, reason);
    }

    [[noreturn]] void unsupported_mode() {
        fail_here("internal error: parsing into an existing node");
    }

    [[noreturn]] void complex_key() {
        fail_here("complex keys are not supported");
    }

    C4_ALWAYS_INLINE std::size_t append(const TapeEntry& entry) {
        dstd::RawBuffer<TapeEntry>& tape = storage_->tape;
        const std::size_t index = tape.size();
        tape.push_back(entry);
        if (has_pending_anchor_) {
            has_pending_anchor_ = false;
            last_anchor_name_ = pending_anchor_;
            last_anchor_index_ = index;
            anchors_[pending_anchor_] = index;
        }
        return index;
    }

    void remember_anchor(ryml::csubstr name) {
        pending_anchor_ = view_of(name);
        has_pending_anchor_ = true;
    }

    C4_ALWAYS_INLINE TapeEntry scalar_entry(ryml::csubstr s, std::uint8_t flags,
                                            bool plain) {
        TapeEntry entry{};
        entry.flags = flags;
        entry.kind = TapeKind::Scalar;
        if (s.len == 0) {
            if (plain) entry.kind = TapeKind::Null;
            return entry;
        }
        if (s.len > TAPE_SIZE_MAX) fail_here("scalar too long");
        if (s.str >= text_begin_ && s.str + s.len <= text_end_) {
            entry.pos = static_cast<std::uint64_t>(s.str - text_begin_);
        } else {
            const char* arena = storage_->arena.data();
            if (arena == nullptr || s.str < arena ||
                s.str + s.len > arena + storage_->arena_used)
                fail_here("internal error: a scalar outside the text");
            entry.pos = static_cast<std::uint64_t>(s.str - arena);
            entry.flags |= tape_flag::ARENA;
        }
        entry.set_size(s.len);
        if (plain && is_null_text(s)) entry.kind = TapeKind::Null;
        return entry;
    }

    C4_ALWAYS_INLINE void add_key(ryml::csubstr s, std::uint8_t style,
                                  bool plain) {
        TapeState& level = *m_curr;
        if (level.kind != Level::Map) fail_here("a key outside a map");
        TapeEntry entry = scalar_entry(s, style | tape_flag::KEY, plain);
        if (plain && s.len == 2 && s.str[0] == '<' && s.str[1] == '<') {
            entry.flags |= tape_flag::MERGE;
            storage_->tape[level.begin].flags |= tape_flag::MERGE;
        }
        const std::size_t index = append(entry);
        level.expanded += 1;
        level.has_key = true;
        level.has_val = false;
        if (entry.kind == TapeKind::Null) {
            note_key(level, index, nullptr, 0, true);
        } else {
            note_key(level, index, s.str, s.len,
                     s.str >= text_begin_ && s.str <= text_end_);
        }
    }

    C4_ALWAYS_INLINE void add_val(ryml::csubstr s, std::uint8_t style,
                                  bool plain) {
        TapeState& level = *m_curr;
        ensure_key(level);
        append(scalar_entry(s, style, plain));
        level.expanded += 1;
        level.has_val = true;
        level.bits |= ryml::VAL;
    }

    // A value without a key in a map: the key was empty.
    void ensure_key(TapeState& level) {
        if (level.kind != Level::Map || level.has_key) return;
        TapeEntry entry{};
        entry.kind = TapeKind::Null;
        entry.flags = tape_flag::KEY;
        const std::size_t index = append(entry);
        level.expanded += 1;
        level.has_key = true;
        note_key(level, index, nullptr, 0, true);
    }

    // A key without a value: the value is null.
    void close_slot(TapeState& level) {
        if (level.kind == Level::Map && level.has_key && !level.has_val) {
            TapeEntry entry{};
            entry.kind = TapeKind::Null;
            append(entry);
            level.expanded += 1;
        }
        level.has_key = false;
        level.has_val = false;
    }

    void begin_container(Level kind) {
        TapeState& level = *m_curr;
        ensure_key(level);
        TapeEntry entry{};
        entry.kind = kind == Level::Map ? TapeKind::Map : TapeKind::Seq;
        const std::size_t index = append(entry);
        level.has_val = true;
        level.bits |= kind == Level::Map ? ryml::MAP : ryml::SEQ;
        push_level(kind, index);
    }

    void end_container() {
        close_slot(*m_curr);
        const TapeState& level = *m_curr;
        TapeEntry& begin = storage_->tape[level.begin];
        begin.pos = storage_->tape.size();
        begin.set_size(std::min(level.expanded, TAPE_SIZE_MAX));
        const std::uint64_t expanded = level.expanded;
        if (level.kind == Level::Map) {
            hashes_.resize(level.key_base);
            if (level.has_set) {
                --sets_in_use_;
                sets_[sets_in_use_].clear();
            }
        }
        _stack_pop();
        m_curr->expanded += expanded;
    }

    void add_alias(ryml::csubstr ref, bool is_key) {
        TapeState& level = *m_curr;
        if (is_key && level.kind != Level::Map)
            fail_here("a key outside a map");
        if (!is_key) ensure_key(level);
        const auto found = anchors_.find(view_of(ref.sub(1)));
        if (found == anchors_.end()) {
            char reason[ERROR_TEXT_CAPACITY / 2];
            const dstd::FmtResult written =
                dstd::fmt_to<"undefined alias '{}'">(reason, sizeof(reason),
                                                     view_of(ref));
            fail_here(dstd::StringView(reason, written.size));
        }
        const std::size_t target = found->second;
        const TapeEntry& named = storage_->tape[target];
        if (named.is_container() && named.pos == 0)
            fail_here("an alias to a node that contains it");
        if (is_key && named.is_container()) complex_key();
        const std::uint64_t nodes = named.is_container() ? named.size() : 1;
        alias_total_ += nodes;
        if (alias_total_ > ALIAS_EXPANSION_LIMIT)
            fail_here("aliases expand to too many nodes");
        TapeEntry entry{};
        entry.kind = TapeKind::Alias;
        entry.pos = target;
        entry.set_size(nodes);
        if (is_key) entry.flags = tape_flag::KEY;
        const std::size_t index = append(entry);
        level.expanded += nodes;
        if (is_key) {
            level.has_key = true;
            level.has_val = false;
            note_key_of(level, index);
        } else {
            level.has_val = true;
            level.bits |= ryml::VAL;
        }
    }

    // Records a key of a map and fails when the map holds its text already.
    void note_key_of(TapeState& level, std::size_t index) {
        const dstd::StringView text = key_text(*storage_, index);
        const bool in_text =
            text.data() >= text_begin_ && text.data() <= text_end_;
        note_key(level, index, text.data(), text.size(), in_text);
    }

    C4_ALWAYS_INLINE void note_key(TapeState& level, std::size_t index,
                                   const char* text, std::size_t length,
                                   bool in_text) {
        const std::size_t count = level.key_count++;
        if (count >= LINEAR_KEY_LIMIT) {
            note_key_in_set(level, index);
            return;
        }
        const char* limit =
            in_text ? text_end_
                    : storage_->arena.data() + storage_->arena.size();
        const std::uint32_t hash = key_hash(text, length, limit);
        std::uint64_t& word = level.seen_bits[(hash >> 6) & 3];
        const std::uint64_t bit = std::uint64_t{1} << (hash & 63);
        if ((word & bit) != 0) [[unlikely]]
            check_repeat(level, index, hash, count);
        word |= bit;
        hashes_.push_back(hash);
    }

    // A key whose hash bit was set: a look at the earlier keys.
    DFTRACER_UTILS_NOINLINE void check_repeat(const TapeState& level,
                                              std::size_t index,
                                              std::uint32_t hash,
                                              std::size_t count) {
        const std::uint32_t* seen = hashes_.data() + level.key_base;
        for (std::size_t i = 0; i < count; ++i) {
            if (seen[i] == hash && has_key_before(level, index))
                fail_duplicate(key_text(*storage_, index), index);
        }
    }

    DFTRACER_UTILS_NOINLINE void note_key_in_set(TapeState& level,
                                                 std::size_t index) {
        if (!level.has_set) {
            if (sets_in_use_ == sets_.size())
                sets_.emplace_back(0, KeyHash{&current_}, KeyEqual{&current_});
            KeySet& set = sets_[sets_in_use_++];
            for (std::size_t i = level.begin + 1; i < index;) {
                set.insert(i);
                i = skip_node(i + 1);
            }
            level.has_set = true;
        }
        if (!sets_[sets_in_use_ - 1].insert(index).second)
            fail_duplicate(key_text(*storage_, index), index);
    }

    std::size_t skip_node(std::size_t index) const {
        const TapeEntry& entry = storage_->tape[index];
        return entry.is_container() ? entry.pos : index + 1;
    }

    // Whether a key before the one at `index` has its text.
    bool has_key_before(const TapeState& level, std::size_t index) const {
        const dstd::StringView text = key_text(*storage_, index);
        for (std::size_t i = level.begin + 1; i < index;) {
            if (key_text(*storage_, i) == text) return true;
            i = skip_node(i + 1);
        }
        return false;
    }

    // The key starts where its text does, or one column before when quoted.
    [[noreturn]] void fail_duplicate(dstd::StringView key, std::size_t index) {
        char reason[ERROR_TEXT_CAPACITY / 2];
        const dstd::FmtResult written =
            dstd::fmt_to<"duplicate key '{}'">(reason, sizeof(reason), key);
        const ryml::LineContents& line = m_curr->line_contents;
        std::uint64_t col = m_curr->pos.col;
        if (!key.empty() && key.data() >= line.full.str &&
            key.data() <= line.full.str + line.full.len)
            col = static_cast<std::uint64_t>(key.data() - line.full.str) + 1 -
                  ((storage_->tape[index].flags & tape_flag::QUOTED) != 0 ? 1
                                                                          : 0);
        leave(trap_, true, m_curr->pos.line + trap_->line_base - 1, col,
              dstd::StringView(reason, written.size));
    }

    Trap* trap_;
    DocumentStorage* storage_ = nullptr;
    const DocumentStorage* current_ = nullptr;
    const char* text_begin_ = nullptr;
    const char* text_end_ = nullptr;
    dstd::RawBuffer<std::uint32_t> hashes_;
    dstd::Vector<KeySet> sets_;
    std::size_t sets_in_use_ = 0;
    dstd::HashMap<dstd::StringView, std::size_t> anchors_;
    dstd::StringView pending_anchor_;
    bool has_pending_anchor_ = false;
    dstd::StringView last_anchor_name_;
    std::size_t last_anchor_index_ = NONE;
    std::uint64_t alias_total_ = 0;
    ryml::TagDirectives tag_directives_;
    ryml::TagCache tag_cache_;
};

}  // namespace

struct TapeParserImpl {
    Trap trap;
    TapeHandler handler;

    TapeParserImpl() : handler(make_callbacks(&trap), &trap) {}
};

}  // namespace dftracer::utils::yaml

#include <rapidyaml_impl.hpp>
#pragma GCC visibility pop

namespace c4::yml {
template class ParseEngine<::dftracer::utils::yaml::TapeHandler>;
}  // namespace c4::yml

namespace dftracer::utils::yaml {

namespace {

using Engine = ryml::ParseEngine<TapeHandler>;

struct ParseJob {
    Engine* engine;
    ryml::csubstr name;
    ryml::substr text;
};

void run_parse(ParseJob* job) {
    job->engine->parse_in_place_ev(job->name, job->text);
}

// Holds only trivial locals, so the jump back leaves nothing undestroyed.
bool run_guarded(Trap* trap, ParseJob* job) {
    if (setjmp(trap->env) != 0) return false;
    run_parse(job);
    return true;
}

}  // namespace

TapeParser::TapeParser() : impl_(dstd::make_unique<TapeParserImpl>()) {}
TapeParser::~TapeParser() = default;

dstd::Result<void> TapeParser::parse(DocumentStorage& storage,
                                     dstd::StringView source_name,
                                     std::uint64_t first_line) {
    TapeParserImpl& impl = *impl_;
    impl.trap.source_name = source_name;
    impl.trap.line_base = first_line;
    impl.trap.text_size = 0;
    storage.reset_tape();
    // A block text has one or two entries a line, so this sizes the tape
    // once: the heap grows a buffer by copying it, which holds the old and the
    // new one together. A text of long flow lines grows past it.
    if (storage.size >= MIN_SIZED_TEXT) {
        const std::size_t lines = count_newlines(storage.base, storage.size);
        storage.tape.reserve(std::min(2 * lines + lines / 4, storage.size / 4) +
                             16);
    }
    impl.handler.prepare(&storage);
    Engine engine(&impl.handler, ryml::ParserOptions().detect_flow_ml(false));
    ParseJob job{&engine, ryml::csubstr(source_name.data(), source_name.size()),
                 ryml::substr(const_cast<char*>(storage.base), storage.size)};
    if (!run_guarded(&impl.trap, &job)) {
        storage.reset_tape();
        return common::make_error<"{}">(
            common::ErrorCode::InvalidArgument,
            dstd::StringView(impl.trap.text, impl.trap.text_size));
    }
    return {};
}

}  // namespace dftracer::utils::yaml
