#include <dftracer/utils/call_tree/internal/call_tree.h>
#include <dftracer/utils/call_tree/internal/process_call_tree.h>
#include <dftracer/utils/call_tree/internal/process_key.h>
#include <dftracer/utils/call_tree/internal/trace_reader.h>
#include <dftracer/utils/call_tree/mpi/build_task.h>
#include <dftracer/utils/call_tree/mpi/builder.h>
#include <dftracer/utils/call_tree/mpi/config.h>
#include <dftracer/utils/call_tree/mpi/file_header.h>
#include <dftracer/utils/call_tree/mpi/filtered_reader.h>
#include <dftracer/utils/call_tree/mpi/pid_index_info.h>
#include <dftracer/utils/call_tree/mpi/serializable.h>
#include <dftracer/utils/call_tree/mpi/serialization.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/format_detector.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/reader/internal/line_processor.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <yyjson.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

namespace dftracer::utils::call_tree {

// ============================================================================
// Serialization Utilities
// ============================================================================

namespace serialization {

void write_uint32(std::vector<char>& buffer, std::uint32_t value) {
    buffer.insert(buffer.end(), reinterpret_cast<const char*>(&value),
                  reinterpret_cast<const char*>(&value) + sizeof(value));
}

void write_uint64(std::vector<char>& buffer, std::uint64_t value) {
    buffer.insert(buffer.end(), reinterpret_cast<const char*>(&value),
                  reinterpret_cast<const char*>(&value) + sizeof(value));
}

void write_int(std::vector<char>& buffer, int value) {
    buffer.insert(buffer.end(), reinterpret_cast<const char*>(&value),
                  reinterpret_cast<const char*>(&value) + sizeof(value));
}

void write_string(std::vector<char>& buffer, const std::string& str) {
    std::uint32_t len = static_cast<std::uint32_t>(str.size());
    write_uint32(buffer, len);
    buffer.insert(buffer.end(), str.begin(), str.end());
}

std::uint32_t read_uint32(const char* data, size_t& offset) {
    std::uint32_t value;
    std::memcpy(&value, data + offset, sizeof(value));
    offset += sizeof(value);
    return value;
}

std::uint64_t read_uint64(const char* data, size_t& offset) {
    std::uint64_t value;
    std::memcpy(&value, data + offset, sizeof(value));
    offset += sizeof(value);
    return value;
}

int read_int(const char* data, size_t& offset) {
    int value;
    std::memcpy(&value, data + offset, sizeof(value));
    offset += sizeof(value);
    return value;
}

std::string read_string(const char* data, size_t& offset) {
    std::uint32_t len = read_uint32(data, offset);
    std::string str(data + offset, len);
    offset += len;
    return str;
}

}  // namespace serialization

// ============================================================================
// SerializableCallNode Implementation
// ============================================================================

std::vector<char> SerializableCallNode::serialize() const {
    std::vector<char> buffer;

    serialization::write_uint64(buffer, id);
    serialization::write_string(buffer, name);
    serialization::write_string(buffer, category);
    serialization::write_uint64(buffer, start_time);
    serialization::write_uint64(buffer, duration);
    serialization::write_int(buffer, level);
    serialization::write_uint64(buffer, parent_id);

    // Children
    serialization::write_uint32(buffer,
                                static_cast<std::uint32_t>(children.size()));
    for (auto child_id : children) {
        serialization::write_uint64(buffer, child_id);
    }

    // Args
    serialization::write_uint32(buffer,
                                static_cast<std::uint32_t>(args.size()));
    for (const auto& [key, value] : args) {
        serialization::write_string(buffer, key);
        serialization::write_string(buffer, value);
    }

    return buffer;
}

SerializableCallNode SerializableCallNode::deserialize(const char* data,
                                                       size_t& offset) {
    SerializableCallNode node;

    node.id = serialization::read_uint64(data, offset);
    node.name = serialization::read_string(data, offset);
    node.category = serialization::read_string(data, offset);
    node.start_time = serialization::read_uint64(data, offset);
    node.duration = serialization::read_uint64(data, offset);
    node.level = serialization::read_int(data, offset);
    node.parent_id = serialization::read_uint64(data, offset);

    // Children
    std::uint32_t num_children = serialization::read_uint32(data, offset);
    node.children.reserve(num_children);
    for (std::uint32_t i = 0; i < num_children; i++) {
        node.children.push_back(serialization::read_uint64(data, offset));
    }

    // Args
    std::uint32_t num_args = serialization::read_uint32(data, offset);
    for (std::uint32_t i = 0; i < num_args; i++) {
        std::string key = serialization::read_string(data, offset);
        std::string value = serialization::read_string(data, offset);
        node.args[key] = value;
    }

    return node;
}

// ============================================================================
// SerializableProcessGraph Implementation
// ============================================================================

std::vector<char> SerializableProcessGraph::serialize() const {
    std::vector<char> buffer;

    // Key
    serialization::write_uint32(buffer, key.pid);
    serialization::write_uint32(buffer, key.tid);
    serialization::write_uint32(buffer, key.node_id);

    // Nodes
    serialization::write_uint32(buffer,
                                static_cast<std::uint32_t>(nodes.size()));
    for (const auto& node : nodes) {
        auto node_data = node.serialize();
        serialization::write_uint32(
            buffer, static_cast<std::uint32_t>(node_data.size()));
        buffer.insert(buffer.end(), node_data.begin(), node_data.end());
    }

    // Root calls
    serialization::write_uint32(buffer,
                                static_cast<std::uint32_t>(root_calls.size()));
    for (auto id : root_calls) {
        serialization::write_uint64(buffer, id);
    }

    // Call sequence
    serialization::write_uint32(
        buffer, static_cast<std::uint32_t>(call_sequence.size()));
    for (auto id : call_sequence) {
        serialization::write_uint64(buffer, id);
    }

    return buffer;
}

SerializableProcessGraph SerializableProcessGraph::deserialize(const char* data,
                                                               size_t& offset) {
    SerializableProcessGraph graph;

    // Key
    graph.key.pid = serialization::read_uint32(data, offset);
    graph.key.tid = serialization::read_uint32(data, offset);
    graph.key.node_id = serialization::read_uint32(data, offset);

    // Nodes
    std::uint32_t num_nodes = serialization::read_uint32(data, offset);
    graph.nodes.reserve(num_nodes);
    for (std::uint32_t i = 0; i < num_nodes; i++) {
        std::uint32_t node_size = serialization::read_uint32(data, offset);
        (void)node_size;  // Not needed for deserialization
        graph.nodes.push_back(SerializableCallNode::deserialize(data, offset));
    }

    // Root calls
    std::uint32_t num_roots = serialization::read_uint32(data, offset);
    graph.root_calls.reserve(num_roots);
    for (std::uint32_t i = 0; i < num_roots; i++) {
        graph.root_calls.push_back(serialization::read_uint64(data, offset));
    }

    // Call sequence
    std::uint32_t num_seq = serialization::read_uint32(data, offset);
    graph.call_sequence.reserve(num_seq);
    for (std::uint32_t i = 0; i < num_seq; i++) {
        graph.call_sequence.push_back(serialization::read_uint64(data, offset));
    }

    return graph;
}

// ============================================================================
// MPIFilteredTraceReader Implementation
// ============================================================================

MPIFilteredTraceReader::MPIFilteredTraceReader(
    const std::set<std::uint32_t>& allowed_pids)
    : allowed_pids_(allowed_pids), processed_count_(0), filtered_count_(0) {}

bool MPIFilteredTraceReader::read(const std::string& trace_file,
                                  internal::CallTree& graph) {
    // Check if it's a gzip file
    ArchiveFormat format = FormatDetector::detect(trace_file);

    if (format == ArchiveFormat::GZIP) {
        std::string index_path =
            utilities::composites::dft::internal::determine_index_path(
                trace_file, "");
        if (fs::exists(index_path)) {
            return read_with_indexer(trace_file, index_path, graph);
        }
    }

    // Fall back to direct reading for plain text files
    std::ifstream file(trace_file);
    if (!file.is_open()) {
        DFTRACER_UTILS_LOG_ERROR("Cannot open trace file: %s",
                                 trace_file.c_str());
        return false;
    }

    std::string line;
    size_t line_count = 0;

    while (std::getline(file, line)) {
        line_count++;

        // Skip brackets and empty lines
        if (line.empty() || line == "[" || line == "]") {
            continue;
        }

        // Remove trailing comma
        if (!line.empty() && line.back() == ',') {
            line.pop_back();
        }

        yyjson_doc* doc = yyjson_read(line.c_str(), line.length(), 0);
        if (!doc) {
            continue;
        }

        yyjson_val* root = yyjson_doc_get_root(doc);
        if (!root) {
            yyjson_doc_free(doc);
            continue;
        }

        // Check PID filter
        yyjson_val* pid_val = yyjson_obj_get(root, "pid");
        if (pid_val) {
            std::uint32_t pid =
                static_cast<std::uint32_t>(yyjson_get_uint(pid_val));

            // Only process if PID is in our allowed set
            if (allowed_pids_.find(pid) != allowed_pids_.end()) {
                // Use the standard internal::TraceReader processing
                internal::TraceReader reader;
                if (reader.process_trace_line(line, graph)) {
                    processed_count_++;
                }
            } else {
                filtered_count_++;
            }
        }

        yyjson_doc_free(doc);
    }

    return true;
}

/**
 * Line processor for filtered reading with indexer
 */
class FilteredLineProcessor
    : public utilities::reader::internal::LineProcessor {
   public:
    FilteredLineProcessor(const std::set<std::uint32_t>& allowed_pids,
                          internal::CallTree& graph,
                          std::size_t& processed_count,
                          std::size_t& filtered_count)
        : allowed_pids_(allowed_pids),
          graph_(graph),
          processed_count_(processed_count),
          filtered_count_(filtered_count),
          reader_() {}

    coro::CoroTask<bool> process(const char* data,
                                 std::size_t length) override {
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

        // Quick PID check
        yyjson_doc* doc = yyjson_read(line.c_str(), line.length(), 0);
        if (!doc) {
            co_return true;
        }

        yyjson_val* root = yyjson_doc_get_root(doc);
        if (!root) {
            yyjson_doc_free(doc);
            co_return true;
        }

        yyjson_val* pid_val = yyjson_obj_get(root, "pid");
        if (pid_val) {
            std::uint32_t pid =
                static_cast<std::uint32_t>(yyjson_get_uint(pid_val));

            if (allowed_pids_.find(pid) != allowed_pids_.end()) {
                if (reader_.process_trace_line(line, graph_)) {
                    processed_count_++;
                }
            } else {
                filtered_count_++;
            }
        }

        yyjson_doc_free(doc);
        co_return true;
    }

   private:
    const std::set<std::uint32_t>& allowed_pids_;
    internal::CallTree& graph_;
    std::size_t& processed_count_;
    std::size_t& filtered_count_;
    internal::TraceReader reader_;
};

bool MPIFilteredTraceReader::read_with_indexer(const std::string& trace_file,
                                               const std::string& index_file,
                                               internal::CallTree& graph) {
    try {
        auto reader = utilities::reader::internal::ReaderFactory::create(
            trace_file, index_file);
        if (!reader || !reader->is_valid()) {
            DFTRACER_UTILS_LOG_ERROR("Failed to create reader for %s",
                                     trace_file.c_str());
            return read(trace_file, graph);  // Fallback
        }

        FilteredLineProcessor processor(allowed_pids_, graph, processed_count_,
                                        filtered_count_);

        std::size_t num_lines = reader->get_num_lines();
        if (num_lines > 0) {
            reader->read_lines_with_processor(1, num_lines, processor);
        }

        return true;
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Error reading with indexer: %s", e.what());
        return false;
    }
}

bool MPIFilteredTraceReader::read_multiple(
    const std::vector<std::string>& trace_files, internal::CallTree& graph) {
    for (const auto& file : trace_files) {
        if (!read(file, graph)) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// MPICallTreeBuilder Implementation
// ============================================================================

MPICallTreeBuilder::MPICallTreeBuilder(const MPICallTreeConfig& config)
    : config_(config),
      call_tree_(std::make_unique<internal::CallTree>()),
      trace_files_(),
      indexers_(),
      pid_index_map_(),
      assigned_pids_(),
      all_pids_(),
      initialized_(false),
      pids_discovered_(false),
      graphs_built_(false),
      graphs_gathered_(false) {}

MPICallTreeBuilder::~MPICallTreeBuilder() {
    if (initialized_) {
        cleanup();
    }
}

MPICallTreeBuilder::MPICallTreeBuilder(MPICallTreeBuilder&& other) noexcept
    : config_(std::move(other.config_)),
      call_tree_(std::move(other.call_tree_)),
      trace_files_(std::move(other.trace_files_)),
      indexers_(std::move(other.indexers_)),
      pid_index_map_(std::move(other.pid_index_map_)),
      assigned_pids_(std::move(other.assigned_pids_)),
      all_pids_(std::move(other.all_pids_)),
      initialized_(other.initialized_),
      pids_discovered_(other.pids_discovered_),
      graphs_built_(other.graphs_built_),
      graphs_gathered_(other.graphs_gathered_) {
    other.initialized_ = false;
}

MPICallTreeBuilder& MPICallTreeBuilder::operator=(
    MPICallTreeBuilder&& other) noexcept {
    if (this != &other) {
        if (initialized_) {
            cleanup();
        }
        config_ = std::move(other.config_);
        call_tree_ = std::move(other.call_tree_);
        trace_files_ = std::move(other.trace_files_);
        indexers_ = std::move(other.indexers_);
        pid_index_map_ = std::move(other.pid_index_map_);
        assigned_pids_ = std::move(other.assigned_pids_);
        all_pids_ = std::move(other.all_pids_);
        initialized_ = other.initialized_;
        pids_discovered_ = other.pids_discovered_;
        graphs_built_ = other.graphs_built_;
        graphs_gathered_ = other.graphs_gathered_;
        other.initialized_ = false;
    }
    return *this;
}

void MPICallTreeBuilder::initialize() {
    if (initialized_) {
        return;
    }

    // Initialize MPI utilities singleton
    mpi::MPIUtils::instance().initialize();

    call_tree_->initialize();
    initialized_ = true;

    if (mpi::MPIUtils::instance().is_root() && config_.verbose) {
        DFTRACER_UTILS_LOG_INFO(
            "MPICallTreeBuilder initialized with %d MPI ranks",
            mpi::MPIUtils::instance().get_world_size());
    }
}

void MPICallTreeBuilder::cleanup() {
    if (!initialized_) {
        return;
    }

    call_tree_->cleanup();
    indexers_.clear();
    trace_files_.clear();
    pid_index_map_.clear();
    assigned_pids_.clear();
    all_pids_.clear();

    initialized_ = false;
    pids_discovered_ = false;
    graphs_built_ = false;
    graphs_gathered_ = false;
}

void MPICallTreeBuilder::add_trace_files(
    const std::vector<std::string>& files) {
    for (const auto& file : files) {
        if (fs::exists(file) && fs::is_regular_file(file)) {
            trace_files_.push_back(file);
        } else if (mpi::MPIUtils::instance().is_root()) {
            DFTRACER_UTILS_LOG_WARN("File not found: %s", file.c_str());
        }
    }
}

void MPICallTreeBuilder::add_trace_directory(const std::string& directory,
                                             const std::string& pattern) {
    if (!fs::exists(directory) || !fs::is_directory(directory)) {
        if (mpi::MPIUtils::instance().is_root()) {
            DFTRACER_UTILS_LOG_ERROR("Directory not found: %s",
                                     directory.c_str());
        }
        return;
    }

    // Recursively find all matching files
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();

            // Simple pattern matching for *.ext or *.part1.part2 patterns
            bool matches = false;
            if (pattern == "*") {
                matches = true;
            } else if (pattern.front() == '*') {
                // *.ext or *.pfw.gz pattern
                std::string suffix = pattern.substr(1);  // Remove the leading *
                matches = (filename.size() >= suffix.size() &&
                           filename.substr(filename.size() - suffix.size()) ==
                               suffix);
            } else {
                matches = (filename.find(pattern) != std::string::npos);
            }

            if (matches) {
                trace_files_.push_back(entry.path().string());
            }
        }
    }

    std::sort(trace_files_.begin(), trace_files_.end());

    if (mpi::MPIUtils::instance().is_root() && config_.verbose) {
        DFTRACER_UTILS_LOG_INFO("Found %zu trace files in %s",
                                trace_files_.size(), directory.c_str());
    }
}

void MPICallTreeBuilder::create_indexer(const std::string& trace_file) {
    if (indexers_.find(trace_file) != indexers_.end()) {
        return;
    }

    ArchiveFormat format = FormatDetector::detect(trace_file);
    if (format != ArchiveFormat::GZIP) {
        return;  // Only create indexers for gzip files
    }

    std::string idx_file = trace_file + ".zindex";
    std::uint64_t ckpt_size =
        config_.checkpoint_size > 0
            ? config_.checkpoint_size
            : utilities::indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE;

    try {
        auto indexer = utilities::indexer::internal::IndexerFactory::create(
            trace_file, idx_file, ckpt_size, false);
        if (indexer) {
            // Build index if needed
            if (indexer->need_rebuild()) {
                if (mpi::MPIUtils::instance().is_root() && config_.verbose) {
                    DFTRACER_UTILS_LOG_INFO("Building index for %s",
                                            trace_file.c_str());
                }
                indexer->build();
            }
            indexers_[trace_file] = std::move(indexer);
        }
    } catch (const std::exception& e) {
        if (config_.verbose) {
            DFTRACER_UTILS_LOG_WARN("Could not create indexer for %s: %s",
                                    trace_file.c_str(), e.what());
        }
    }
}

std::set<std::uint32_t> MPICallTreeBuilder::scan_file_for_pids(
    const std::string& trace_file) {
    std::set<std::uint32_t> pids;

    // Check if it's a gzip file with an index
    ArchiveFormat format = FormatDetector::detect(trace_file);
    std::string index_path =
        utilities::composites::dft::internal::determine_index_path(trace_file,
                                                                   "");

    if (format == ArchiveFormat::GZIP && fs::exists(index_path)) {
        try {
            auto reader = utilities::reader::internal::ReaderFactory::create(
                trace_file, index_path);
            if (reader && reader->is_valid()) {
                // Read first N lines to discover PIDs
                std::size_t num_lines = reader->get_num_lines();
                std::string content = reader->read_lines(
                    1, std::min(num_lines, (std::size_t)100000));

                std::istringstream iss(content);
                std::string line;
                while (std::getline(iss, line)) {
                    if (line.empty() || line == "[" || line == "]") continue;
                    if (!line.empty() && line.back() == ',') line.pop_back();

                    yyjson_doc* doc =
                        yyjson_read(line.c_str(), line.length(), 0);
                    if (doc) {
                        yyjson_val* root = yyjson_doc_get_root(doc);
                        if (root) {
                            yyjson_val* pid_val = yyjson_obj_get(root, "pid");
                            if (pid_val) {
                                pids.insert(static_cast<std::uint32_t>(
                                    yyjson_get_uint(pid_val)));
                            }
                        }
                        yyjson_doc_free(doc);
                    }
                }

                return pids;
            }
        } catch (const std::exception& e) {
            // Fall through to direct reading
        }
    }

    // For gzip files without index, use gzopen
    if (format == ArchiveFormat::GZIP) {
        gzFile gz = gzopen(trace_file.c_str(), "rb");
        if (!gz) {
            return pids;
        }

        char buffer[65536];
        std::string current_line;
        int line_count = 0;

        while (line_count < 100000) {
            int bytes_read = gzread(gz, buffer, sizeof(buffer) - 1);
            if (bytes_read <= 0) break;
            buffer[bytes_read] = '\0';

            current_line += buffer;

            // Process complete lines
            size_t pos;
            while ((pos = current_line.find('\n')) != std::string::npos) {
                std::string line = current_line.substr(0, pos);
                current_line = current_line.substr(pos + 1);
                line_count++;

                if (line.empty() || line == "[" || line == "]") continue;
                if (!line.empty() && line.back() == ',') line.pop_back();

                yyjson_doc* doc = yyjson_read(line.c_str(), line.length(), 0);
                if (doc) {
                    yyjson_val* root = yyjson_doc_get_root(doc);
                    if (root) {
                        yyjson_val* pid_val = yyjson_obj_get(root, "pid");
                        if (pid_val) {
                            pids.insert(static_cast<std::uint32_t>(
                                yyjson_get_uint(pid_val)));
                        }
                    }
                    yyjson_doc_free(doc);
                }

                if (line_count >= 100000) break;
            }
        }

        gzclose(gz);
        return pids;
    }

    // Fall back to direct file reading for plain text
    std::ifstream file(trace_file);
    if (!file.is_open()) {
        return pids;
    }

    std::string line;
    int line_count = 0;
    while (std::getline(file, line) && line_count < 100000) {
        line_count++;
        if (line.empty() || line == "[" || line == "]") continue;
        if (!line.empty() && line.back() == ',') line.pop_back();

        yyjson_doc* doc = yyjson_read(line.c_str(), line.length(), 0);
        if (doc) {
            yyjson_val* root = yyjson_doc_get_root(doc);
            if (root) {
                yyjson_val* pid_val = yyjson_obj_get(root, "pid");
                if (pid_val) {
                    pids.insert(
                        static_cast<std::uint32_t>(yyjson_get_uint(pid_val)));
                }
            }
            yyjson_doc_free(doc);
        }
    }

    return pids;
}

void MPICallTreeBuilder::distribute_pids() {
    // Round-robin distribution using MPIUtils singleton
    auto& mpi = mpi::MPIUtils::instance();
    assigned_pids_.clear();
    for (size_t i = static_cast<size_t>(mpi.get_rank()); i < all_pids_.size();
         i += static_cast<size_t>(mpi.get_world_size())) {
        assigned_pids_.insert(all_pids_[i]);
    }

    if (config_.verbose) {
        DFTRACER_UTILS_LOG_DEBUG("[Rank %d] Assigned %zu PIDs", mpi.get_rank(),
                                 assigned_pids_.size());
    }
}

std::map<std::uint32_t, PIDIndexInfo> MPICallTreeBuilder::discover_pids() {
    if (!initialized_) {
        initialize();
    }

    auto& mpi = mpi::MPIUtils::instance();

    if (mpi.is_root() && config_.verbose) {
        DFTRACER_UTILS_LOG_INFO(
            "Phase 1: Discovering PIDs from %zu trace files...",
            trace_files_.size());
    }

    // Broadcast file list from rank 0 using MPIUtils
    int num_files = static_cast<int>(trace_files_.size());
    mpi.broadcast_int(num_files, 0);

    if (!mpi.is_root()) {
        trace_files_.resize(num_files);
    }

    for (int i = 0; i < num_files; i++) {
        mpi.broadcast_string(trace_files_[i], 0);
    }

    // Each rank scans files to discover PIDs
    std::set<std::uint32_t> local_pids;

    for (const auto& trace_file : trace_files_) {
        // Create indexer if needed
        create_indexer(trace_file);

        // Scan for PIDs
        auto file_pids = scan_file_for_pids(trace_file);
        local_pids.insert(file_pids.begin(), file_pids.end());

        // Store PID index info
        for (auto pid : file_pids) {
            if (pid_index_map_.find(pid) == pid_index_map_.end()) {
                pid_index_map_[pid] = PIDIndexInfo(pid, 0, 0, 0, trace_file);
            }
        }
    }

    // Gather all PIDs to rank 0 using MPIUtils
    std::vector<std::uint32_t> local_pid_vec(local_pids.begin(),
                                             local_pids.end());
    std::vector<std::uint32_t> all_pids_gathered;
    std::vector<int> recv_counts;
    std::vector<int> displacements;

    mpi.gatherv_uint32(local_pid_vec, all_pids_gathered, recv_counts,
                       displacements, 0);

    // Remove duplicates and sort on rank 0
    if (mpi.is_root()) {
        std::set<std::uint32_t> unique_pids(all_pids_gathered.begin(),
                                            all_pids_gathered.end());
        all_pids_.assign(unique_pids.begin(), unique_pids.end());
        std::sort(all_pids_.begin(), all_pids_.end());

        if (config_.verbose) {
            DFTRACER_UTILS_LOG_INFO("Discovered %zu unique PIDs",
                                    all_pids_.size());
        }
    }

    // Broadcast unique PIDs to all ranks
    mpi.broadcast_uint32_vector(all_pids_, 0);

    // Distribute PIDs across ranks
    distribute_pids();

    mpi.barrier();

    all_pids_.assign(local_pids.begin(), local_pids.end());
    assigned_pids_ = local_pids;

    pids_discovered_ = true;
    return pid_index_map_;
}

bool MPICallTreeBuilder::read_traces_for_pids(
    const std::vector<std::string>& files,
    const std::set<std::uint32_t>& pids) {
    MPIFilteredTraceReader reader(pids);
    return reader.read_multiple(files, *call_tree_);
}

MPICallTreeResult MPICallTreeBuilder::build() {
    MPICallTreeResult result;
    auto& mpi = mpi::MPIUtils::instance();

    if (!pids_discovered_) {
        discover_pids();
    }

    if (assigned_pids_.empty()) {
        if (config_.verbose) {
            DFTRACER_UTILS_LOG_DEBUG(
                "[Rank %d] No PIDs assigned, skipping build", mpi.get_rank());
        }
        result.success = true;
        graphs_built_ = true;
        return result;
    }

    if (mpi.is_root() && config_.verbose) {
        DFTRACER_UTILS_LOG_INFO("%s", "Phase 2: Building call graphs...");
    }

    mpi.barrier();

    auto start_time = std::chrono::high_resolution_clock::now();

    // Use pipeline for parallel trace reading
    if (config_.num_threads > 0) {
        // For now, use simple sequential processing
        // Pipeline can be expanded for more complex workflows
        read_traces_for_pids(trace_files_, assigned_pids_);
    } else {
        // Sequential processing
        read_traces_for_pids(trace_files_, assigned_pids_);
    }

    // Build hierarchy
    call_tree_->build_hierarchy();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    result.elapsed_time_s = elapsed.count();
    result.local_pids = assigned_pids_.size();
    result.local_events = 0;

    // Count events
    for (const auto& key : call_tree_->keys()) {
        auto* graph = call_tree_->get(key);
        if (graph) {
            result.local_events += graph->calls.size();
        }
    }

    // Gather statistics using MPIUtils
    mpi.reduce_sum_size_t(result.local_pids, result.total_pids, 0);
    mpi.reduce_sum_size_t(result.local_events, result.total_events, 0);

    double max_time = 0;
    mpi.reduce_max_double(result.elapsed_time_s, max_time, 0);
    result.elapsed_time_s = max_time;

    result.success = true;
    graphs_built_ = true;

    if (mpi.is_root() && config_.verbose) {
        DFTRACER_UTILS_LOG_INFO("Build completed in %.2f seconds",
                                result.elapsed_time_s);
        DFTRACER_UTILS_LOG_INFO("Total PIDs: %zu", result.total_pids);
        DFTRACER_UTILS_LOG_INFO("Total events: %zu", result.total_events);
    }

    return result;
}

SerializableProcessGraph MPICallTreeBuilder::convert_to_serializable(
    const internal::ProcessCallTree& graph) const {
    SerializableProcessGraph result;
    result.key = graph.key;
    result.root_calls = graph.root_calls;
    result.call_sequence = graph.call_sequence;

    for (const auto& [id, node] : graph.calls) {
        SerializableCallNode snode;
        snode.id = node->get_id();
        snode.name = node->get_name();
        snode.category = node->get_category();
        snode.start_time = node->get_start_time();
        snode.duration = node->get_duration();
        snode.level = node->get_level();
        snode.parent_id = node->get_parent_id();
        snode.children = node->get_children();
        snode.args = node->get_args();
        result.nodes.push_back(std::move(snode));
    }

    return result;
}

void MPICallTreeBuilder::merge_from_serializable(
    const SerializableProcessGraph& serializable) {
    internal::ProcessCallTree& graph = (*call_tree_)[serializable.key];
    graph.key = serializable.key;
    graph.root_calls = serializable.root_calls;
    graph.call_sequence = serializable.call_sequence;

    for (const auto& snode : serializable.nodes) {
        auto node = call_tree_->get_factory().create_node(
            snode.id, snode.name, snode.category, snode.start_time,
            snode.duration, snode.level, snode.args);
        node->set_parent_id(snode.parent_id);
        for (auto child_id : snode.children) {
            node->add_child(child_id);
        }
        graph.calls[snode.id] = node;
    }
}

bool MPICallTreeBuilder::alltoall_graphs() {
    auto& mpi = mpi::MPIUtils::instance();

    // Serialize local graphs
    std::vector<SerializableProcessGraph> local_graphs;
    for (const auto& key : call_tree_->keys()) {
        auto* graph = call_tree_->get(key);
        if (graph) {
            local_graphs.push_back(convert_to_serializable(*graph));
        }
    }

    // Serialize to bytes
    std::vector<char> send_buffer;
    serialization::write_uint32(
        send_buffer, static_cast<std::uint32_t>(local_graphs.size()));
    for (const auto& graph : local_graphs) {
        auto data = graph.serialize();
        serialization::write_uint32(send_buffer,
                                    static_cast<std::uint32_t>(data.size()));
        send_buffer.insert(send_buffer.end(), data.begin(), data.end());
    }

    // Use MPIUtils for allgatherv
    std::vector<char> recv_buffer;
    std::vector<int> recv_sizes;
    std::vector<int> displacements;

    mpi.allgatherv_char(send_buffer, recv_buffer, recv_sizes, displacements);

    // Deserialize graphs from other ranks
    int world_size = mpi.get_world_size();
    int rank = mpi.get_rank();
    for (int r = 0; r < world_size; r++) {
        if (r == rank) continue;  // Skip our own data

        size_t offset = static_cast<size_t>(displacements[r]);
        std::uint32_t num_graphs =
            serialization::read_uint32(recv_buffer.data(), offset);

        for (std::uint32_t i = 0; i < num_graphs; i++) {
            std::uint32_t graph_size =
                serialization::read_uint32(recv_buffer.data(), offset);
            (void)graph_size;
            auto graph = SerializableProcessGraph::deserialize(
                recv_buffer.data(), offset);
            merge_from_serializable(graph);
        }
    }

    return true;
}

bool MPICallTreeBuilder::gather() {
    if (!graphs_built_) {
        return false;
    }

    auto& mpi = mpi::MPIUtils::instance();

    if (mpi.is_root() && config_.verbose) {
        DFTRACER_UTILS_LOG_INFO(
            "%s", "Phase 3: Gathering call graphs (all-to-all)...");
    }

    mpi.barrier();

    bool success = alltoall_graphs();

    mpi.barrier();

    graphs_gathered_ = success;

    if (mpi.is_root() && config_.verbose) {
        DFTRACER_UTILS_LOG_INFO("Gather completed. Total graphs: %zu",
                                call_tree_->size());
    }

    return success;
}

bool MPICallTreeBuilder::save(const std::string& filename) const {
    // Only rank 0 saves (all ranks have same data after gather)
    if (!mpi::MPIUtils::instance().is_root()) {
        return true;
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        DFTRACER_UTILS_LOG_ERROR("Cannot open output file: %s",
                                 filename.c_str());
        return false;
    }

    // Write header
    CallGraphFileHeader header;
    header.num_process_graphs = static_cast<std::uint32_t>(call_tree_->size());

    // Count total events
    std::uint64_t total_events = 0;
    for (const auto& key : call_tree_->keys()) {
        auto* graph = call_tree_->get(key);
        if (graph) {
            total_events += graph->calls.size();
        }
    }
    header.total_events = total_events;
    header.data_offset = sizeof(CallGraphFileHeader);

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));

    // Write each process graph
    for (const auto& key : call_tree_->keys()) {
        auto* graph = call_tree_->get(key);
        if (graph) {
            auto serializable =
                const_cast<MPICallTreeBuilder*>(this)->convert_to_serializable(
                    *graph);
            auto data = serializable.serialize();
            std::uint32_t size = static_cast<std::uint32_t>(data.size());
            file.write(reinterpret_cast<const char*>(&size), sizeof(size));
            file.write(data.data(), data.size());
        }
    }

    if (config_.verbose) {
        DFTRACER_UTILS_LOG_INFO("Saved call graph to %s", filename.c_str());
    }

    return true;
}

std::unique_ptr<internal::CallTree> MPICallTreeBuilder::load(
    const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        DFTRACER_UTILS_LOG_ERROR("Cannot open file: %s", filename.c_str());
        return nullptr;
    }

    // Read header
    CallGraphFileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (!header.is_valid()) {
        DFTRACER_UTILS_LOG_ERROR("%s", "Invalid call graph file format");
        return nullptr;
    }

    auto call_graph = std::make_unique<internal::CallTree>();
    call_graph->initialize();

    // Read each process graph
    for (std::uint32_t i = 0; i < header.num_process_graphs; i++) {
        std::uint32_t size;
        file.read(reinterpret_cast<char*>(&size), sizeof(size));

        std::vector<char> data(size);
        file.read(data.data(), size);

        size_t offset = 0;
        auto serializable =
            SerializableProcessGraph::deserialize(data.data(), offset);

        // Merge into call graph
        internal::ProcessCallTree& graph = (*call_graph)[serializable.key];
        graph.key = serializable.key;
        graph.root_calls = serializable.root_calls;
        graph.call_sequence = serializable.call_sequence;

        for (const auto& snode : serializable.nodes) {
            auto node = call_graph->get_factory().create_node(
                snode.id, snode.name, snode.category, snode.start_time,
                snode.duration, snode.level, snode.args);
            node->set_parent_id(snode.parent_id);
            for (auto child_id : snode.children) {
                node->add_child(child_id);
            }
            graph.calls[snode.id] = node;
        }
    }

    return call_graph;
}

void MPICallTreeBuilder::print_summary() const {
    auto& mpi = mpi::MPIUtils::instance();
    std::size_t local_graphs = call_tree_->size();
    std::size_t local_events = 0;

    for (const auto& key : call_tree_->keys()) {
        auto* graph = call_tree_->get(key);
        if (graph) {
            local_events += graph->calls.size();
        }
    }

    std::size_t total_graphs = 0;
    std::size_t total_events = 0;

    // Use MPIUtils for reduce operations
    const_cast<mpi::MPIUtils&>(mpi).reduce_sum_size_t(local_graphs,
                                                      total_graphs, 0);
    const_cast<mpi::MPIUtils&>(mpi).reduce_sum_size_t(local_events,
                                                      total_events, 0);

    if (mpi.is_root()) {
        DFTRACER_UTILS_LOG_INFO(
            "%s", "\n============ MPI Call Graph Summary ============");
        DFTRACER_UTILS_LOG_INFO("MPI Ranks: %d", mpi.get_world_size());
        DFTRACER_UTILS_LOG_INFO("Total PIDs: %zu", all_pids_.size());
        DFTRACER_UTILS_LOG_INFO("Total process graphs: %zu", total_graphs);
        DFTRACER_UTILS_LOG_INFO("Total events: %zu", total_events);
        DFTRACER_UTILS_LOG_INFO(
            "%s", "================================================\n");
    }

    // Each rank prints its summary
    int world_size = mpi.get_world_size();
    int rank = mpi.get_rank();
    for (int r = 0; r < world_size; r++) {
        if (r == rank) {
            DFTRACER_UTILS_LOG_INFO("[Rank %d] Local Summary:", rank);
            DFTRACER_UTILS_LOG_INFO("  Assigned PIDs: %zu",
                                    assigned_pids_.size());
            DFTRACER_UTILS_LOG_INFO("  Process graphs: %zu", local_graphs);
            DFTRACER_UTILS_LOG_INFO("  Events: %zu", local_events);
        }
        const_cast<mpi::MPIUtils&>(mpi).barrier();
    }
}

}  // namespace dftracer::utils::call_tree
