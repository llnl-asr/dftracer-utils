#include <dftracer/utils/call_tree/internal/call_tree.h>
#include <dftracer/utils/call_tree/internal/factory.h>
#include <dftracer/utils/call_tree/internal/node.h>
#include <dftracer/utils/call_tree/internal/process_call_tree.h>
#include <dftracer/utils/call_tree/internal/process_key.h>
#include <dftracer/utils/call_tree/internal/trace_reader.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/common/json/parser.h>
#include <dftracer/utils/utilities/composites/dft/event.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <simdjson.h>

#include <algorithm>
#include <cstdio>
#include <string_view>

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

CallTreeNode::CallTreeNode(std::uint64_t id, std::string_view name,
                           std::string_view category)
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
    id_ = 0;
    name_ = {};
    category_ = {};
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

void CallTreeNode::initialize(std::uint64_t id, std::string_view name,
                              std::string_view category,
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
    args_.clear();
    children_.clear();
    name_ = {};
    category_ = {};
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
    std::uint64_t id, std::string_view name, std::string_view category,
    std::uint64_t start_time, std::uint64_t duration, int level, ArgsMap args) {
    auto node = std::make_shared<CallTreeNode>(id, name, category);
    node->initialize(id, name, category, start_time, duration, level);
    node->set_args(std::move(args));
    managed_nodes_.push_back(node);
    node_count_++;
    return node;
}

// ============================================================================
// TraceReader Implementation (delegates to utilities::reader::TraceReader)
// ============================================================================

namespace {

using dftracer::utils::utilities::common::json::JsonParser;
using dftracer::utils::utilities::composites::dft::DFTracerEvent;

struct ParsedEvent {
    bool parsed = false;
    bool filtered = false;
};

dftracer::utils::StringIntern& name_intern() {
    static dftracer::utils::StringIntern instance;
    return instance;
}

ParsedEvent ingest_event(JsonParser& parser, CallTree& graph,
                         const std::set<std::uint32_t>* allowed_pids) {
    ParsedEvent out;

    DFTracerEvent ev;
    if (!DFTracerEvent::parse_ondemand(parser, ev)) return out;
    out.parsed = true;

    if (allowed_pids && allowed_pids->find(static_cast<std::uint32_t>(
                            ev.pid)) == allowed_pids->end()) {
        out.filtered = true;
        return out;
    }

    if (!ev.is_complete()) return out;

    int level = 0;
    std::uint32_t tid = 0;
    std::uint32_t node_id = 0;
    if (auto p = ev.args["level"])
        level = static_cast<int>(p.get<std::int64_t>());
    if (auto p = ev.args["tid"])
        tid = static_cast<std::uint32_t>(p.get<std::uint64_t>());
    if (auto p = ev.args["node_id"])
        node_id = static_cast<std::uint32_t>(p.get<std::uint64_t>());

    auto name_sv = name_intern().intern(ev.name);
    auto cat_sv = name_intern().intern(ev.cat);

    ProcessKey key(static_cast<std::uint32_t>(ev.pid), tid, node_id);
    auto call = graph.get_factory().create_node(
        ev.id, name_sv, cat_sv, ev.ts, ev.dur, level, std::move(ev.args));
    graph.add_call(key, call);
    return out;
}

}  // namespace

coro::CoroTask<ReadCounts> read_trace_file_async(
    std::string trace_file, CallTree* graph,
    const std::set<std::uint32_t>* allowed_pids) {
    using dftracer::utils::utilities::reader::ReadConfig;
    using dftracer::utils::utilities::reader::TraceReader;
    using dftracer::utils::utilities::reader::TraceReaderConfig;

    ReadCounts counts;

    TraceReaderConfig cfg;
    cfg.file_path = trace_file;
    cfg.auto_build_index = true;
    TraceReader reader(std::move(cfg));

    auto gen = reader.read_json(ReadConfig{});
    while (auto opt = co_await gen.next()) {
        auto res = ingest_event(*opt->parser, *graph, allowed_pids);
        if (res.filtered)
            counts.filtered++;
        else if (res.parsed)
            counts.processed++;
    }

    co_return counts;
}

ReadCounts read_trace_file(const std::string& trace_file, CallTree& graph,
                           const std::set<std::uint32_t>* allowed_pids) {
    return read_trace_file_async(trace_file, &graph, allowed_pids).get();
}

bool TraceReader::read(const std::string& trace_file, CallTree& graph) {
    DFTRACER_UTILS_LOG_INFO("reading trace file: %s", trace_file.c_str());
    auto counts = read_trace_file(trace_file, graph, nullptr);
    DFTRACER_UTILS_LOG_INFO("processed %zu trace entries from %s",
                            counts.processed, trace_file.c_str());
    return true;
}

bool TraceReader::read_multiple(const std::vector<std::string>& trace_files,
                                CallTree& graph) {
    DFTRACER_UTILS_LOG_INFO("reading %zu trace files...", trace_files.size());
    bool all_success = true;
    for (const auto& file : trace_files) {
        if (!read(file, graph)) {
            DFTRACER_UTILS_LOG_ERROR("failed to read: %s", file.c_str());
            all_success = false;
        }
    }
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
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        std::string filename = entry.path().filename().string();
        if (pattern == "*" ||
            filename.find(pattern.substr(1)) != std::string::npos) {
            trace_files.push_back(entry.path().string());
        }
    }

    if (trace_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("no trace files found in %s matching %s",
                                 directory.c_str(), pattern.c_str());
        return false;
    }

    std::sort(trace_files.begin(), trace_files.end());
    DFTRACER_UTILS_LOG_INFO("found %zu trace files in %s", trace_files.size(),
                            directory.c_str());
    return read_multiple(trace_files, graph);
}

bool TraceReader::process_trace_line(JsonParser& parser, CallTree& graph) {
    auto res = ingest_event(parser, graph, nullptr);
    return res.parsed;
}

bool TraceReader::process_trace_line(const std::string& line, CallTree& graph) {
    JsonParser parser;
    if (!parser.parse(line)) return false;
    return process_trace_line(parser, graph);
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

void CallTree::merge_from(CallTree&& other) {
    for (auto& [key, src_graph] : other.process_graphs_) {
        if (!src_graph) continue;
        auto it = process_graphs_.find(key);
        if (it == process_graphs_.end()) {
            process_graphs_.emplace(key, std::move(src_graph));
        } else {
            auto& dst = *it->second;
            for (auto& [id, node] : src_graph->calls) {
                dst.calls[id] = std::move(node);
            }
            dst.call_sequence.insert(dst.call_sequence.end(),
                                     src_graph->call_sequence.begin(),
                                     src_graph->call_sequence.end());
        }
    }
    other.process_graphs_.clear();
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

    std::sort(sorted_calls.begin(), sorted_calls.end(),
              [](const auto& a, const auto& b) {
                  std::uint64_t a_start = a->get_start_time();
                  std::uint64_t b_start = b->get_start_time();
                  if (a_start != b_start) return a_start < b_start;
                  std::uint64_t a_end = a_start + a->get_duration();
                  std::uint64_t b_end = b_start + b->get_duration();
                  if (a_end != b_end) return a_end > b_end;
                  return a->get_level() < b->get_level();
              });

    struct OpenEntry {
        std::uint64_t end_time;
        std::uint64_t id;
    };
    std::vector<std::vector<OpenEntry>> open_by_level;

    for (auto& call : sorted_calls) {
        const std::uint64_t call_start = call->get_start_time();
        const std::uint64_t call_end = call_start + call->get_duration();
        const int call_level = call->get_level();

        std::uint64_t parent_id = 0;
        int probe_max =
            std::min<int>(call_level, static_cast<int>(open_by_level.size())) -
            1;
        for (int lvl = probe_max; lvl >= 0; --lvl) {
            auto& stack = open_by_level[lvl];
            while (!stack.empty() && stack.back().end_time < call_start) {
                stack.pop_back();
            }
            for (auto sit = stack.rbegin(); sit != stack.rend(); ++sit) {
                if (sit->end_time >= call_end) {
                    parent_id = sit->id;
                    break;
                }
            }
            if (parent_id != 0) break;
        }

        if (parent_id != 0) {
            call->set_parent_id(parent_id);
            graph->calls[parent_id]->add_child(call->get_id());
        } else {
            graph->root_calls.push_back(call->get_id());
        }

        if (call_level >= static_cast<int>(open_by_level.size())) {
            open_by_level.resize(call_level + 1);
        }
        open_by_level[call_level].push_back({call_end, call->get_id()});
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
    auto nm = call->get_name();
    auto ct = call->get_category();
    printf("%.*s [%.*s] level=%d dur=%luus ts=%lu\n",
           static_cast<int>(nm.size()), nm.data(), static_cast<int>(ct.size()),
           ct.data(), call->get_level(), (unsigned long)call->get_duration(),
           (unsigned long)call->get_start_time());

    // print children
    for (std::uint64_t child_id : call->get_children()) {
        print_calls_recursive(graph, child_id, indent + 1);
    }
}

}  // namespace internal
}  // namespace dftracer::utils::call_tree
