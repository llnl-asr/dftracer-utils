// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
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
    out += std::to_string(manifest.format_version.value());
    out += ",\n  \"shards\": [";
    for (std::size_t i = 0; i < manifest.shards.size(); ++i) {
        const IndexShardEntry& e = manifest.shards[i];
        out += (i == 0) ? "\n    {" : ",\n    {";
        out += "\n      \"path\": ";
        append_escaped(out, e.path);
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
        throw common::DFTUtilsException(
            common::ErrorCode::Parse,
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
            throw common::DFTUtilsException(
                common::ErrorCode::Parse, "shard manifest entry missing path");
        }
        e.path.assign(path);

        std::uint64_t u64 = 0;
        if (s["num_files"].get(u64) == simdjson::SUCCESS) e.num_files = u64;
        if (s["num_events"].get(u64) == simdjson::SUCCESS) e.num_events = u64;
        manifest.shards.push_back(std::move(e));
    }
    return manifest;
}

void write_shard_manifest(const std::string& root,
                          const IndexShardManifest& manifest) {
    std::error_code ec;
    common::fs::create_directories(root, ec);

    const common::fs::path final_path = shard_manifest_path(root);
    const common::fs::path tmp_path =
        common::fs::path(root) /
        (std::string(SHARD_MANIFEST) + ".tmp." + std::to_string(::getpid()));

    const std::string body = to_json(manifest);
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        out.flush();
        if (!out) {
            common::fs::remove(tmp_path, ec);
            throw common::DFTUtilsException(
                common::ErrorCode::Io,
                "failed to write shard manifest: " + tmp_path.string());
        }
    }

    common::fs::rename(tmp_path, final_path, ec);
    if (ec) {
        common::fs::remove(tmp_path, ec);
        throw common::DFTUtilsException(common::ErrorCode::Io,
                                        "failed to publish shard manifest to " +
                                            final_path.string() + ": " +
                                            ec.message());
    }
}

std::optional<IndexShardManifest> read_shard_manifest(const std::string& root) {
    const common::fs::path path = shard_manifest_path(root);
    std::error_code ec;
    if (!common::fs::exists(path, ec)) return std::nullopt;

    simdjson::padded_string contents;
    auto load_err = simdjson::padded_string::load(path.string()).get(contents);
    if (load_err) {
        throw common::DFTUtilsException(common::ErrorCode::Io,
                                        "failed to read shard manifest " +
                                            path.string() + ": " +
                                            simdjson::error_message(load_err));
    }
    return parse_manifest(std::string_view(contents.data(), contents.size()));
}

bool has_shard_manifest(const std::string& root) {
    std::error_code ec;
    return common::fs::exists(shard_manifest_path(root), ec);
}

std::string resolve_shard_set_root(const std::string& target) {
    if (target.empty()) return "";
    const std::string root = internal::normalize_index_root(target);
    if (has_shard_manifest(root)) return root;
    return "";
}

}  // namespace dftracer::utils::index::store
