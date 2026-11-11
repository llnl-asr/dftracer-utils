// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/allocator.h>
#include <dftracer/utils/yaml/document.h>

#include <csetjmp>
#include <cstddef>
#include <limits>
#include <utility>

// The error callbacks of rapidyaml never return: they jump back to the parse.
// Its templates and inline functions stay out of this library's symbols.
#pragma GCC visibility push(hidden)
#include <rapidyaml.v0.16.0.singlesrc.hpp>
#pragma GCC visibility pop

namespace dftracer::utils::yaml {

namespace {

constexpr std::size_t ERROR_TEXT_CAPACITY = 512;

struct Trap {
    std::jmp_buf env;
    dstd::StringView source_name;
    char text[ERROR_TEXT_CAPACITY];
    std::size_t text_size = 0;
};

ryml::Callbacks make_callbacks(void* user_data);

}  // namespace

struct DocumentStorage {
    dstd::String buffer;
    Trap* trap = nullptr;
    ryml::Tree tree;

    DocumentStorage() : tree(make_callbacks(this)) {}
};

namespace {

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

[[noreturn]] void leave(void* user_data, std::size_t line, std::size_t col,
                        ryml::csubstr msg) {
    auto* storage = static_cast<DocumentStorage*>(user_data);
    Trap* trap = storage->trap;
    if (trap == nullptr) {
        dstd::fatal("rapidyaml: error outside a parse",
                    dstd::String(view_of(msg)).c_str());
    }
    const bool located = line != ryml::npos && col != ryml::npos;
    dstd::FmtResult written;
    if (located && !trap->source_name.empty()) {
        written = dstd::fmt_to<"{}:{}:{}: {}">(trap->text, sizeof(trap->text),
                                               trap->source_name, line + 1,
                                               col + 1, view_of(msg));
    } else if (located) {
        written = dstd::fmt_to<"{}:{}: {}">(trap->text, sizeof(trap->text),
                                            line + 1, col + 1, view_of(msg));
    } else if (!trap->source_name.empty()) {
        written = dstd::fmt_to<"{}: {}">(trap->text, sizeof(trap->text),
                                         trap->source_name, view_of(msg));
    } else {
        written =
            dstd::fmt_to<"{}">(trap->text, sizeof(trap->text), view_of(msg));
    }
    trap->text_size = written.size;
    std::longjmp(trap->env, 1);
}

[[noreturn]] void on_basic_error(ryml::csubstr msg,
                                 const ryml::ErrorDataBasic& data,
                                 void* user_data) {
    leave(user_data, data.location.line, data.location.col, msg);
}

[[noreturn]] void on_parse_error(ryml::csubstr msg,
                                 const ryml::ErrorDataParse& data,
                                 void* user_data) {
    leave(user_data, data.ymlloc.line, data.ymlloc.col, msg);
}

[[noreturn]] void on_visit_error(ryml::csubstr msg, const ryml::ErrorDataVisit&,
                                 void* user_data) {
    leave(user_data, ryml::npos, ryml::npos, msg);
}

ryml::Callbacks make_callbacks(void* user_data) {
    ryml::Callbacks callbacks;
    callbacks.set_user_data(user_data)
        .set_allocate(ryml_allocate)
        .set_free(ryml_free)
        .set_error_basic(on_basic_error)
        .set_error_parse(on_parse_error)
        .set_error_visit(on_visit_error);
    return callbacks;
}

struct ParseJob {
    ryml::Parser* parser;
    ryml::Tree* tree;
    ryml::ReferenceResolver* resolver;
    ryml::csubstr name;
    ryml::substr text;
};

void run_parse(ParseJob* job) {
    ryml::parse_in_place(job->parser, job->name, job->text, job->tree);
}

void run_resolve(ParseJob* job) { job->tree->resolve(job->resolver); }

// Holds only trivial locals, so the jump back leaves nothing undestroyed.
bool run_guarded(Trap* trap, void (*step)(ParseJob*), ParseJob* job) {
    if (setjmp(trap->env) != 0) return false;
    step(job);
    return true;
}

constexpr std::size_t LINEAR_KEY_LIMIT = 16;

// Sets `duplicate` to the first key that a map of the tree holds twice, by the
// text of the key, and returns whether there is one. Reads only.
bool find_duplicate_key(const ryml::Tree& tree, dstd::StringView& duplicate) {
    if (tree.empty()) return false;
    dstd::Vector<std::size_t> pending;
    dstd::Vector<dstd::StringView> keys;
    dstd::HashSet<dstd::StringView> seen;
    pending.push_back(tree.root_id());
    while (!pending.empty()) {
        const std::size_t id = pending.back();
        pending.pop_back();
        const bool is_map = tree.is_map(id);
        keys.clear();
        seen.clear();
        for (std::size_t child = tree.first_child(id); child != ryml::npos;
             child = tree.next_sibling(child)) {
            if (tree.is_container(child)) pending.push_back(child);
            if (!is_map) continue;
            const dstd::StringView key = tree.key_is_null(child)
                                             ? dstd::StringView{}
                                             : view_of(tree.key(child));
            if (keys.size() < LINEAR_KEY_LIMIT) {
                for (const dstd::StringView earlier : keys) {
                    if (earlier == key) {
                        duplicate = key;
                        return true;
                    }
                }
                keys.push_back(key);
                continue;
            }
            if (seen.empty()) seen.insert(keys.begin(), keys.end());
            if (!seen.insert(key).second) {
                duplicate = key;
                return true;
            }
        }
    }
    return false;
}

const ryml::Tree& tree_of(const DocumentStorage* storage) {
    return storage->tree;
}

bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }

bool is_flexible_case(dstd::StringView s) {
    if (s.empty()) return true;
    bool lower = true;
    for (char c : s) lower = lower && is_lower(c);
    if (lower) return true;
    if (!is_upper(s[0])) return false;
    bool rest_lower = true;
    bool rest_upper = true;
    for (std::size_t i = 1; i < s.size(); ++i) {
        rest_lower = rest_lower && is_lower(s[i]);
        rest_upper = rest_upper && is_upper(s[i]);
    }
    return rest_lower || rest_upper;
}

bool equals_lowered(dstd::StringView s, dstd::StringView lower_name) {
    if (s.size() != lower_name.size()) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c =
            is_upper(s[i]) ? static_cast<char>(s[i] - 'A' + 'a') : s[i];
        if (c != lower_name[i]) return false;
    }
    return true;
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
    storage->buffer = std::move(text);

    Trap trap;
    trap.source_name = source_name;
    ryml::EventHandlerTree handler(storage->tree.callbacks());
    ryml::Parser parser(&handler);
    ryml::ReferenceResolver resolver;
    ParseJob job{&parser, &storage->tree, &resolver,
                 ryml::csubstr(source_name.data(), source_name.size()),
                 ryml::substr(storage->buffer.data(), storage->buffer.size())};
    storage->trap = &trap;
    bool parsed = run_guarded(&trap, run_parse, &job);
    storage->trap = nullptr;
    if (parsed) {
        // Before the references are resolved: a merge key overrides keys.
        dstd::StringView duplicate;
        if (find_duplicate_key(storage->tree, duplicate)) {
            if (source_name.empty())
                return common::make_error<"duplicate key '{}'">(
                    common::ErrorCode::InvalidArgument, duplicate);
            return common::make_error<"{}: duplicate key '{}'">(
                common::ErrorCode::InvalidArgument, source_name, duplicate);
        }
        storage->trap = &trap;
        parsed = run_guarded(&trap, run_resolve, &job);
        storage->trap = nullptr;
    }
    if (!parsed) {
        return common::make_error<"{}">(
            common::ErrorCode::InvalidArgument,
            dstd::StringView(trap.text, trap.text_size));
    }
    return Document(std::move(storage));
}

dstd::Result<Document> Document::load_file(dstd::PathView path) {
    auto text = dstd::fs::read_file(path);
    if (!text) return text.error();
    return parse(std::move(*text), path.str());
}

Value Document::root() const {
    const ryml::Tree& tree = tree_of(storage_.get());
    if (tree.empty()) return Value(storage_.get(), ryml::npos);
    std::size_t id = tree.root_id();
    if (tree.is_stream(id)) id = tree.first_child(id);
    return Value(storage_.get(), id);
}

bool Value::is_object() const {
    return id_ != ryml::npos && tree_of(storage_).is_map(id_);
}

bool Value::is_array() const {
    return id_ != ryml::npos && tree_of(storage_).is_seq(id_);
}

bool Value::is_string() const {
    if (id_ == ryml::npos) return false;
    const ryml::Tree& tree = tree_of(storage_);
    return tree.has_val(id_) && !tree.is_container(id_) &&
           !tree.val_is_null(id_);
}

bool Value::is_null() const {
    if (id_ == ryml::npos) return true;
    const ryml::Tree& tree = tree_of(storage_);
    if (tree.is_container(id_)) return false;
    return !tree.has_val(id_) || tree.val_is_null(id_);
}

dstd::Optional<Value> Value::find(dstd::StringView key) const {
    if (!is_object()) return dstd::nullopt;
    const std::size_t child = tree_of(storage_).find_child(
        id_, ryml::csubstr(key.data(), key.size()));
    if (child == ryml::npos) return dstd::nullopt;
    return Value(storage_, child);
}

void Value::for_each_member(
    dstd::FunctionRef<void(dstd::StringView, Value)> fn) const {
    if (!is_object()) return;
    const ryml::Tree& tree = tree_of(storage_);
    for (std::size_t child = tree.first_child(id_); child != ryml::npos;
         child = tree.next_sibling(child)) {
        const dstd::StringView key = tree.key_is_null(child)
                                         ? dstd::StringView{}
                                         : view_of(tree.key(child));
        fn(key, Value(storage_, child));
    }
}

void Value::for_each_element(dstd::FunctionRef<void(Value)> fn) const {
    if (!is_array()) return;
    const ryml::Tree& tree = tree_of(storage_);
    for (std::size_t child = tree.first_child(id_); child != ryml::npos;
         child = tree.next_sibling(child))
        fn(Value(storage_, child));
}

dstd::StringView Value::raw() const {
    if (!is_string()) return {};
    return view_of(tree_of(storage_).val(id_));
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
        if (equals_lowered(text, name)) return true;
    for (const dstd::StringView name : FALSE_NAMES)
        if (equals_lowered(text, name)) return false;
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
