// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/config.h>
#include <simdjson.h>

#include <cerrno>
#include <cstdlib>

namespace dftracer::utils::plugins {

dstd::Result<ConfigTree::Node> ConfigTree::build_node(
    const simdjson::dom::element& el) {
    auto fail = [](simdjson::error_code err) {
        return common::make_error<"plugin config does not read: {}">(
            common::ErrorCode::Parse, simdjson::error_message(err));
    };
    Node n;
    switch (el.type()) {
        case simdjson::dom::element_type::OBJECT: {
            n.kind = DFTU_VAL_OBJECT;
            simdjson::dom::object object;
            if (const auto err = el.get_object().get(object)) return fail(err);
            for (auto kv : object) {
                auto child = build_node(kv.value);
                if (!child) return child.error();
                n.members.emplace_back(dstd::String(kv.key), std::move(*child));
            }
            break;
        }
        case simdjson::dom::element_type::ARRAY: {
            n.kind = DFTU_VAL_ARRAY;
            simdjson::dom::array array;
            if (const auto err = el.get_array().get(array)) return fail(err);
            for (auto item : array) {
                auto child = build_node(item);
                if (!child) return child.error();
                n.items.push_back(std::move(*child));
            }
            break;
        }
        case simdjson::dom::element_type::STRING: {
            n.kind = DFTU_VAL_STR;
            dstd::StringView sv;
            if (const auto err = el.get_string().get(sv)) return fail(err);
            n.str = dstd::String(sv);
            break;
        }
        case simdjson::dom::element_type::INT64: {
            n.kind = DFTU_VAL_I64;
            if (const auto err = el.get_int64().get(n.i64)) return fail(err);
            break;
        }
        case simdjson::dom::element_type::UINT64: {
            n.kind = DFTU_VAL_I64;
            std::uint64_t u = 0;
            if (const auto err = el.get_uint64().get(u)) return fail(err);
            n.i64 = static_cast<std::int64_t>(u);
            break;
        }
        case simdjson::dom::element_type::DOUBLE:
            n.kind = DFTU_VAL_F64;
            if (const auto err = el.get_double().get(n.f64)) return fail(err);
            break;
        case simdjson::dom::element_type::BOOL:
            n.kind = DFTU_VAL_BOOL;
            if (const auto err = el.get_bool().get(n.b)) return fail(err);
            break;
        case simdjson::dom::element_type::NULL_VALUE:
        default:
            n.kind = DFTU_VAL_NULL;
            break;
    }
    return n;
}

dstd::Result<ConfigTree> ConfigTree::from_json_file(dstd::StringView path) {
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    const auto err = parser.load(path).get(doc);
    if (err)
        return common::make_error<"failed to parse plugin config '{}': {}">(
            err == simdjson::IO_ERROR ? common::ErrorCode::Io
                                      : common::ErrorCode::Parse,
            path, simdjson::error_message(err));
    if (doc.type() != simdjson::dom::element_type::OBJECT)
        return common::make_error<
            "plugin config '{}' root must be a JSON "
            "object">(common::ErrorCode::InvalidArgument, path);
    ConfigTree tree;
    auto root = build_node(doc);
    if (!root) return root.error();
    tree.root_node_ = std::move(*root);
    return tree;
}

dstd::Result<ConfigTree> ConfigTree::from_json_string(dstd::StringView json) {
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    // The padded copy must outlive `doc`, which borrows its buffer.
    simdjson::padded_string padded(json);
    const auto err = parser.parse(padded).get(doc);
    if (err)
        return common::make_error<"failed to parse plugin config: {}">(
            common::ErrorCode::Parse, simdjson::error_message(err));
    if (doc.type() != simdjson::dom::element_type::OBJECT)
        return common::make_error(common::ErrorCode::InvalidArgument,
                                  "plugin config root must be a JSON object");
    ConfigTree tree;
    auto root = build_node(doc);
    if (!root) return root.error();
    tree.root_node_ = std::move(*root);
    return tree;
}

ConfigTree::Node* ConfigTree::find_member(Node& obj, dstd::StringView key) {
    for (auto& [k, v] : obj.members) {
        if (k == key) return &v;
    }
    return nullptr;
}

ConfigTree::Node ConfigTree::infer_leaf(dstd::StringView raw) {
    Node n;
    if (!raw.empty()) {
        std::int64_t iv = 0;
        const char* first = raw.data();
        const char* last = raw.data() + raw.size();
        const auto r = dstd::from_chars(first, last, iv);
        if (r.ec == std::errc() && r.ptr == last) {
            n.kind = DFTU_VAL_I64;
            n.i64 = iv;
            return n;
        }
    }
    {
        const dstd::String s(raw);
        errno = 0;
        char* end = nullptr;
        const double dv = std::strtod(s.c_str(), &end);
        if (!s.empty() && end == s.c_str() + s.size() && errno == 0) {
            n.kind = DFTU_VAL_F64;
            n.f64 = dv;
            return n;
        }
    }
    using ::dftracer::utils::common::detail::iequals;
    if (iequals(raw, "true")) {
        n.kind = DFTU_VAL_BOOL;
        n.b = true;
        return n;
    }
    if (iequals(raw, "false")) {
        n.kind = DFTU_VAL_BOOL;
        n.b = false;
        return n;
    }
    n.kind = DFTU_VAL_STR;
    n.str = dstd::String(raw);
    return n;
}

void ConfigTree::set(dstd::StringView dotted_key, dstd::StringView raw_value) {
    dirty_ = true;

    dstd::Vector<dstd::StringView> segs;
    std::size_t start = 0;
    while (start <= dotted_key.size()) {
        const auto dot = dotted_key.find('.', start);
        if (dot == dstd::StringView::npos) {
            segs.push_back(dotted_key.substr(start));
            break;
        }
        segs.push_back(dotted_key.substr(start, dot - start));
        start = dot + 1;
    }
    if (segs.empty() || (segs.size() == 1 && segs[0].empty())) return;

    Node* cur = &root_node_;
    for (std::size_t i = 0; i + 1 < segs.size(); ++i) {
        if (cur->kind != DFTU_VAL_OBJECT) {
            *cur = Node{};
            cur->kind = DFTU_VAL_OBJECT;
        }
        Node* child = find_member(*cur, segs[i]);
        if (!child) {
            cur->members.emplace_back(dstd::String(segs[i]), Node{});
            child = &cur->members.back().second;
        }
        if (child->kind != DFTU_VAL_OBJECT) {
            *child = Node{};
            child->kind = DFTU_VAL_OBJECT;
        }
        cur = child;
    }

    if (cur->kind != DFTU_VAL_OBJECT) {
        *cur = Node{};
        cur->kind = DFTU_VAL_OBJECT;
    }
    Node value = infer_leaf(raw_value);
    if (Node* leaf = find_member(*cur, segs.back())) {
        *leaf = std::move(value);
    } else {
        cur->members.emplace_back(dstd::String(segs.back()), std::move(value));
    }
}

void ConfigTree::deep_merge(Node& dst, const Node& src) {
    if (dst.kind == DFTU_VAL_OBJECT && src.kind == DFTU_VAL_OBJECT) {
        for (const auto& [k, sv] : src.members) {
            if (Node* d = find_member(dst, k)) {
                deep_merge(*d, sv);
            } else {
                dst.members.emplace_back(k, sv);
            }
        }
    } else {
        dst = src;
    }
}

void ConfigTree::merge_from(const ConfigTree& other) {
    dirty_ = true;
    deep_merge(root_node_, other.root_node_);
}

void ConfigTree::materialize_into(const Node& node, dftu_value* out) const {
    out->kind = node.kind;
    out->count = 0;
    switch (node.kind) {
        case DFTU_VAL_NULL:
            break;
        case DFTU_VAL_BOOL:
            out->as.b = node.b ? 1u : 0u;
            break;
        case DFTU_VAL_I64:
            out->as.i64 = node.i64;
            break;
        case DFTU_VAL_F64:
            out->as.f64 = node.f64;
            break;
        case DFTU_VAL_STR: {
            strings_.push_back(node.str);
            const dstd::String& s = strings_.back();
            out->count = static_cast<std::uint32_t>(s.size());
            out->as.str = s.c_str();
            break;
        }
        case DFTU_VAL_ARRAY: {
            value_arrays_.emplace_back();
            dstd::Vector<dftu_value>& arr = value_arrays_.back();
            arr.resize(node.items.size());
            out->count = static_cast<std::uint32_t>(node.items.size());
            out->as.items = arr.data();
            for (std::size_t i = 0; i < node.items.size(); ++i) {
                materialize_into(node.items[i], &arr[i]);
            }
            break;
        }
        case DFTU_VAL_OBJECT: {
            value_arrays_.emplace_back();
            dstd::Vector<dftu_value>& vals = value_arrays_.back();
            vals.resize(node.members.size());
            member_arrays_.emplace_back();
            dstd::Vector<dftu_member>& mem = member_arrays_.back();
            mem.resize(node.members.size());
            out->count = static_cast<std::uint32_t>(node.members.size());
            out->as.members = mem.data();
            for (std::size_t i = 0; i < node.members.size(); ++i) {
                strings_.push_back(node.members[i].first);
                const dstd::String& k = strings_.back();
                mem[i].key = k.c_str();
                mem[i].key_len = static_cast<std::uint32_t>(k.size());
                mem[i].value = &vals[i];
                materialize_into(node.members[i].second, &vals[i]);
            }
            break;
        }
    }
}

void ConfigTree::materialize() const {
    strings_.clear();
    value_arrays_.clear();
    member_arrays_.clear();
    root_ = dstd::make_unique<dftu_value>();
    materialize_into(root_node_, root_.get());
    dirty_ = false;
}

const dftu_value* ConfigTree::root() const {
    if (dirty_ || !root_) materialize();
    return root_.get();
}

}  // namespace dftracer::utils::plugins
