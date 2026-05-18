// Compact custom-binary save/load for in-memory CallTree.
//
// On-disk layout (little-endian, all multi-byte fields native u32/u64/...):
//
//   magic[8]      = "DFTCGRP2"
//   version u32   = 2
//   flags   u32   (reserved, currently 0)
//   string_table:
//     count u32
//     for each: u32 length + raw bytes (utf-8; embedded NULs OK)
//   process_count u32
//   for each ProcessCallTree:
//     pid u32, tid u32, node_id u32
//     call_count u32
//     for each CallTreeNode:
//       id u64
//       name_str_id u32, cat_str_id u32
//       start_time u64, duration u64
//       level i32, parent_id u64
//       child_count u32, then u64 ids
//       arg_count u32, then per arg:
//         key_str_id u32
//         type u8   { 0:string-id-u32, 1:u64, 2:i64, 3:double, 4:bool-u8 }
//         payload (type-dependent)
//     root_count u32, then u64 ids
//     seq_count  u32, then u64 ids

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/call_tree/internal/call_tree.h>
#include <dftracer/utils/call_tree/internal/process_call_tree.h>
#include <dftracer/utils/call_tree/internal/process_key.h>
#include <dftracer/utils/call_tree/mpi/serializable.h>
#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/utilities/composites/dft/args_map.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::call_tree {

namespace {

using utilities::composites::dft::ArgsMap;
using utilities::composites::dft::ArgsValueProxy;

enum ArgTypeTag : std::uint8_t {
    ARG_STRING = 0,
    ARG_U64 = 1,
    ARG_I64 = 2,
    ARG_DOUBLE = 3,
    ARG_BOOL = 4,
};

dftracer::utils::StringIntern& binary_load_intern() {
    static dftracer::utils::StringIntern instance;
    return instance;
}

void append_bytes(std::vector<char>& out, const void* p, std::size_t n) {
    out.insert(out.end(), static_cast<const char*>(p),
               static_cast<const char*>(p) + n);
}

template <typename T>
void put_pod(std::vector<char>& out, T v) {
    append_bytes(out, &v, sizeof(v));
}

// Builder maintains the global string table and assigns dense ids; identical
// strings get the same id. `strings_` is a deque (not vector) so push_back
// keeps existing element addresses stable -- the index_ map stores
// string_views into those elements, and a vector realloc would dangle them
// (especially nasty for SSO strings whose storage lives inside the string
// object).
class StringTable {
   public:
    std::uint32_t intern(std::string_view s) {
        auto it = index_.find(s);
        if (it != index_.end()) return it->second;
        const std::uint32_t id = static_cast<std::uint32_t>(strings_.size());
        strings_.emplace_back(s);
        index_.emplace(std::string_view(strings_.back()), id);
        return id;
    }

    void write(std::vector<char>& out) const {
        put_pod<std::uint32_t>(out,
                               static_cast<std::uint32_t>(strings_.size()));
        for (const auto& s : strings_) {
            put_pod<std::uint32_t>(out, static_cast<std::uint32_t>(s.size()));
            append_bytes(out, s.data(), s.size());
        }
    }

   private:
    std::deque<std::string> strings_;
    ankerl::unordered_dense::map<std::string_view, std::uint32_t> index_;
};

// Cursor over an in-memory buffer; tracks bounds and an `ok` flag so callers
// can early-exit on truncation without per-call error plumbing.
struct Cursor {
    const char* p;
    const char* end;
    bool ok = true;

    template <typename T>
    bool get_pod(T& out) {
        if (end - p < static_cast<std::ptrdiff_t>(sizeof(T))) {
            ok = false;
            return false;
        }
        std::memcpy(&out, p, sizeof(T));
        p += sizeof(T);
        return true;
    }
    bool get_string_view(std::string_view& out, std::uint32_t len) {
        if (end - p < static_cast<std::ptrdiff_t>(len)) {
            ok = false;
            return false;
        }
        out = std::string_view(p, len);
        p += len;
        return true;
    }
};

void serialize_node(std::vector<char>& out, StringTable& strings,
                    const internal::CallTreeNode& n) {
    put_pod<std::uint64_t>(out, n.get_id());
    put_pod<std::uint32_t>(out, strings.intern(n.get_name()));
    put_pod<std::uint32_t>(out, strings.intern(n.get_category()));
    put_pod<std::uint64_t>(out, n.get_start_time());
    put_pod<std::uint64_t>(out, n.get_duration());
    put_pod<std::int32_t>(out, static_cast<std::int32_t>(n.get_level()));
    put_pod<std::uint64_t>(out, n.get_parent_id());

    const auto& children = n.get_children();
    put_pod<std::uint32_t>(out, static_cast<std::uint32_t>(children.size()));
    for (auto id : children) put_pod<std::uint64_t>(out, id);

    // Pull args out as (key, ArgsValueProxy) so we can preserve typing.
    std::vector<std::pair<std::string_view, ArgsValueProxy>> args;
    n.get_args().for_each_member(
        [&](std::string_view k, ArgsValueProxy v) { args.emplace_back(k, v); });

    put_pod<std::uint32_t>(out, static_cast<std::uint32_t>(args.size()));
    for (auto& [k, v] : args) {
        put_pod<std::uint32_t>(out, strings.intern(k));
        if (v.is_string()) {
            put_pod<std::uint8_t>(out, ARG_STRING);
            const auto s = v.get<std::string>();
            put_pod<std::uint32_t>(out, strings.intern(s));
        } else if (v.is_uint()) {
            put_pod<std::uint8_t>(out, ARG_U64);
            put_pod<std::uint64_t>(out, v.get<std::uint64_t>());
        } else if (v.is_int()) {
            put_pod<std::uint8_t>(out, ARG_I64);
            put_pod<std::int64_t>(out, v.get<std::int64_t>());
        } else if (v.is_number()) {
            put_pod<std::uint8_t>(out, ARG_DOUBLE);
            put_pod<double>(out, v.get<double>());
        } else if (v.is_bool()) {
            put_pod<std::uint8_t>(out, ARG_BOOL);
            put_pod<std::uint8_t>(out, v.get<bool>() ? 1 : 0);
        } else {
            put_pod<std::uint8_t>(out, ARG_STRING);
            put_pod<std::uint32_t>(out, strings.intern(""));
        }
    }
}

}  // namespace

coro::CoroTask<bool> save_binary(CoroScope* scope,
                                 const internal::CallTree& tree,
                                 std::string output_path) {
    // First pass: emit body to a scratch buffer while populating the
    // string table. Then write header + table + body.
    std::vector<char> body;
    body.reserve(1 << 20);
    StringTable strings;

    auto keys = const_cast<internal::CallTree&>(tree).keys();
    put_pod<std::uint32_t>(body, static_cast<std::uint32_t>(keys.size()));

    for (const auto& key : keys) {
        auto* graph = const_cast<internal::CallTree&>(tree).get(key);
        if (!graph) continue;

        put_pod<std::uint32_t>(body, key.pid);
        put_pod<std::uint32_t>(body, key.tid);
        put_pod<std::uint32_t>(body, key.node_id);
        put_pod<std::uint32_t>(body,
                               static_cast<std::uint32_t>(graph->calls.size()));
        for (const auto& [id, node] : graph->calls) {
            if (node) serialize_node(body, strings, *node);
        }
        put_pod<std::uint32_t>(
            body, static_cast<std::uint32_t>(graph->root_calls.size()));
        for (auto id : graph->root_calls) put_pod<std::uint64_t>(body, id);
        put_pod<std::uint32_t>(
            body, static_cast<std::uint32_t>(graph->call_sequence.size()));
        for (auto id : graph->call_sequence) put_pod<std::uint64_t>(body, id);
    }

    std::vector<char> out;
    out.reserve(8 + 4 + 4 + body.size() + 16);
    append_bytes(out, CALLTREE_BINARY_MAGIC, sizeof(CALLTREE_BINARY_MAGIC));
    put_pod<std::uint32_t>(out, CALLTREE_BINARY_VERSION);
    put_pod<std::uint32_t>(out, 0u);  // flags
    strings.write(out);
    append_bytes(out, body.data(), body.size());

    utilities::fileio::parallel::WriterConfig wc;
    wc.layout = utilities::fileio::parallel::FileLayout::STRIPED;
    wc.gzip = false;
    auto writer = utilities::fileio::parallel::make_writer(wc);
    if (co_await writer->open(output_path, 1, false, scope) != 0) {
        DFTRACER_UTILS_LOG_ERROR("save_binary: open failed: %s",
                                 output_path.c_str());
        co_return false;
    }
    if (co_await writer->write_chunk(0, ByteView(out.data(), out.size())) !=
        0) {
        co_return false;
    }
    if (co_await writer->close() != 0) co_return false;
    co_return true;
}

coro::CoroTask<std::unique_ptr<internal::CallTree>> load_binary(
    CoroScope* /*scope*/, std::string input_path) {
    int fd = ::open(input_path.c_str(), O_RDONLY);
    if (fd < 0) {
        DFTRACER_UTILS_LOG_ERROR("load_binary: cannot open %s",
                                 input_path.c_str());
        co_return nullptr;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        co_return nullptr;
    }
    std::vector<char> buf(static_cast<std::size_t>(st.st_size));
    std::size_t got = 0;
    while (got < buf.size()) {
        ssize_t n = ::read(fd, buf.data() + got, buf.size() - got);
        if (n <= 0) break;
        got += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (got != buf.size()) co_return nullptr;

    Cursor c{buf.data(), buf.data() + buf.size()};
    if (c.end - c.p < 8 || std::memcmp(c.p, CALLTREE_BINARY_MAGIC,
                                       sizeof(CALLTREE_BINARY_MAGIC)) != 0) {
        DFTRACER_UTILS_LOG_ERROR("load_binary: bad magic in %s",
                                 input_path.c_str());
        co_return nullptr;
    }
    c.p += 8;
    std::uint32_t version = 0, flags = 0;
    if (!c.get_pod(version) || !c.get_pod(flags)) co_return nullptr;
    if (version != CALLTREE_BINARY_VERSION) {
        DFTRACER_UTILS_LOG_ERROR("load_binary: unsupported version %u",
                                 version);
        co_return nullptr;
    }

    std::uint32_t nstr = 0;
    if (!c.get_pod(nstr)) co_return nullptr;
    std::vector<std::string_view> table;
    table.reserve(nstr);
    for (std::uint32_t i = 0; i < nstr && c.ok; ++i) {
        std::uint32_t len = 0;
        std::string_view s;
        if (!c.get_pod(len) || !c.get_string_view(s, len)) co_return nullptr;
        table.push_back(s);
    }
    auto lookup_str = [&](std::uint32_t id) -> std::string_view {
        return id < table.size() ? table[id] : std::string_view{};
    };

    auto tree = std::make_unique<internal::CallTree>();
    tree->initialize();

    std::uint32_t nprocs = 0;
    if (!c.get_pod(nprocs)) co_return nullptr;

    for (std::uint32_t pi = 0; pi < nprocs && c.ok; ++pi) {
        std::uint32_t pid = 0, tid = 0, node_id = 0, ncalls = 0;
        if (!c.get_pod(pid) || !c.get_pod(tid) || !c.get_pod(node_id) ||
            !c.get_pod(ncalls))
            break;
        internal::ProcessKey key(pid, tid, node_id);

        for (std::uint32_t ci = 0; ci < ncalls && c.ok; ++ci) {
            std::uint64_t id = 0, start = 0, dur = 0, parent = 0;
            std::uint32_t name_id = 0, cat_id = 0;
            std::int32_t level = 0;
            if (!c.get_pod(id) || !c.get_pod(name_id) || !c.get_pod(cat_id) ||
                !c.get_pod(start) || !c.get_pod(dur) || !c.get_pod(level) ||
                !c.get_pod(parent))
                break;

            std::uint32_t nchildren = 0;
            if (!c.get_pod(nchildren)) break;
            std::vector<std::uint64_t> children;
            children.reserve(nchildren);
            for (std::uint32_t k = 0; k < nchildren && c.ok; ++k) {
                std::uint64_t cid = 0;
                if (!c.get_pod(cid)) break;
                children.push_back(cid);
            }

            std::uint32_t nargs = 0;
            if (!c.get_pod(nargs)) break;
            ArgsMap args;
            if (nargs > 0) args.set_valid(true);
            for (std::uint32_t k = 0; k < nargs && c.ok; ++k) {
                std::uint32_t key_id = 0;
                std::uint8_t type = 0;
                if (!c.get_pod(key_id) || !c.get_pod(type)) break;
                auto key_sv = lookup_str(key_id);
                switch (type) {
                    case ARG_STRING: {
                        std::uint32_t val_id = 0;
                        if (!c.get_pod(val_id)) {
                            c.ok = false;
                            break;
                        }
                        args.insert(key_sv, std::string(lookup_str(val_id)));
                        break;
                    }
                    case ARG_U64: {
                        std::uint64_t v = 0;
                        if (!c.get_pod(v)) {
                            c.ok = false;
                            break;
                        }
                        args.insert(key_sv, v);
                        break;
                    }
                    case ARG_I64: {
                        std::int64_t v = 0;
                        if (!c.get_pod(v)) {
                            c.ok = false;
                            break;
                        }
                        args.insert(key_sv, v);
                        break;
                    }
                    case ARG_DOUBLE: {
                        double v = 0;
                        if (!c.get_pod(v)) {
                            c.ok = false;
                            break;
                        }
                        args.insert(key_sv, v);
                        break;
                    }
                    case ARG_BOOL: {
                        std::uint8_t v = 0;
                        if (!c.get_pod(v)) {
                            c.ok = false;
                            break;
                        }
                        args.insert(key_sv, v != 0);
                        break;
                    }
                    default:
                        c.ok = false;
                        break;
                }
            }

            auto name = binary_load_intern().intern(lookup_str(name_id));
            auto cat = binary_load_intern().intern(lookup_str(cat_id));
            auto node = tree->get_factory().create_node(
                id, name, cat, start, dur, static_cast<int>(level),
                std::move(args));
            node->set_parent_id(parent);
            for (auto cid : children) node->add_child(cid);
            tree->add_call(key, node);
        }

        auto* pgraph = tree->get(key);
        if (!pgraph) continue;
        // add_call already appended each new node id into call_sequence in
        // insertion order; the saved roots/sequence are authoritative, so
        // clear before replacing.
        pgraph->root_calls.clear();
        pgraph->call_sequence.clear();
        std::uint32_t nroots = 0;
        if (!c.get_pod(nroots)) break;
        for (std::uint32_t k = 0; k < nroots && c.ok; ++k) {
            std::uint64_t id = 0;
            if (!c.get_pod(id)) break;
            pgraph->root_calls.push_back(id);
        }
        std::uint32_t nseq = 0;
        if (!c.get_pod(nseq)) break;
        for (std::uint32_t k = 0; k < nseq && c.ok; ++k) {
            std::uint64_t id = 0;
            if (!c.get_pod(id)) break;
            pgraph->call_sequence.push_back(id);
        }
    }

    if (!c.ok) {
        DFTRACER_UTILS_LOG_ERROR("load_binary: truncated/malformed file %s",
                                 input_path.c_str());
        co_return nullptr;
    }
    co_return tree;
}

}  // namespace dftracer::utils::call_tree
