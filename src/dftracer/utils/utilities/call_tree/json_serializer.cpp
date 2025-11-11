#include <dftracer/utils/call_tree/json_serializer.h>

#include <cstdio>
#include <cstring>
#include <set>

namespace dftracer::utils::call_tree {
namespace internal {

JsonSerializer::JsonSerializer() : hostname_hash_("") {}

size_t JsonSerializer::initialize(char* buffer,
                                  const std::string& hostname_hash) {
    hostname_hash_ = hostname_hash;
    // Write opening bracket for JSON array (Chrome Tracing format requirement)
    buffer[0] = '[';
    buffer[1] = '\n';
    return 2;
}

bool JsonSerializer::convert_args_to_json(
    const std::unordered_map<std::string, std::string>& args,
    std::stringstream& stream) {
    if (args.empty()) {
        return false;
    }

    // Known fields that should always be strings (hash values, etc.)
    const std::set<std::string> string_fields = {"hhash", "fhash", "exec_hash",
                                                 "cmd_hash", "hostname_hash"};

    size_t count = 0;
    for (const auto& [key, value] : args) {
        // Add comma separator if not first element
        if (count > 0) {
            stream << ",";
        }

        // Check if this field should always be a string
        bool force_string = (string_fields.find(key) != string_fields.end());

        // Check if value looks like a pure number
        // To be safe, only treat it as a number if:
        // 1. Not a known string field
        // 2. It doesn't contain any letters (handles hex strings like
        // "df57e0a251b84b54")
        // 3. It successfully parses as a number
        // 4. The entire string was consumed during parsing
        bool is_number = false;
        if (!force_string && !value.empty()) {
            // First check: no alphabetic characters
            bool has_alpha = false;
            for (char c : value) {
                if (std::isalpha(c)) {
                    has_alpha = true;
                    break;
                }
            }

            // Only try to parse as number if no alphabetic chars
            if (!has_alpha && (std::isdigit(value[0]) || value[0] == '-' ||
                               value[0] == '+')) {
                char* end;
                // Try integer parse
                std::strtoll(value.c_str(), &end, 10);
                if (end && *end == '\0') {
                    is_number = true;
                } else {
                    // Try float parse
                    std::strtod(value.c_str(), &end);
                    if (end && *end == '\0') {
                        is_number = true;
                    }
                }
            }
        }

        // Format as JSON key-value pair
        stream << "\"" << key << "\":";
        if (is_number) {
            stream << value;
        } else {
            // Escape special characters in string values
            stream << "\"";
            for (char c : value) {
                switch (c) {
                    case '"':
                        stream << "\\\"";
                        break;
                    case '\\':
                        stream << "\\\\";
                        break;
                    case '\n':
                        stream << "\\n";
                        break;
                    case '\r':
                        stream << "\\r";
                        break;
                    case '\t':
                        stream << "\\t";
                        break;
                    default:
                        stream << c;
                        break;
                }
            }
            stream << "\"";
        }

        count++;
    }

    return true;
}

size_t JsonSerializer::serialize_node(char* buffer, int index,
                                      const CallTreeNode& node,
                                      std::uint32_t process_id,
                                      std::uint32_t thread_id) {
    size_t written_size = 0;

    // Get node data
    const auto& args = node.get_args();

    // Build args JSON string if present
    std::stringstream args_stream;
    bool has_args = convert_args_to_json(args, args_stream);

    // Build complete args object including hostname hash and metadata
    std::stringstream all_args;

    // Check if args already has hhash, if not add it
    bool has_hhash = args.find("hhash") != args.end();
    if (!has_hhash && !hostname_hash_.empty()) {
        all_args << "\"hhash\":\"" << hostname_hash_ << "\"";
    }

    // Check if args already has level, if not add it
    bool has_level = args.find("level") != args.end();
    if (!has_level) {
        if (all_args.str().size() > 0) all_args << ",";
        all_args << "\"level\":" << node.get_level();
    }

    // Add parent_id if not root and not already in args
    bool has_parent = args.find("parent_id") != args.end();
    if (node.get_parent_id() != 0 && !has_parent) {
        if (all_args.str().size() > 0) all_args << ",";
        all_args << "\"parent_id\":" << node.get_parent_id();
    }

    // Add custom args if present
    if (has_args) {
        if (all_args.str().size() > 0) all_args << ",";
        all_args << args_stream.str();
    }

    // Format as Chrome Tracing complete event (phase "X")
    // Following DFTracer's format exactly:
    // {"id":%d,"name":"%s","cat":"%s","pid":%d,"tid":%lu,"ts":%llu,"dur":%llu,"ph":"X","args":{...}}
    written_size = std::snprintf(
        buffer,
        16384,  // Large buffer size to handle long strings
        R"({"id":%d,"name":"%s","cat":"%s","pid":%u,"tid":%u,"ts":%llu,"dur":%llu,"ph":"X","args":{%s}})",
        index, node.get_name().c_str(), node.get_category().c_str(), process_id,
        thread_id, static_cast<unsigned long long>(node.get_start_time()),
        static_cast<unsigned long long>(node.get_duration()),
        all_args.str().c_str());

    // Add newline terminator
    if (written_size > 0) {
        buffer[written_size++] = '\n';
        buffer[written_size] = '\0';
    }

    return written_size;
}

size_t JsonSerializer::serialize_metadata(char* buffer, const std::string& name,
                                          const std::string& value,
                                          const char* ph,
                                          std::uint32_t process_id,
                                          std::uint32_t thread_id,
                                          bool is_string) {
    size_t written_size = 0;

    // Format metadata event (phase "M")
    // Following DFTracer's format:
    // {"name":"%s","cat":"dftracer","pid":%d,"tid":%lu,"ph":"M","args":{"hhash":"%s","name":"%s","value":"%s"}}

    if (is_string) {
        written_size = std::snprintf(
            buffer, 8192,
            R"({"name":"%s","cat":"call_tree","pid":%u,"tid":%u,"ph":"%s","args":{"hhash":"%s","name":"%s","value":"%s"}})",
            ph, process_id, thread_id, ph, hostname_hash_.c_str(), name.c_str(),
            value.c_str());
    } else {
        written_size = std::snprintf(
            buffer, 8192,
            R"({"name":"%s","cat":"call_tree","pid":%u,"tid":%u,"ph":"%s","args":{"hhash":"%s","name":"%s","value":%s}})",
            ph, process_id, thread_id, ph, hostname_hash_.c_str(), name.c_str(),
            value.c_str());
    }

    // Add newline terminator
    if (written_size > 0) {
        buffer[written_size++] = '\n';
        buffer[written_size] = '\0';
    }

    return written_size;
}

size_t JsonSerializer::finalize(char* buffer, bool write_bracket) {
    if (write_bracket) {
        // Write closing bracket for JSON array
        buffer[0] = ']';
        buffer[1] = '\n';
        return 2;
    }
    return 0;
}

}  // namespace internal
}  // namespace dftracer::utils::call_tree
