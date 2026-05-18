#ifndef DFTRACER_UTILS_CALL_TREE_INTERNAL_TRACE_READER_H
#define DFTRACER_UTILS_CALL_TREE_INTERNAL_TRACE_READER_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/common/json/parser.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace dftracer::utils::call_tree {
namespace internal {

class CallTree;

using TraceCallback = std::function<bool(const std::string& json_line)>;

class TraceReader {
   public:
    TraceReader() = default;
    ~TraceReader() = default;

    bool read(const std::string& trace_file, CallTree& graph);
    bool read_multiple(const std::vector<std::string>& trace_files,
                       CallTree& graph);
    bool read_directory(const std::string& directory,
                        const std::string& pattern, CallTree& graph);

    bool process_trace_line(
        dftracer::utils::utilities::common::json::JsonParser& parser,
        CallTree& graph);
    bool process_trace_line(const std::string& line, CallTree& graph);
};

struct ReadCounts {
    std::size_t processed = 0;
    std::size_t filtered = 0;
};

// allowed_pids == nullptr disables filtering.
ReadCounts read_trace_file(
    const std::string& trace_file, CallTree& graph,
    const std::set<std::uint32_t>* allowed_pids = nullptr);

// Coroutine entry point. Drives utilities::reader::TraceReader::read_json
// inline so callers can fan out via CoroScope::spawn over multiple files.
coro::CoroTask<ReadCounts> read_trace_file_async(
    std::string trace_file, CallTree* graph,
    const std::set<std::uint32_t>* allowed_pids = nullptr);

}  // namespace internal
}  // namespace dftracer::utils::call_tree

#endif  // DFTRACER_UTILS_CALL_TREE_INTERNAL_TRACE_READER_H
