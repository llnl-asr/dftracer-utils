#include <dftracer/utils/call_tree/json_serializer.h>
#include <dftracer/utils/utilities/composites/dft/args_map.h>

#include <cstdio>
#include <cstring>
#include <set>

namespace dftracer::utils::call_tree {
namespace internal {

using dftracer::utils::utilities::composites::dft::ArgsValueProxy;

JsonSerializer::JsonSerializer() : hostname_hash_("") {}

size_t JsonSerializer::initialize(char* buffer,
                                  const std::string& hostname_hash) {
    hostname_hash_ = hostname_hash;
    buffer[0] = '[';
    buffer[1] = '\n';
    return 2;
}

bool JsonSerializer::convert_args_to_json(const ArgsMap& args,
                                          std::stringstream& stream) {
    if (!args) return false;

    static const std::set<std::string_view, std::less<>> string_fields = {
        "hhash", "fhash", "exec_hash", "cmd_hash", "hostname_hash"};

    size_t count = 0;
    bool any = false;
    args.for_each_member([&](std::string_view key, ArgsValueProxy v) {
        if (count > 0) stream << ",";
        count++;
        any = true;

        stream << "\"" << key << "\":";

        if (v.is_string()) {
            std::string sv = v.get<std::string>();
            bool force_string = string_fields.find(key) != string_fields.end();
            bool is_number = false;
            if (!force_string && !sv.empty()) {
                bool has_alpha = false;
                for (char c : sv) {
                    if (std::isalpha(static_cast<unsigned char>(c))) {
                        has_alpha = true;
                        break;
                    }
                }
                if (!has_alpha &&
                    (std::isdigit(static_cast<unsigned char>(sv[0])) ||
                     sv[0] == '-' || sv[0] == '+')) {
                    char* end;
                    std::strtoll(sv.c_str(), &end, 10);
                    if (end && *end == '\0')
                        is_number = true;
                    else {
                        std::strtod(sv.c_str(), &end);
                        if (end && *end == '\0') is_number = true;
                    }
                }
            }
            if (is_number) {
                stream << sv;
            } else {
                stream << "\"";
                for (char c : sv) {
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
        } else if (v.is_uint()) {
            stream << v.get<std::uint64_t>();
        } else if (v.is_int()) {
            stream << v.get<std::int64_t>();
        } else if (v.is_number()) {
            stream << v.get<double>();
        } else if (v.is_bool()) {
            stream << (v.get<bool>() ? "true" : "false");
        } else {
            stream << "null";
        }
    });

    return any;
}

size_t JsonSerializer::serialize_node(char* buffer, int index,
                                      const CallTreeNode& node,
                                      std::uint32_t process_id,
                                      std::uint32_t thread_id) {
    const auto& args = node.get_args();

    std::stringstream args_stream;
    bool has_args = convert_args_to_json(args, args_stream);

    std::stringstream all_args;

    bool has_hhash = args["hhash"].exists();
    if (!has_hhash && !hostname_hash_.empty()) {
        all_args << "\"hhash\":\"" << hostname_hash_ << "\"";
    }

    bool has_level = args["level"].exists();
    if (!has_level) {
        if (all_args.str().size() > 0) all_args << ",";
        all_args << "\"level\":" << node.get_level();
    }

    bool has_parent = args["parent_id"].exists();
    if (node.get_parent_id() != 0 && !has_parent) {
        if (all_args.str().size() > 0) all_args << ",";
        all_args << "\"parent_id\":" << node.get_parent_id();
    }

    if (has_args) {
        if (all_args.str().size() > 0) all_args << ",";
        all_args << args_stream.str();
    }

    auto nm = node.get_name();
    auto ct = node.get_category();
    size_t written_size = std::snprintf(
        buffer, 16384,
        R"({"id":%d,"name":"%.*s","cat":"%.*s","pid":%u,"tid":%u,"ts":%llu,"dur":%llu,"ph":"X","args":{%s}})",
        index, static_cast<int>(nm.size()), nm.data(),
        static_cast<int>(ct.size()), ct.data(), process_id, thread_id,
        static_cast<unsigned long long>(node.get_start_time()),
        static_cast<unsigned long long>(node.get_duration()),
        all_args.str().c_str());

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

    if (written_size > 0) {
        buffer[written_size++] = '\n';
        buffer[written_size] = '\0';
    }

    return written_size;
}

size_t JsonSerializer::finalize(char* buffer, bool write_bracket) {
    if (write_bracket) {
        buffer[0] = ']';
        buffer[1] = '\n';
        return 2;
    }
    return 0;
}

}  // namespace internal
}  // namespace dftracer::utils::call_tree
