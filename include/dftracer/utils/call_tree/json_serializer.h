#ifndef DFTRACER_UTILS_CALL_TREE_JSON_SERIALIZER_H
#define DFTRACER_UTILS_CALL_TREE_JSON_SERIALIZER_H

#include <dftracer/utils/call_tree/internal/node.h>

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

namespace dftracer::utils::call_tree {
namespace internal {

/**
 * JsonSerializer - Serializes call tree nodes to JSON format
 *
 * Follows the Chrome Tracing format similar to DFTracer, with events
 * that are compatible with Perfetto and Chrome's about:tracing viewer.
 *
 * JSON format characteristics (from DFTracer):
 * - File starts with '[' and ends with ']' for JSON array format
 * - Each event is a single JSON line (JSON Lines format)
 * - Uses Chrome Tracing event format with fields:
 *   - id: Event sequence number
 *   - name: Function/event name
 *   - cat: Category
 *   - pid: Process ID
 *   - tid: Thread ID
 *   - ts: Timestamp in microseconds
 *   - dur: Duration in microseconds
 *   - ph: Phase ("X" for complete events, "M" for metadata)
 *   - args: Additional metadata/arguments
 */
class JsonSerializer {
   public:
    JsonSerializer();
    ~JsonSerializer() = default;

    /**
     * Initialize the serializer and write JSON array opening bracket
     * @param buffer Output buffer for serialization
     * @param hostname_hash Optional hostname hash for identification
     * @return Number of bytes written
     */
    size_t initialize(char* buffer, const std::string& hostname_hash = "");

    /**
     * Serialize a call tree node as a complete event (duration event)
     * @param buffer Output buffer
     * @param index Event sequence number
     * @param node Call tree node to serialize
     * @param process_id Process ID
     * @param thread_id Thread ID
     * @return Number of bytes written
     */
    size_t serialize_node(char* buffer, int index, const CallTreeNode& node,
                          std::uint32_t process_id, std::uint32_t thread_id);

    /**
     * Serialize metadata event
     * @param buffer Output buffer
     * @param name Metadata name
     * @param value Metadata value
     * @param ph Phase type (typically "M" for metadata)
     * @param process_id Process ID
     * @param thread_id Thread ID
     * @param is_string Whether value is a string (needs quotes)
     * @return Number of bytes written
     */
    size_t serialize_metadata(char* buffer, const std::string& name,
                              const std::string& value, const char* ph,
                              std::uint32_t process_id, std::uint32_t thread_id,
                              bool is_string = true);

    /**
     * Finalize serialization and write closing bracket
     * @param buffer Output buffer
     * @param write_bracket Whether to write the closing ']' bracket
     * @return Number of bytes written
     */
    size_t finalize(char* buffer, bool write_bracket = true);

   private:
    /**
     * Convert node arguments/metadata to JSON string
     * @param args Map of key-value arguments
     * @param stream Output string stream
     * @return True if metadata was present, false otherwise
     */
    bool convert_args_to_json(const ArgsMap& args, std::stringstream& stream);

    std::string hostname_hash_;
};

}  // namespace internal
}  // namespace dftracer::utils::call_tree

#endif  // DFTRACER_UTILS_CALL_TREE_JSON_SERIALIZER_H
