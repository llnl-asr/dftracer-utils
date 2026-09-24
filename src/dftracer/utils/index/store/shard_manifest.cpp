#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/index/store/shard_manifest.h>
#include <simdjson.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace dftracer::utils::index::store {

namespace {

void append_escaped(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

}  // namespace

std::string to_json(const IndexShardManifest& manifest) {
    std::string out;
    out += "{\n  \"format_version\": ";
    out += std::to_string(manifest.format_version);
    out += ",\n  \"shards\": [";
    for (std::size_t i = 0; i < manifest.shards.size(); ++i) {
        const IndexShardEntry& e = manifest.shards[i];
        out += (i == 0) ? "\n    {" : ",\n    {";
        out += "\n      \"path\": ";
        append_escaped(out, e.path);
        out += ",\n      \"file_id_min\": ";
        out += std::to_string(e.file_id_min);
        out += ",\n      \"file_id_max\": ";
        out += std::to_string(e.file_id_max);
        out += ",\n      \"num_files\": ";
        out += std::to_string(e.num_files);
        out += ",\n      \"num_events\": ";
        out += std::to_string(e.num_events);
        out += "\n    }";
    }
    out += manifest.shards.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

IndexShardManifest parse_manifest(std::string_view json) {
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto err = parser.parse(simdjson::padded_string(json)).get(doc);
    if (err) {
        throw DFTUtilsException(ErrorCode::PARSE,
                                std::string("shard manifest parse failed: ") +
                                    simdjson::error_message(err));
    }

    IndexShardManifest manifest;
    std::uint64_t schema = 0;
    if (doc["format_version"].get(schema) == simdjson::SUCCESS)
        manifest.format_version = static_cast<std::uint32_t>(schema);

    simdjson::dom::array shards;
    if (doc["shards"].get(shards) != simdjson::SUCCESS) return manifest;

    for (simdjson::dom::element s : shards) {
        IndexShardEntry e;
        std::string_view path;
        if (s["path"].get(path) != simdjson::SUCCESS) {
            throw DFTUtilsException(ErrorCode::PARSE,
                                    "shard manifest entry missing path");
        }
        e.path.assign(path);

        std::int64_t i64 = 0;
        std::uint64_t u64 = 0;
        if (s["file_id_min"].get(i64) == simdjson::SUCCESS) e.file_id_min = i64;
        if (s["file_id_max"].get(i64) == simdjson::SUCCESS) e.file_id_max = i64;
        if (s["num_files"].get(u64) == simdjson::SUCCESS) e.num_files = u64;
        if (s["num_events"].get(u64) == simdjson::SUCCESS) e.num_events = u64;
        manifest.shards.push_back(std::move(e));
    }
    return manifest;
}

void write_shard_manifest(const std::string& dir,
                          const IndexShardManifest& manifest) {
    std::error_code ec;
    fs::create_directories(dir, ec);

    const fs::path final_path = fs::path(dir) / SHARD_MANIFEST_FILENAME;
    const fs::path tmp_path =
        fs::path(dir) / (std::string(SHARD_MANIFEST_FILENAME) + ".tmp." +
                         std::to_string(::getpid()));

    const std::string body = to_json(manifest);
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        out.flush();
        if (!out) {
            fs::remove(tmp_path, ec);
            throw DFTUtilsException(
                ErrorCode::IO,
                "failed to write shard manifest: " + tmp_path.string());
        }
    }

    fs::rename(tmp_path, final_path, ec);
    if (ec) {
        fs::remove(tmp_path, ec);
        throw DFTUtilsException(ErrorCode::IO,
                                "failed to publish shard manifest to " +
                                    final_path.string() + ": " + ec.message());
    }
}

std::optional<IndexShardManifest> read_shard_manifest(const std::string& dir) {
    const fs::path path = fs::path(dir) / SHARD_MANIFEST_FILENAME;
    std::error_code ec;
    if (!fs::exists(path, ec)) return std::nullopt;

    simdjson::padded_string contents;
    auto load_err = simdjson::padded_string::load(path.string()).get(contents);
    if (load_err) {
        throw DFTUtilsException(
            ErrorCode::IO, "failed to read shard manifest " + path.string() +
                               ": " + simdjson::error_message(load_err));
    }
    return parse_manifest(std::string_view(contents.data(), contents.size()));
}

bool has_shard_manifest(const std::string& dir) {
    std::error_code ec;
    return fs::exists(fs::path(dir) / SHARD_MANIFEST_FILENAME, ec);
}

std::string resolve_shard_set_root(const std::string& target) {
    if (has_shard_manifest(target)) return target;
    const std::string nested = (fs::path(target) / SHARD_SET_DIRNAME).string();
    if (has_shard_manifest(nested)) return nested;
    return "";
}

}  // namespace dftracer::utils::index::store
