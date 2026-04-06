#include <dftracer/utils/call_tree/internal/call_tree.h>
#include <dftracer/utils/call_tree/internal/factory.h>
#include <dftracer/utils/call_tree/internal/node.h>
#include <dftracer/utils/call_tree/internal/process_call_tree.h>
#include <dftracer/utils/call_tree/internal/process_key.h>
#include <dftracer/utils/call_tree/internal/trace_reader.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/format_detector.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/reader/internal/line_processor.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <yyjson.h>
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>

namespace dftracer::utils::call_tree {
namespace internal {

// ============================================================================
// CallTreeNode Implementation
// ============================================================================

CallTreeNode::CallTreeNode()
    : id_(0),
      name_(),
      category_(),
      start_time_(0),
      duration_(0),
      level_(0),
      parent_id_(0),
      args_(),
      children_(),
      initialized_(false),
      cleaned_up_(false) {}

CallTreeNode::CallTreeNode(std::uint64_t id, const std::string& name,
                           const std::string& category)
    : id_(id),
      name_(name),
      category_(category),
      start_time_(0),
      duration_(0),
      level_(0),
      parent_id_(0),
      args_(),
      children_(),
      initialized_(false),
      cleaned_up_(false) {}

CallTreeNode::~CallTreeNode() {
    if (!cleaned_up_) {
        cleanup();
    }
    // Clear all state
    id_ = 0;
    name_.clear();
    category_.clear();
    start_time_ = 0;
    duration_ = 0;
    level_ = 0;
    parent_id_ = 0;
    args_.clear();
    children_.clear();
    initialized_ = false;
    cleaned_up_ = true;
}

CallTreeNode::CallTreeNode(CallTreeNode&& other) noexcept
    : id_(other.id_),
      name_(std::move(other.name_)),
      category_(std::move(other.category_)),
      start_time_(other.start_time_),
      duration_(other.duration_),
      level_(other.level_),
      parent_id_(other.parent_id_),
      args_(std::move(other.args_)),
      children_(std::move(other.children_)),
      initialized_(other.initialized_),
      cleaned_up_(other.cleaned_up_) {
    // Reset other
    other.id_ = 0;
    other.start_time_ = 0;
    other.duration_ = 0;
    other.level_ = 0;
    other.parent_id_ = 0;
    other.initialized_ = false;
    other.cleaned_up_ = true;
}

CallTreeNode& CallTreeNode::operator=(CallTreeNode&& other) noexcept {
    if (this != &other) {
        // Clean up current resources
        if (!cleaned_up_) {
            cleanup();
        }

        // Move from other
        id_ = other.id_;
        name_ = std::move(other.name_);
        category_ = std::move(other.category_);
        start_time_ = other.start_time_;
        duration_ = other.duration_;
        level_ = other.level_;
        parent_id_ = other.parent_id_;
        args_ = std::move(other.args_);
        children_ = std::move(other.children_);
        initialized_ = other.initialized_;
        cleaned_up_ = other.cleaned_up_;

        // Reset other
        other.id_ = 0;
        other.start_time_ = 0;
        other.duration_ = 0;
        other.level_ = 0;
        other.parent_id_ = 0;
        other.initialized_ = false;
        other.cleaned_up_ = true;
    }
    return *this;
}

void CallTreeNode::initialize(std::uint64_t id, const std::string& name,
                              const std::string& category,
                              std::uint64_t start_time, std::uint64_t duration,
                              int level) {
    id_ = id;
    name_ = name;
    category_ = category;
    start_time_ = start_time;
    duration_ = duration;
    level_ = level;
    parent_id_ = 0;
    args_.clear();
    children_.clear();
    initialized_ = true;
    cleaned_up_ = false;
}

void CallTreeNode::cleanup() {
    if (cleaned_up_) {
        return;
    }

    // Clear containers to free memory
    args_.clear();
    children_.clear();
    name_.clear();
    category_.clear();

    cleaned_up_ = true;
}

// ============================================================================
// CallTreeFactory Implementation
// ============================================================================

CallTreeFactory::CallTreeFactory()
    : node_count_(0),
      initialized_(false),
      cleaned_up_(false),
      managed_nodes_() {}

CallTreeFactory::~CallTreeFactory() {
    if (!cleaned_up_) {
        cleanup();
    }
    node_count_ = 0;
    initialized_ = false;
    cleaned_up_ = true;
    managed_nodes_.clear();
}

void CallTreeFactory::initialize() {
    node_count_ = 0;
    managed_nodes_.clear();
    initialized_ = true;
    cleaned_up_ = false;
}

void CallTreeFactory::cleanup() {
    if (cleaned_up_) {
        return;
    }

    // Clean up all managed nodes
    for (auto& node : managed_nodes_) {
        if (node) {
            node->cleanup();
        }
    }
    managed_nodes_.clear();
    node_count_ = 0;
    cleaned_up_ = true;
}

std::shared_ptr<CallTreeNode> CallTreeFactory::create_node(
    std::uint64_t id, const std::string& name, const std::string& category,
    std::uint64_t start_time, std::uint64_t duration, int level,
    const std::unordered_map<std::string, std::string>& args) {
    auto node = std::make_shared<CallTreeNode>(id, name, category);
    node->initialize(id, name, category, start_time, duration, level);
    node->set_args(args);

    // Track the node for cleanup
    managed_nodes_.push_back(node);
    node_count_++;

    return node;
}

// ============================================================================
// TraceLineProcessor - LineProcessor for parsing trace events
// ============================================================================

class TraceLineProcessor
    : public dftracer::utils::utilities::reader::internal::LineProcessor {
   public:
    TraceLineProcessor(TraceReader& reader, CallTree& graph)
        : reader_(reader),
          graph_(graph),
          line_count_(0),
          processed_(0),
          report_interval_(10000) {}

    coro::CoroTask<bool> process(const char* data,
                                 std::size_t length) override {
        line_count_++;

        // Progress indicator
        if (line_count_ % report_interval_ == 0) {
            DFTRACER_UTILS_LOG_DEBUG("  processed %zu lines, %zu traces...",
                                     line_count_, processed_);
        }

        // Skip empty lines, brackets
        if (length == 0) {
            co_return true;
        }

        std::string line(data, length);

        // Skip brackets
        if (line == "[" || line == "]") {
            co_return true;
        }

        // Remove trailing comma
        if (!line.empty() && line.back() == ',') {
            line.pop_back();
        }

        if (reader_.process_trace_line(line, graph_)) {
            processed_++;
        }

        co_return true;  // Continue processing
    }

    void end() override {
        DFTRACER_UTILS_LOG_INFO(
            "processed %zu trace entries from %zu total lines", processed_,
            line_count_);
    }

    std::size_t get_processed_count() const { return processed_; }

   private:
    TraceReader& reader_;
    CallTree& graph_;
    std::size_t line_count_;
    std::size_t processed_;
    std::size_t report_interval_;
};

// ============================================================================
// TraceReader Implementation
// ============================================================================

bool TraceReader::read(const std::string& trace_file, CallTree& graph) {
    DFTRACER_UTILS_LOG_INFO("reading trace file: %s", trace_file.c_str());

    // Try to use Reader API first (for compressed files, tar.gz, etc.)
    if (read_with_reader(trace_file, graph)) {
        return true;
    }

    // Fallback to direct reading for plain text files
    return read_direct(trace_file, graph);
}

bool TraceReader::read_with_reader(const std::string& trace_file,
                                   CallTree& graph) {
    try {
        // Detect file format
        auto format = dftracer::utils::FormatDetector::detect(trace_file);

        // For GZIP files, skip Reader API and use direct zlib decompression
        // since this path expects a prebuilt `.dftindex` store.
        if (format == dftracer::utils::ArchiveFormat::GZIP) {
            return false;  // Will trigger fallback to read_direct which handles
                           // gzip
        }

        // Check if format is supported by Reader
        if (!dftracer::utils::utilities::reader::internal::ReaderFactory::
                is_format_supported(format)) {
            // Not supported, will use fallback
            return false;
        }

        std::string index_path = dftracer::utils::utilities::composites::dft::
            internal::determine_index_path(trace_file, "");

        // Create reader (this will auto-build index if needed)
        auto reader =
            dftracer::utils::utilities::reader::internal::ReaderFactory::create(
                trace_file, index_path);
        if (!reader || !reader->is_valid()) {
            DFTRACER_UTILS_LOG_ERROR("Failed to create reader for %s",
                                     trace_file.c_str());
            return false;
        }

        DFTRACER_UTILS_LOG_INFO("Using Reader API for %s (format: %s)",
                                trace_file.c_str(),
                                reader->get_format_name().c_str());

        // Create line processor
        TraceLineProcessor processor(*this, graph);

        // Read all lines using line processor
        std::size_t num_lines = reader->get_num_lines();
        if (num_lines > 0) {
            reader->read_lines_with_processor(1, num_lines, processor);
        }

        return true;

    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Reader API failed for %s: %s",
                                 trace_file.c_str(), e.what());
        return false;
    }
}

bool TraceReader::read_direct(const std::string& trace_file, CallTree& graph) {
    // Detect file format to see if we need decompression
    ArchiveFormat format = FormatDetector::detect(trace_file);

    // Handle gzip files with zlib
    if (format == ArchiveFormat::GZIP) {
        DFTRACER_UTILS_LOG_INFO("Using zlib decompression for %s",
                                trace_file.c_str());

        gzFile gz = gzopen(trace_file.c_str(), "rb");
        if (!gz) {
            DFTRACER_UTILS_LOG_ERROR("Cannot open gzip file: %s",
                                     trace_file.c_str());
            return false;
        }

        char buffer[65536];
        std::string current_line;
        size_t line_count = 0;
        size_t processed = 0;
        size_t report_interval = 10000;

        while (true) {
            int bytes_read = gzread(gz, buffer, sizeof(buffer) - 1);
            if (bytes_read <= 0) {
                // Process any remaining line
                if (!current_line.empty()) {
                    line_count++;
                    if (!current_line.empty() && current_line != "[" &&
                        current_line != "]") {
                        if (current_line.back() == ',') current_line.pop_back();
                        if (process_trace_line(current_line, graph)) {
                            processed++;
                        }
                    }
                }
                break;
            }

            buffer[bytes_read] = '\0';
            current_line += buffer;

            // Process complete lines
            size_t pos;
            while ((pos = current_line.find('\n')) != std::string::npos) {
                std::string line = current_line.substr(0, pos);
                current_line = current_line.substr(pos + 1);
                line_count++;

                if (line_count % report_interval == 0) {
                    DFTRACER_UTILS_LOG_DEBUG(
                        "  processed %zu lines, %zu traces...", line_count,
                        processed);
                }

                if (line.empty() || line == "[" || line == "]") continue;
                if (!line.empty() && line.back() == ',') line.pop_back();

                if (process_trace_line(line, graph)) {
                    processed++;
                }
            }
        }

        gzclose(gz);
        DFTRACER_UTILS_LOG_INFO("processed %zu trace entries from %zu lines",
                                processed, line_count);
        return true;
    }

    // Handle tar.gz - not supported without indexer
    if (format == ArchiveFormat::TAR_GZ) {
        DFTRACER_UTILS_LOG_ERROR("Cannot read tar.gz file without index: %s",
                                 trace_file.c_str());
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "Please create an index using dftracer_map");
        return false;
    }

    // Plain text file
    DFTRACER_UTILS_LOG_INFO("Using direct file reading for %s",
                            trace_file.c_str());

    std::ifstream file(trace_file);
    if (!file.is_open()) {
        DFTRACER_UTILS_LOG_ERROR("cant open trace file: %s",
                                 trace_file.c_str());
        return false;
    }

    std::string line;
    size_t line_count = 0;
    size_t processed = 0;
    size_t report_interval = 10000;

    while (std::getline(file, line)) {
        line_count++;

        // progress indicator
        if (line_count % report_interval == 0) {
            DFTRACER_UTILS_LOG_DEBUG("  processed %zu lines, %zu traces...",
                                     line_count, processed);
        }

        // skip brackets and empty lines
        if (line.empty() || line == "[" || line == "]") {
            continue;
        }

        // remove trailing comma
        if (!line.empty() && line.back() == ',') {
            line.pop_back();
        }

        if (process_trace_line(line, graph)) {
            processed++;
        } else {
            // Don't spam errors for metadata entries
            if (line_count < 10) {
                DFTRACER_UTILS_LOG_ERROR("failed to parse line %zu in %s",
                                         line_count, trace_file.c_str());
            }
        }
    }

    DFTRACER_UTILS_LOG_INFO("processed %zu trace entries from %s", processed,
                            trace_file.c_str());

    return true;
}

bool TraceReader::read_multiple(const std::vector<std::string>& trace_files,
                                CallTree& graph) {
    bool all_success = true;

    DFTRACER_UTILS_LOG_INFO("reading %zu trace files...", trace_files.size());

    size_t file_num = 0;
    (void)file_num;
    for (const auto& file : trace_files) {
        file_num++;
        DFTRACER_UTILS_LOG_DEBUG("[%zu/%zu] ", file_num, trace_files.size());
        if (!read(file, graph)) {
            DFTRACER_UTILS_LOG_ERROR("failed to read: %s", file.c_str());
            all_success = false;
        }
    }

    // build parent child relationships after all traces loaded
    DFTRACER_UTILS_LOG_INFO(
        "building call hierarchy for %zu process/thread/node combinations...",
        graph.size());
    graph.build_hierarchy();

    return all_success;
}

bool TraceReader::read_directory(const std::string& directory,
                                 const std::string& pattern, CallTree& graph) {
    if (!fs::exists(directory) || !fs::is_directory(directory)) {
        DFTRACER_UTILS_LOG_ERROR("directory does not exist: %s",
                                 directory.c_str());
        return false;
    }

    std::vector<std::string> trace_files;

    // collect all matching files
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();

            // simple pattern matching (for now, just check file extension)
            if (pattern == "*" ||
                filename.find(pattern.substr(1)) != std::string::npos) {
                trace_files.push_back(entry.path().string());
            }
        }
    }

    if (trace_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("no trace files found in %s matching %s",
                                 directory.c_str(), pattern.c_str());
        return false;
    }

    // sort files for consistent processing order
    std::sort(trace_files.begin(), trace_files.end());

    DFTRACER_UTILS_LOG_INFO("found %zu trace files in %s", trace_files.size(),
                            directory.c_str());

    return read_multiple(trace_files, graph);
}

bool TraceReader::process_trace_line(const std::string& line, CallTree& graph) {
    yyjson_doc* doc = yyjson_read(line.c_str(), line.length(), 0);
    if (!doc) {
        return false;
    }

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root) {
        yyjson_doc_free(doc);
        return false;
    }

    // get basic fields
    yyjson_val* id_val = yyjson_obj_get(root, "id");
    yyjson_val* name_val = yyjson_obj_get(root, "name");
    yyjson_val* cat_val = yyjson_obj_get(root, "cat");
    yyjson_val* pid_val = yyjson_obj_get(root, "pid");
    yyjson_val* ph_val = yyjson_obj_get(root, "ph");
    yyjson_val* ts_val = yyjson_obj_get(root, "ts");
    yyjson_val* dur_val = yyjson_obj_get(root, "dur");
    yyjson_val* args_val = yyjson_obj_get(root, "args");

    // skip metadata entries
    if (!ph_val || !yyjson_is_str(ph_val) ||
        strcmp(yyjson_get_str(ph_val), "X") != 0) {
        yyjson_doc_free(doc);
        return true;  // not an error just skip
    }

    if (!id_val || !name_val || !pid_val || !ts_val) {
        yyjson_doc_free(doc);
        return false;
    }

    std::uint64_t call_id = yyjson_get_uint(id_val);
    std::uint64_t pid = yyjson_get_uint(pid_val);
    std::string name = yyjson_get_str(name_val);
    std::string category = cat_val ? yyjson_get_str(cat_val) : "";
    std::uint64_t start_time = yyjson_get_uint(ts_val);
    std::uint64_t duration = dur_val ? yyjson_get_uint(dur_val) : 0;

    // get level, tid, and node_id from args
    int level = 0;
    std::uint32_t tid = 0;
    std::uint32_t node_id = 0;

    // Collect all args
    std::unordered_map<std::string, std::string> args;

    if (args_val && yyjson_is_obj(args_val)) {
        yyjson_val* level_val = yyjson_obj_get(args_val, "level");
        if (level_val) {
            level = yyjson_get_int(level_val);
        }

        yyjson_val* tid_val = yyjson_obj_get(args_val, "tid");
        if (tid_val) {
            tid = static_cast<std::uint32_t>(yyjson_get_uint(tid_val));
        }

        yyjson_val* node_val = yyjson_obj_get(args_val, "node_id");
        if (node_val) {
            node_id = static_cast<std::uint32_t>(yyjson_get_uint(node_val));
        }

        // Store all args
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(args_val, &iter);
        yyjson_val *arg_key, *arg_val;
        while ((arg_key = yyjson_obj_iter_next(&iter))) {
            arg_val = yyjson_obj_iter_get_val(arg_key);
            if (yyjson_is_str(arg_val)) {
                args[yyjson_get_str(arg_key)] = yyjson_get_str(arg_val);
            } else if (yyjson_is_int(arg_val)) {
                args[yyjson_get_str(arg_key)] =
                    std::to_string(yyjson_get_int(arg_val));
            } else if (yyjson_is_uint(arg_val)) {
                args[yyjson_get_str(arg_key)] =
                    std::to_string(yyjson_get_uint(arg_val));
            }
        }
    }

    // Create function call using factory
    ProcessKey key(static_cast<std::uint32_t>(pid), tid, node_id);
    auto call = graph.get_factory().create_node(
        call_id, name, category, start_time, duration, level, args);

    // Add call to graph
    graph.add_call(key, call);

    yyjson_doc_free(doc);
    return true;
}

// ============================================================================
// CallTree Implementation
// ============================================================================

CallTree::CallTree()
    : process_graphs_(),
      factory_(),
      log_file_(),
      initialized_(false),
      cleaned_up_(false) {}

CallTree::CallTree(const std::string& log_file)
    : process_graphs_(),
      factory_(),
      log_file_(log_file),
      initialized_(false),
      cleaned_up_(false) {}

CallTree::~CallTree() {
    if (!cleaned_up_) {
        cleanup();
    }
    // Clear all state
    process_graphs_.clear();
    log_file_.clear();
    initialized_ = false;
    cleaned_up_ = true;
}

void CallTree::initialize() {
    factory_.initialize();
    process_graphs_.clear();
    initialized_ = true;
    cleaned_up_ = false;
}

void CallTree::cleanup() {
    if (cleaned_up_) {
        return;
    }

    // Clean up all process graphs
    for (auto& [key, graph] : process_graphs_) {
        if (graph) {
            graph->calls.clear();
            graph->root_calls.clear();
            graph->call_sequence.clear();
        }
    }
    process_graphs_.clear();

    // Clean up factory
    factory_.cleanup();

    cleaned_up_ = true;
}

bool CallTree::load(const std::string& trace_file) {
    if (!initialized_) {
        initialize();
    }
    log_file_ = trace_file;
    TraceReader reader;
    return reader.read(trace_file, *this);
}

void CallTree::add_call(const ProcessKey& key,
                        std::shared_ptr<CallTreeNode> call) {
    // make sure process graph exists
    if (process_graphs_.find(key) == process_graphs_.end()) {
        process_graphs_[key] = std::make_unique<ProcessCallTree>();
        process_graphs_[key]->key = key;
    }

    ProcessCallTree* graph = process_graphs_[key].get();
    graph->calls[call->get_id()] = call;
    graph->call_sequence.push_back(call->get_id());
}

void CallTree::build_hierarchy() {
    DFTRACER_UTILS_LOG_INFO("building hierarchy for %zu process graphs...",
                            process_graphs_.size());

    size_t count = 0;
    for (auto& [key, graph] : process_graphs_) {
        count++;
        if (count % 10 == 0 || count == process_graphs_.size()) {
            DFTRACER_UTILS_LOG_DEBUG("  processed %zu/%zu processes...", count,
                                     process_graphs_.size());
        }
        build_hierarchy_internal(graph.get());
    }

    DFTRACER_UTILS_LOG_INFO("%s", "hierarchy building complete");
}

void CallTree::build_hierarchy_for_process(const ProcessKey& key) {
    auto it = process_graphs_.find(key);
    if (it != process_graphs_.end()) {
        build_hierarchy_internal(it->second.get());
    }
}

void CallTree::build_hierarchy_internal(ProcessCallTree* graph) {
    // Skip if already built (root_calls is populated)
    if (!graph->root_calls.empty()) {
        return;
    }

    std::vector<std::shared_ptr<CallTreeNode>> sorted_calls;
    sorted_calls.reserve(graph->calls.size());

    for (auto& [id, call] : graph->calls) {
        sorted_calls.push_back(call);
    }

    // sort by start time to build hierarchy
    std::sort(sorted_calls.begin(), sorted_calls.end(),
              [](const auto& a, const auto& b) {
                  return a->get_start_time() < b->get_start_time();
              });

    // find parents for each call
    for (auto& call : sorted_calls) {
        bool found_parent = false;

        // look for parent that contains this call
        for (auto& potential_parent : sorted_calls) {
            if (potential_parent->get_id() == call->get_id()) continue;

            std::uint64_t parent_end = potential_parent->get_start_time() +
                                       potential_parent->get_duration();

            // check if call is inside parent timespan and level is correct
            if (call->get_start_time() >= potential_parent->get_start_time() &&
                (call->get_start_time() + call->get_duration()) <= parent_end &&
                call->get_level() > potential_parent->get_level()) {
                // find closest parent by level
                if (!found_parent ||
                    potential_parent->get_level() >
                        graph->calls[call->get_parent_id()]->get_level()) {
                    call->set_parent_id(potential_parent->get_id());
                    found_parent = true;
                }
            }
        }

        // add to parent children or root
        if (found_parent) {
            graph->calls[call->get_parent_id()]->add_child(call->get_id());
        } else {
            graph->root_calls.push_back(call->get_id());
        }
    }
}

ProcessCallTree* CallTree::get(const ProcessKey& key) {
    auto it = process_graphs_.find(key);
    if (it != process_graphs_.end()) {
        return it->second.get();
    }
    return nullptr;
}

ProcessCallTree* CallTree::get(std::uint32_t pid, std::uint32_t tid,
                               std::uint32_t node_id) {
    return get(ProcessKey(pid, tid, node_id));
}

ProcessCallTree& CallTree::operator[](const ProcessKey& key) {
    auto it = process_graphs_.find(key);
    if (it == process_graphs_.end()) {
        // Create new process graph if it doesn't exist
        process_graphs_[key] = std::make_unique<ProcessCallTree>();
        process_graphs_[key]->key = key;
        return *process_graphs_[key];
    }
    return *it->second;
}

std::vector<ProcessKey> CallTree::keys() const {
    std::vector<ProcessKey> result;
    result.reserve(process_graphs_.size());
    for (const auto& [key, graph] : process_graphs_) {
        result.push_back(key);
    }
    return result;
}

void CallTree::print(const ProcessKey& key) const {
    auto it = process_graphs_.find(key);
    if (it == process_graphs_.end()) {
        DFTRACER_UTILS_LOG_WARN(
            "no graph for process key (pid=%u, tid=%u, node=%u)", key.pid,
            key.tid, key.node_id);
        return;
    }

    const ProcessCallTree& graph = *it->second;
    DFTRACER_UTILS_LOG_INFO(
        "call graph for process key (pid=%u, tid=%u, node=%u)", key.pid,
        key.tid, key.node_id);
    DFTRACER_UTILS_LOG_INFO("total calls: %zu", graph.calls.size());
    DFTRACER_UTILS_LOG_INFO("%s", "");

    // print root calls first
    for (std::uint64_t root_id : graph.root_calls) {
        print_calls_recursive(graph, root_id, 0);
    }
}

void CallTree::print(std::uint32_t pid, std::uint32_t tid,
                     std::uint32_t node_id) const {
    print(ProcessKey(pid, tid, node_id));
}

void CallTree::print_calls_recursive(const ProcessCallTree& graph,
                                     std::uint64_t call_id, int indent) const {
    auto it = graph.calls.find(call_id);
    if (it == graph.calls.end()) {
        return;
    }

    const auto& call = it->second;

    // print indentation
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }

    // print call info
    printf("%s [%s] level=%d dur=%luus ts=%lu\n", call->get_name().c_str(),
           call->get_category().c_str(), call->get_level(),
           (unsigned long)call->get_duration(),
           (unsigned long)call->get_start_time());

    // print children
    for (std::uint64_t child_id : call->get_children()) {
        print_calls_recursive(graph, child_id, indent + 1);
    }
}

}  // namespace internal
}  // namespace dftracer::utils::call_tree
