// Arrow IPC save/load for in-memory CallTree. Produces a .arrow file with a
// single record batch (zstd buffer-level compression by default) consumable
// by pyarrow / polars / nanoarrow / dfanalyzer.
//
// Schema (one row per CallTreeNode, rows grouped by ProcessKey and ordered
// by call_sequence within each group):
//
//   pid          uint64
//   tid          uint64
//   node_pkid    uint64           // ProcessKey.node_id
//   id           uint64           // node id
//   name         utf8             // ZSTD compresses repeated values well
//   category     utf8
//   start_time   uint64
//   duration     uint64
//   level        int64
//   parent_id    uint64
//   is_root      bool             // node is in ProcessCallTree::root_calls
//   seq_idx      int64            // position in ProcessCallTree::call_sequence
//   children     utf8             // ',' joined child ids
//   arg_keys     utf8             // '\x1f' (US sep) joined keys
//   arg_values   utf8             // '\x1f' joined stringified values

#include <dftracer/utils/call_tree/internal/call_tree.h>
#include <dftracer/utils/call_tree/internal/process_call_tree.h>
#include <dftracer/utils/call_tree/internal/process_key.h>
#include <dftracer/utils/call_tree/mpi/serializable.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/string_intern.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#include <dftracer/utils/utilities/common/arrow/ipc_reader.h>
#include <dftracer/utils/utilities/common/arrow/ipc_writer.h>
#include <dftracer/utils/utilities/composites/dft/args_map.h>
#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#endif

namespace dftracer::utils::call_tree {

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

namespace {

using utilities::common::arrow::ArrowExportResult;
using utilities::common::arrow::ColumnSpec;
using utilities::common::arrow::ColumnType;
using utilities::common::arrow::IpcCompression;
using utilities::common::arrow::IpcReader;
using utilities::common::arrow::IpcWriter;
using utilities::common::arrow::RecordBatchBuilder;
using utilities::composites::dft::ArgsValueProxy;

constexpr char ARG_SEP = '\x1f';

void join_uint64(std::string& out, const std::vector<std::uint64_t>& v) {
    out.clear();
    bool first = true;
    for (auto x : v) {
        if (!first) out.push_back(',');
        out.append(std::to_string(x));
        first = false;
    }
}

std::string args_value_to_string(ArgsValueProxy v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_uint()) return std::to_string(v.get<std::uint64_t>());
    if (v.is_int()) return std::to_string(v.get<std::int64_t>());
    if (v.is_number()) return std::to_string(v.get<double>());
    if (v.is_bool()) return v.get<bool>() ? "true" : "false";
    return {};
}

dftracer::utils::StringIntern& arrow_load_intern() {
    static dftracer::utils::StringIntern instance;
    return instance;
}

// Split `s` on `delim`; preserves empty tokens.
std::vector<std::string_view> split_view(std::string_view s, char delim) {
    std::vector<std::string_view> out;
    if (s.empty()) return out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == delim) {
            out.emplace_back(s.data() + start, i - start);
            start = i + 1;
        }
    }
    out.emplace_back(s.data() + start, s.size() - start);
    return out;
}

std::uint64_t parse_u64(std::string_view s) {
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') break;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}

// Locate the index of column `name` in a flat record-batch schema.
int find_column(ArrowSchema* schema, const char* name) {
    if (!schema || !schema->children) return -1;
    for (int i = 0; i < schema->n_children; ++i) {
        if (schema->children[i] && schema->children[i]->name &&
            std::strcmp(schema->children[i]->name, name) == 0) {
            return i;
        }
    }
    return -1;
}

// Pull a string out of an Arrow string-view: plain utf8 returns the
// underlying buffer; dictionary<utf8> resolves the dictionary entry.
std::string_view get_string(const ArrowArrayView* view, std::int64_t row) {
    if (view->dictionary != nullptr) {
        const std::int64_t idx = ArrowArrayViewGetIntUnsafe(view, row);
        auto s = ArrowArrayViewGetStringUnsafe(view->dictionary, idx);
        return std::string_view(s.data, static_cast<std::size_t>(s.size_bytes));
    }
    auto s = ArrowArrayViewGetStringUnsafe(view, row);
    return std::string_view(s.data, static_cast<std::size_t>(s.size_bytes));
}

}  // namespace

coro::CoroTask<bool> save_arrow(CoroScope* /*scope*/,
                                const internal::CallTree& tree,
                                std::string output_path) {
    RecordBatchBuilder builder;
    builder.declare_schema({
        {"pid", ColumnType::UINT64},
        {"tid", ColumnType::UINT64},
        {"node_pkid", ColumnType::UINT64},
        {"id", ColumnType::UINT64},
        {"name", ColumnType::STRING},
        {"category", ColumnType::STRING},
        {"start_time", ColumnType::UINT64},
        {"duration", ColumnType::UINT64},
        {"level", ColumnType::INT64},
        {"parent_id", ColumnType::UINT64},
        {"is_root", ColumnType::BOOL},
        {"seq_idx", ColumnType::INT64},
        {"children", ColumnType::STRING},
        {"arg_keys", ColumnType::STRING},
        {"arg_values", ColumnType::STRING},
    });

    auto keys = const_cast<internal::CallTree&>(tree).keys();
    std::string children_join;
    std::string arg_keys_join;
    std::string arg_values_join;

    for (const auto& key : keys) {
        auto* graph = const_cast<internal::CallTree&>(tree).get(key);
        if (!graph) continue;

        std::unordered_set<std::uint64_t> root_set(graph->root_calls.begin(),
                                                   graph->root_calls.end());

        std::int64_t seq_idx = 0;
        for (std::uint64_t nid : graph->call_sequence) {
            auto it = graph->calls.find(nid);
            if (it == graph->calls.end()) continue;
            const auto& node = it->second;
            if (!node) continue;

            builder.append_uint64(0, key.pid);
            builder.append_uint64(1, key.tid);
            builder.append_uint64(2, key.node_id);
            builder.append_uint64(3, node->get_id());
            builder.append_string(4, node->get_name());
            builder.append_string(5, node->get_category());
            builder.append_uint64(6, node->get_start_time());
            builder.append_uint64(7, node->get_duration());
            builder.append_int64(8,
                                 static_cast<std::int64_t>(node->get_level()));
            builder.append_uint64(9, node->get_parent_id());
            builder.append_bool(10, root_set.count(nid) > 0);
            builder.append_int64(11, seq_idx++);

            join_uint64(children_join, node->get_children());
            builder.append_string(12, children_join);

            arg_keys_join.clear();
            arg_values_join.clear();
            bool first = true;
            node->get_args().for_each_member(
                [&](std::string_view k, ArgsValueProxy v) {
                    if (!first) {
                        arg_keys_join.push_back(ARG_SEP);
                        arg_values_join.push_back(ARG_SEP);
                    }
                    arg_keys_join.append(k);
                    arg_values_join.append(args_value_to_string(v));
                    first = false;
                });
            builder.append_string(13, arg_keys_join);
            builder.append_string(14, arg_values_join);

            builder.end_row();
        }
    }

    if (builder.num_rows() == 0) {
        DFTRACER_UTILS_LOG_WARN("save_arrow: tree is empty, writing empty %s",
                                output_path.c_str());
    }

    auto batch = builder.finish();
    IpcWriter writer;
    if (co_await writer.open(output_path, IpcCompression::ZSTD) != 0) {
        DFTRACER_UTILS_LOG_ERROR("save_arrow: open failed: %s",
                                 output_path.c_str());
        co_return false;
    }
    if (co_await writer.write_batch(batch) != 0) {
        DFTRACER_UTILS_LOG_ERROR("%s", "save_arrow: write_batch failed");
        co_return false;
    }
    if (co_await writer.close() != 0) {
        DFTRACER_UTILS_LOG_ERROR("%s", "save_arrow: close failed");
        co_return false;
    }
    co_return true;
}

coro::CoroTask<std::unique_ptr<internal::CallTree>> load_arrow(
    CoroScope* /*scope*/, std::string input_path) {
    IpcReader reader;
    if (reader.open(input_path) != 0) {
        DFTRACER_UTILS_LOG_ERROR("load_arrow: open failed: %s",
                                 input_path.c_str());
        co_return nullptr;
    }

    auto tree = std::make_unique<internal::CallTree>();
    tree->initialize();

    using utilities::composites::dft::ArgsMap;

    auto process_batch = [&](ArrowExportResult& batch) -> int {
        ArrowSchema* schema = batch.get_schema();
        ArrowArray* array = batch.get_array();
        if (!schema || !array) return -1;

        const int c_pid = find_column(schema, "pid");
        const int c_tid = find_column(schema, "tid");
        const int c_node_pkid = find_column(schema, "node_pkid");
        const int c_id = find_column(schema, "id");
        const int c_name = find_column(schema, "name");
        const int c_cat = find_column(schema, "category");
        const int c_start = find_column(schema, "start_time");
        const int c_dur = find_column(schema, "duration");
        const int c_level = find_column(schema, "level");
        const int c_parent = find_column(schema, "parent_id");
        const int c_isroot = find_column(schema, "is_root");
        const int c_seq = find_column(schema, "seq_idx");
        const int c_children = find_column(schema, "children");
        const int c_argk = find_column(schema, "arg_keys");
        const int c_argv = find_column(schema, "arg_values");
        if (c_pid < 0 || c_tid < 0 || c_node_pkid < 0 || c_id < 0 ||
            c_name < 0 || c_cat < 0 || c_start < 0 || c_dur < 0 ||
            c_level < 0 || c_parent < 0 || c_isroot < 0 || c_seq < 0 ||
            c_children < 0 || c_argk < 0 || c_argv < 0) {
            DFTRACER_UTILS_LOG_ERROR("%s",
                                     "load_arrow: schema missing required "
                                     "columns");
            return -1;
        }

        ArrowArrayView view;
        ArrowError err;
        if (ArrowArrayViewInitFromSchema(&view, schema, &err) != NANOARROW_OK) {
            DFTRACER_UTILS_LOG_ERROR("load_arrow: InitFromSchema: %s",
                                     err.message);
            return -1;
        }
        struct ViewGuard {
            ArrowArrayView* v;
            ~ViewGuard() { ArrowArrayViewReset(v); }
        } guard{&view};
        if (ArrowArrayViewSetArray(&view, array, &err) != NANOARROW_OK) {
            DFTRACER_UTILS_LOG_ERROR("load_arrow: SetArray: %s", err.message);
            return -1;
        }

        const std::int64_t n = array->length;
        for (std::int64_t i = 0; i < n; ++i) {
            const std::uint64_t pid =
                ArrowArrayViewGetUIntUnsafe(view.children[c_pid], i);
            const std::uint64_t tid =
                ArrowArrayViewGetUIntUnsafe(view.children[c_tid], i);
            const std::uint64_t node_pkid =
                ArrowArrayViewGetUIntUnsafe(view.children[c_node_pkid], i);
            const std::uint64_t id =
                ArrowArrayViewGetUIntUnsafe(view.children[c_id], i);
            const std::uint64_t start =
                ArrowArrayViewGetUIntUnsafe(view.children[c_start], i);
            const std::uint64_t dur =
                ArrowArrayViewGetUIntUnsafe(view.children[c_dur], i);
            const std::int64_t level =
                ArrowArrayViewGetIntUnsafe(view.children[c_level], i);
            const std::uint64_t parent =
                ArrowArrayViewGetUIntUnsafe(view.children[c_parent], i);
            const bool is_root =
                ArrowArrayViewGetIntUnsafe(view.children[c_isroot], i) != 0;

            auto name_sv = get_string(view.children[c_name], i);
            auto cat_sv = get_string(view.children[c_cat], i);
            auto children_sv = get_string(view.children[c_children], i);
            auto argk_sv = get_string(view.children[c_argk], i);
            auto argv_sv = get_string(view.children[c_argv], i);

            // Args round-trip as strings; type info is lost vs the typed
            // custom-binary format.
            ArgsMap args;
            auto keys_tok = split_view(argk_sv, ARG_SEP);
            auto vals_tok = split_view(argv_sv, ARG_SEP);
            const std::size_t n_args =
                std::min(keys_tok.size(), vals_tok.size());
            if (n_args > 0) args.set_valid(true);
            for (std::size_t k = 0; k < n_args; ++k) {
                args.insert(keys_tok[k], std::string(vals_tok[k]));
            }

            auto name_interned = arrow_load_intern().intern(name_sv);
            auto cat_interned = arrow_load_intern().intern(cat_sv);

            auto node = tree->get_factory().create_node(
                id, name_interned, cat_interned, start, dur,
                static_cast<int>(level), std::move(args));
            node->set_parent_id(parent);
            for (auto child_sv : split_view(children_sv, ',')) {
                if (child_sv.empty()) continue;
                node->add_child(parse_u64(child_sv));
            }

            internal::ProcessKey key(static_cast<std::uint32_t>(pid),
                                     static_cast<std::uint32_t>(tid),
                                     static_cast<std::uint32_t>(node_pkid));
            tree->add_call(key, node);

            // add_call already appended id to call_sequence (in row order,
            // which is the saved call_sequence order). Only push the root
            // flag here.
            auto* pgraph = tree->get(key);
            if (pgraph && is_root) pgraph->root_calls.push_back(id);
        }
        return 0;
    };

    if (reader.for_each_batch(process_batch) != 0) {
        DFTRACER_UTILS_LOG_ERROR("%s", "load_arrow: batch iteration failed");
        co_return nullptr;
    }
    co_return tree;
}

#else   // !DFTRACER_UTILS_ENABLE_ARROW_IPC

coro::CoroTask<bool> save_arrow(CoroScope* /*scope*/,
                                const internal::CallTree& /*tree*/,
                                std::string /*output_path*/) {
    DFTRACER_UTILS_LOG_ERROR("%s",
                             "save_arrow: build without DFTRACER_UTILS_ENABLE_"
                             "ARROW_IPC, cannot write Arrow IPC");
    co_return false;
}

coro::CoroTask<std::unique_ptr<internal::CallTree>> load_arrow(
    CoroScope* /*scope*/, std::string /*input_path*/) {
    DFTRACER_UTILS_LOG_ERROR("%s", "load_arrow: arrow IPC disabled");
    co_return nullptr;
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC

}  // namespace dftracer::utils::call_tree
