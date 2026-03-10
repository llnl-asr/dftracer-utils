#include <dftracer/utils/call_tree/call_tree.h>
#include <dftracer/utils/call_tree/internal/call_tree.h>
#include <dftracer/utils/call_tree/internal/process_key.h>
#include <dftracer/utils/call_tree/internal/trace_reader.h>
#include <dftracer/utils/call_tree/json_serializer.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

namespace dftracer::utils::call_tree {

namespace internal {

/**
 * Internal implementation class (PIMPL pattern)
 * Hides complex CallTree internals from public API
 */
class CallTreeImpl {
   public:
    CallTree graph;
    std::vector<std::string> trace_files;
    std::string trace_directory;
    std::string output_path;
    bool is_generated;

    CallTreeImpl() : is_generated(false) { graph.initialize(); }

    ~CallTreeImpl() { graph.cleanup(); }

    bool find_trace_files(const std::string& dir, const std::string& pattern) {
        trace_files.clear();
        trace_directory = dir;

        if (!fs::exists(dir) || !fs::is_directory(dir)) {
            DFTRACER_UTILS_LOG_ERROR("Directory not found: %s", dir.c_str());
            return false;
        }

        // Recursively find matching files
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();

                // Simple pattern matching for *.ext
                bool matches = false;
                if (pattern == "*") {
                    matches = true;
                } else if (pattern.front() == '*') {
                    std::string suffix = pattern.substr(1);
                    matches = (filename.size() >= suffix.size() &&
                               filename.substr(filename.size() -
                                               suffix.size()) == suffix);
                } else {
                    matches = (filename.find(pattern) != std::string::npos);
                }

                if (matches) {
                    trace_files.push_back(entry.path().string());
                }
            }
        }

        std::sort(trace_files.begin(), trace_files.end());
        return !trace_files.empty();
    }

    bool load_traces() {
        if (trace_files.empty()) {
            DFTRACER_UTILS_LOG_ERROR("%s", "No trace files to load");
            return false;
        }

        TraceReader reader;
        bool success = reader.read_multiple(trace_files, graph);

        if (success) {
            graph.build_hierarchy();
        }

        return success;
    }

    void traverse_depth_first(const ProcessCallTree& process_graph,
                              std::uint64_t node_id,
                              std::vector<CallTreeNodeInfo>& nodes) const {
        auto it = process_graph.calls.find(node_id);
        if (it == process_graph.calls.end()) {
            return;
        }

        const auto& node = it->second;

        // Add current node
        CallTreeNodeInfo info;
        info.id = node->get_id();
        info.name = node->get_name();
        info.category = node->get_category();
        info.start_time_us = node->get_start_time();
        info.duration_us = node->get_duration();
        info.level = node->get_level();
        info.parent_id = node->get_parent_id();
        info.num_children = node->get_children().size();
        info.children_ids = node->get_children();
        info.args = node->get_args();

        nodes.push_back(info);

        // Recursively traverse children
        for (std::uint64_t child_id : node->get_children()) {
            traverse_depth_first(process_graph, child_id, nodes);
        }
    }

    void print_node_recursive(const ProcessCallTree& process_graph,
                              std::uint64_t node_id, int indent, int max_depth,
                              std::ostream& out) const {
        if (max_depth > 0 && indent >= max_depth) {
            return;
        }

        auto it = process_graph.calls.find(node_id);
        if (it == process_graph.calls.end()) {
            return;
        }

        const auto& node = it->second;

        // Print indentation
        for (int i = 0; i < indent; i++) {
            out << "  ";
        }

        // Print node info
        out << node->get_name() << " [" << node->get_category() << "] "
            << "level=" << node->get_level() << " "
            << "dur=" << (static_cast<double>(node->get_duration()) / 1000.0)
            << "ms "
            << "children=" << node->get_children().size() << "\n";

        // Print children
        for (std::uint64_t child_id : node->get_children()) {
            print_node_recursive(process_graph, child_id, indent + 1, max_depth,
                                 out);
        }
    }

    void print_node_recursive_stdio(const ProcessCallTree& process_graph,
                                    std::uint64_t node_id, int indent,
                                    int max_depth) const {
        if (max_depth > 0 && indent >= max_depth) {
            return;
        }

        auto it = process_graph.calls.find(node_id);
        if (it == process_graph.calls.end()) {
            return;
        }

        const auto& node = it->second;

        // Print indentation
        for (int i = 0; i < indent; i++) {
            printf("  ");
        }

        // Print node info
        printf("%s [%s] level=%d dur=%.3fms children=%zu\n",
               node->get_name().c_str(), node->get_category().c_str(),
               node->get_level(),
               static_cast<double>(node->get_duration()) / 1000.0,
               node->get_children().size());

        // Print children
        for (std::uint64_t child_id : node->get_children()) {
            print_node_recursive_stdio(process_graph, child_id, indent + 1,
                                       max_depth);
        }
    }

    void compute_level_stats(const ProcessCallTree& process_graph,
                             std::uint64_t node_id,
                             std::vector<std::uint64_t>& total_time_per_level,
                             std::vector<size_t>& count_per_level,
                             int& max_level, size_t& leaf_count) const {
        auto it = process_graph.calls.find(node_id);
        if (it == process_graph.calls.end()) {
            return;
        }

        const auto& node = it->second;
        int level = node->get_level();

        if (level >= static_cast<int>(total_time_per_level.size())) {
            total_time_per_level.resize(level + 1, 0);
            count_per_level.resize(level + 1, 0);
        }

        total_time_per_level[level] += node->get_duration();
        count_per_level[level]++;

        if (level > max_level) {
            max_level = level;
        }

        if (node->get_children().empty()) {
            leaf_count++;
        }

        for (std::uint64_t child_id : node->get_children()) {
            compute_level_stats(process_graph, child_id, total_time_per_level,
                                count_per_level, max_level, leaf_count);
        }
    }
};

}  // namespace internal

// ============================================================================
// CallTree Public API Implementation
// ============================================================================

CallTree::CallTree() : impl_(std::make_unique<internal::CallTreeImpl>()) {}

CallTree::~CallTree() = default;

CallTree::CallTree(CallTree&&) noexcept = default;
CallTree& CallTree::operator=(CallTree&&) noexcept = default;

bool CallTree::load_from_directory(const std::string& trace_dir,
                                   const std::string& pattern) {
    bool found = impl_->find_trace_files(trace_dir, pattern);

    if (found) {
        DFTRACER_UTILS_LOG_INFO("Found %zu trace files in %s",
                                impl_->trace_files.size(), trace_dir.c_str());

        // Set default output path
        fs::path dir_path(trace_dir);
        std::string dir_name = dir_path.filename().string();
        impl_->output_path = dir_name + ".calltree";
    }

    return found;
}

bool CallTree::generate() {
    if (impl_->trace_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s", "No trace files loaded. Call load_from_directory() first.");
        return false;
    }

    DFTRACER_UTILS_LOG_INFO("Generating call tree from %zu trace files...",
                            impl_->trace_files.size());

    bool success = impl_->load_traces();

    if (success) {
        impl_->is_generated = true;
        DFTRACER_UTILS_LOG_INFO("%s", "Call tree generation complete");
        DFTRACER_UTILS_LOG_INFO("  Total processes: %zu", impl_->graph.size());
    } else {
        DFTRACER_UTILS_LOG_ERROR("%s", "Failed to generate call tree");
    }

    return success;
}

void CallTree::print_depth_first(int max_depth) const {
    if (!impl_->is_generated) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s", "Call tree not generated. Call generate() first.");
        return;
    }

    auto keys = impl_->graph.keys();

    for (const auto& key : keys) {
        auto* process_graph = impl_->graph.get(key);
        if (!process_graph) continue;

        DFTRACER_UTILS_LOG_INFO(
            "\n=== Process/Thread: PID=%u, TID=%u, Node=%u ===", key.pid,
            key.tid, key.node_id);
        DFTRACER_UTILS_LOG_INFO("Total nodes: %zu",
                                process_graph->calls.size());
        DFTRACER_UTILS_LOG_INFO("Root calls: %zu",
                                process_graph->root_calls.size());
        DFTRACER_UTILS_LOG_INFO("%s", "");

        for (std::uint64_t root_id : process_graph->root_calls) {
            impl_->print_node_recursive_stdio(*process_graph, root_id, 0,
                                              max_depth);
        }
    }
}

bool CallTree::print_depth_first_to_file(const std::string& filename,
                                         int max_depth) const {
    if (!impl_->is_generated) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s", "Call tree not generated. Call generate() first.");
        return false;
    }

    std::ofstream file(filename);
    if (!file.is_open()) {
        DFTRACER_UTILS_LOG_ERROR("Cannot open file for writing: %s",
                                 filename.c_str());
        return false;
    }

    auto keys = impl_->graph.keys();

    for (const auto& key : keys) {
        auto* process_graph = impl_->graph.get(key);
        if (!process_graph) continue;

        file << "\n=== Process/Thread: PID=" << key.pid << ", TID=" << key.tid
             << ", Node=" << key.node_id << " ===" << std::endl;
        file << "Total nodes: " << process_graph->calls.size() << std::endl;
        file << "Root calls: " << process_graph->root_calls.size() << std::endl;
        file << std::endl;

        for (std::uint64_t root_id : process_graph->root_calls) {
            impl_->print_node_recursive(*process_graph, root_id, 0, max_depth,
                                        file);
        }
    }

    file.close();
    DFTRACER_UTILS_LOG_INFO("Call tree printed to: %s", filename.c_str());
    return true;
}

std::vector<CallTreeNodeInfo> CallTree::get_nodes_depth_first() const {
    std::vector<CallTreeNodeInfo> all_nodes;

    if (!impl_->is_generated) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s", "Call tree not generated. Call generate() first.");
        return all_nodes;
    }

    auto keys = impl_->graph.keys();

    for (const auto& key : keys) {
        auto* process_graph = impl_->graph.get(key);
        if (!process_graph) continue;

        for (std::uint64_t root_id : process_graph->root_calls) {
            impl_->traverse_depth_first(*process_graph, root_id, all_nodes);
        }
    }

    return all_nodes;
}

std::string CallTree::get_output_path() const { return impl_->output_path; }

void CallTree::set_output_path(const std::string& path) {
    impl_->output_path = path;
}

bool CallTree::save_to_file(const std::string& filename) const {
    if (!impl_->is_generated) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s", "Call tree not generated. Call generate() first.");
        return false;
    }

    std::string output_file = filename.empty() ? impl_->output_path : filename;

    std::ofstream file(output_file, std::ios::binary);
    if (!file.is_open()) {
        DFTRACER_UTILS_LOG_ERROR("Cannot open file for writing: %s",
                                 output_file.c_str());
        return false;
    }

    // Write header
    const char magic[8] = {'C', 'A', 'L', 'L', 'T', 'R', 'E', 'E'};
    file.write(magic, 8);

    std::uint32_t version = 1;
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));

    // Get nodes in depth-first order
    auto nodes = get_nodes_depth_first();

    std::uint64_t num_nodes = nodes.size();
    file.write(reinterpret_cast<const char*>(&num_nodes), sizeof(num_nodes));

    // Write each node
    for (const auto& node : nodes) {
        file.write(reinterpret_cast<const char*>(&node.id), sizeof(node.id));

        std::uint32_t name_len = static_cast<std::uint32_t>(node.name.size());
        file.write(reinterpret_cast<const char*>(&name_len), sizeof(name_len));
        file.write(node.name.data(), name_len);

        std::uint32_t cat_len =
            static_cast<std::uint32_t>(node.category.size());
        file.write(reinterpret_cast<const char*>(&cat_len), sizeof(cat_len));
        file.write(node.category.data(), cat_len);

        file.write(reinterpret_cast<const char*>(&node.start_time_us),
                   sizeof(node.start_time_us));
        file.write(reinterpret_cast<const char*>(&node.duration_us),
                   sizeof(node.duration_us));
        file.write(reinterpret_cast<const char*>(&node.level),
                   sizeof(node.level));
        file.write(reinterpret_cast<const char*>(&node.parent_id),
                   sizeof(node.parent_id));

        std::uint64_t num_children = node.num_children;
        file.write(reinterpret_cast<const char*>(&num_children),
                   sizeof(num_children));
    }

    file.close();
    DFTRACER_UTILS_LOG_INFO("Call tree saved to: %s", output_file.c_str());
    DFTRACER_UTILS_LOG_INFO("  Nodes written: %zu", nodes.size());

    return true;
}

bool CallTree::save_to_json(const std::string& filename) const {
    if (!impl_->is_generated) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s", "Call tree not generated. Call generate() first.");
        return false;
    }

    // Determine output file - use .pfw extension for compatibility with
    // DFTracer tools
    std::string output_file = filename;
    if (output_file.empty()) {
        // Replace .calltree extension with .pfw if present, otherwise append
        std::string base = impl_->output_path;
        if (base.size() >= 9 && base.substr(base.size() - 9) == ".calltree") {
            base = base.substr(0, base.size() - 9);
        }
        output_file = base + ".pfw";
    }

    std::ofstream file(output_file);
    if (!file.is_open()) {
        DFTRACER_UTILS_LOG_ERROR("Cannot open file for writing: %s",
                                 output_file.c_str());
        return false;
    }

    DFTRACER_UTILS_LOG_INFO(
        "%s", "Serializing call tree to JSON (Chrome Tracing format)...");

    // Create JSON serializer
    internal::JsonSerializer serializer;

    // Buffer for serialization (16KB should be enough for most events)
    const size_t BUFFER_SIZE = 16384;
    char buffer[BUFFER_SIZE];

    // Get hostname for identification
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    std::string hostname_hash = std::string(hostname);

    // Write opening bracket
    size_t written = serializer.initialize(buffer, hostname_hash);
    file.write(buffer, written);

    // Write metadata events for file header
    std::time_t now = std::time(nullptr);
    char timestamp[256];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
                  std::localtime(&now));

    written = serializer.serialize_metadata(buffer, "timestamp", timestamp, "M",
                                            0, 0, true);
    file.write(buffer, written - 1);  // Don't write the newline yet
    file.write(",\n", 2);             // Write comma separator

    written = serializer.serialize_metadata(buffer, "format", "call_tree", "M",
                                            0, 0, true);
    file.write(buffer, written - 1);
    file.write(",\n", 2);

    // Get all process keys
    auto keys = impl_->graph.keys();

    // Track event index (similar to DFTracer)
    int event_index = 0;
    size_t total_events = 0;

    // Iterate over all processes/threads
    for (const auto& key : keys) {
        auto* process_graph = impl_->graph.get(key);
        if (!process_graph) continue;

        // Traverse and serialize nodes in depth-first order
        for (std::uint64_t root_id : process_graph->root_calls) {
            std::vector<std::uint64_t> stack;
            stack.push_back(root_id);

            while (!stack.empty()) {
                std::uint64_t node_id = stack.back();
                stack.pop_back();

                auto it = process_graph->calls.find(node_id);
                if (it == process_graph->calls.end()) continue;

                const auto& node = it->second;

                // Serialize this node
                written = serializer.serialize_node(buffer, event_index++,
                                                    *node, key.pid, key.tid);

                // Write to file with comma separator (except last event)
                file.write(buffer, written - 1);  // Don't write newline

                // Add children to stack in reverse order for depth-first
                const auto& children = node->get_children();
                for (auto child_it = children.rbegin();
                     child_it != children.rend(); ++child_it) {
                    stack.push_back(*child_it);
                }

                // Write comma separator for next event
                file.write(",\n", 2);
                total_events++;
            }
        }
    }

    // Write closing bracket (overwrites the last comma)
    file.seekp(-2, std::ios::cur);  // Back up over ",\n"
    file.write("\n", 1);            // Just write newline

    written = serializer.finalize(buffer, true);
    file.write(buffer, written);

    file.close();

    DFTRACER_UTILS_LOG_INFO("Call tree saved to JSON: %s", output_file.c_str());
    DFTRACER_UTILS_LOG_INFO("  Total events: %zu", total_events);
    DFTRACER_UTILS_LOG_INFO("  Unique processes: %zu", keys.size());
    DFTRACER_UTILS_LOG_INFO(
        "%s", "  Format: Chrome Tracing (compatible with Perfetto)");

    return true;
}

bool CallTree::load_from_file(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        DFTRACER_UTILS_LOG_ERROR("Cannot open file for reading: %s",
                                 filename.c_str());
        return false;
    }

    // Read and verify header
    char magic[8];
    file.read(magic, 8);

    if (std::memcmp(magic, "CALLTREE", 8) != 0) {
        DFTRACER_UTILS_LOG_ERROR("%s", "Invalid file format");
        return false;
    }

    std::uint32_t version;
    file.read(reinterpret_cast<char*>(&version), sizeof(version));

    if (version != 1) {
        DFTRACER_UTILS_LOG_ERROR("Unsupported version: %u", version);
        return false;
    }

    std::uint64_t num_nodes;
    file.read(reinterpret_cast<char*>(&num_nodes), sizeof(num_nodes));

    DFTRACER_UTILS_LOG_INFO("Loading %lu nodes from %s",
                            (unsigned long)num_nodes, filename.c_str());

    // Note: This is a simplified load that just verifies the file
    // Full reconstruction would require rebuilding the CallTree structure

    file.close();
    DFTRACER_UTILS_LOG_INFO("%s", "Call tree file validated successfully");

    return true;
}

CallTreeStats CallTree::get_statistics() const {
    CallTreeStats stats;

    if (!impl_->is_generated) {
        return stats;
    }

    auto keys = impl_->graph.keys();
    stats.num_processes = keys.size();

    // Use set to count unique process+thread combinations
    std::set<std::pair<std::uint32_t, std::uint32_t>> unique_process_threads;
    for (const auto& key : keys) {
        unique_process_threads.insert({key.pid, key.tid});
    }
    stats.num_processes = unique_process_threads.size();

    std::vector<std::uint64_t> total_time_per_level;
    std::vector<size_t> count_per_level;
    int max_level = 0;
    size_t total_leaf_count = 0;
    size_t total_node_count = 0;

    for (const auto& key : keys) {
        auto* process_graph = impl_->graph.get(key);
        if (!process_graph) continue;

        total_node_count += process_graph->calls.size();

        for (std::uint64_t root_id : process_graph->root_calls) {
            impl_->compute_level_stats(*process_graph, root_id,
                                       total_time_per_level, count_per_level,
                                       max_level, total_leaf_count);
        }
    }

    stats.total_nodes = total_node_count;
    stats.num_levels = max_level + 1;
    stats.num_leaf_nodes = total_leaf_count;
    stats.max_depth = max_level;
    stats.unique_processes = stats.num_processes;

    // Compute average time per level
    stats.avg_time_per_level_us.resize(stats.num_levels);
    stats.nodes_per_level.resize(stats.num_levels);

    for (size_t i = 0; i < stats.num_levels; i++) {
        stats.nodes_per_level[i] = count_per_level[i];
        if (count_per_level[i] > 0) {
            stats.avg_time_per_level_us[i] =
                static_cast<double>(total_time_per_level[i]) /
                static_cast<double>(count_per_level[i]);
        } else {
            stats.avg_time_per_level_us[i] = 0.0;
        }
    }

    return stats;
}

void CallTree::print_statistics() const {
    auto stats = get_statistics();

    DFTRACER_UTILS_LOG_INFO("%s",
                            "\n============ Call Tree Statistics ============");
    DFTRACER_UTILS_LOG_INFO("Total nodes:           %zu", stats.total_nodes);
    DFTRACER_UTILS_LOG_INFO("Number of levels:      %zu", stats.num_levels);
    DFTRACER_UTILS_LOG_INFO("Leaf nodes:            %zu", stats.num_leaf_nodes);
    DFTRACER_UTILS_LOG_INFO("Unique processes:      %zu", stats.num_processes);
    DFTRACER_UTILS_LOG_INFO("%s", "");

    DFTRACER_UTILS_LOG_INFO("%s", "Per-Level Statistics:");
    DFTRACER_UTILS_LOG_INFO("%8s%12s%20s", "Level", "Nodes", "Avg Time (ms)");
    DFTRACER_UTILS_LOG_INFO("%s", "----------------------------------------");

    for (size_t i = 0; i < stats.num_levels; i++) {
        DFTRACER_UTILS_LOG_INFO("%8zu%12zu%20.3f", i, stats.nodes_per_level[i],
                                stats.avg_time_per_level_us[i] / 1000.0);
    }

    DFTRACER_UTILS_LOG_INFO("%s",
                            "=============================================\n");
}

bool CallTree::is_generated() const { return impl_->is_generated; }

size_t CallTree::get_num_trace_files() const {
    return impl_->trace_files.size();
}

void CallTree::clear() {
    impl_->graph.cleanup();
    impl_->graph.initialize();
    impl_->trace_files.clear();
    impl_->trace_directory.clear();
    impl_->output_path.clear();
    impl_->is_generated = false;
}

std::vector<std::uint32_t> CallTree::get_process_ids() const {
    std::vector<std::uint32_t> process_ids;

    if (!impl_->is_generated) {
        return process_ids;
    }

    auto keys = impl_->graph.keys();
    for (const auto& key : keys) {
        // Only add unique PIDs (keys may have duplicates with different TIDs)
        if (std::find(process_ids.begin(), process_ids.end(), key.pid) ==
            process_ids.end()) {
            process_ids.push_back(key.pid);
        }
    }

    return process_ids;
}

std::vector<std::uint32_t> CallTree::get_thread_ids(std::uint32_t pid) const {
    std::vector<std::uint32_t> thread_ids;

    if (!impl_->is_generated) {
        return thread_ids;
    }

    auto keys = impl_->graph.keys();
    for (const auto& key : keys) {
        if (key.pid == pid) {
            thread_ids.push_back(key.tid);
        }
    }

    return thread_ids;
}

std::vector<CallTreeNodeInfo> CallTree::get_root_nodes(
    std::uint32_t pid, std::uint32_t tid) const {
    std::vector<CallTreeNodeInfo> root_nodes;

    if (!impl_->is_generated) {
        return root_nodes;
    }

    internal::ProcessKey key{pid, tid};
    auto* process_graph = impl_->graph.get(key);

    if (!process_graph) {
        return root_nodes;
    }

    // Convert root node IDs to CallTreeNodeInfo structures
    for (std::uint64_t root_id : process_graph->root_calls) {
        auto it = process_graph->calls.find(root_id);
        if (it != process_graph->calls.end()) {
            const auto& node = it->second;

            CallTreeNodeInfo info;
            info.id = node->get_id();
            info.name = node->get_name();
            info.category = node->get_category();
            info.start_time_us = node->get_start_time();
            info.duration_us = node->get_duration();
            info.level = node->get_level();
            info.parent_id = node->get_parent_id();
            info.num_children = node->get_children().size();
            info.children_ids = node->get_children();
            info.args = node->get_args();

            root_nodes.push_back(info);
        }
    }

    return root_nodes;
}

std::vector<CallTreeNodeInfo> CallTree::get_all_nodes() const {
    return get_nodes_depth_first();
}

CallTreeNodeInfo CallTree::get_node_by_id(std::uint64_t id) const {
    CallTreeNodeInfo empty_node;

    if (!impl_->is_generated) {
        return empty_node;
    }

    // Search through all process graphs for the node with this ID
    auto keys = impl_->graph.keys();
    for (const auto& key : keys) {
        auto* process_graph = impl_->graph.get(key);
        if (!process_graph) continue;

        auto it = process_graph->calls.find(id);
        if (it != process_graph->calls.end()) {
            const auto& node = it->second;

            CallTreeNodeInfo info;
            info.id = node->get_id();
            info.name = node->get_name();
            info.category = node->get_category();
            info.start_time_us = node->get_start_time();
            info.duration_us = node->get_duration();
            info.level = node->get_level();
            info.parent_id = node->get_parent_id();
            info.num_children = node->get_children().size();
            info.children_ids = node->get_children();
            info.args = node->get_args();

            return info;
        }
    }

    return empty_node;
}

}  // namespace dftracer::utils::call_tree
