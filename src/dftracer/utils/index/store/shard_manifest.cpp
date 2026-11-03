// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/store/shard_manifest.h>
#include <simdjson.h>
#include <unistd.h>

#include <cstdio>

namespace dftracer::utils::index::store {

namespace {

void append_escaped(dstd::String& out, dstd::StringView s) {
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

dstd::String to_json(const IndexShardManifest& manifest) {
    dstd::String out;
    out += "{\n  \"format_version\": ";
    out += dstd::to_string(manifest.format_version.value());
    out += ",\n  \"shards\": [";
    for (std::size_t i = 0; i < manifest.shards.size(); ++i) {
        const IndexShardEntry& e = manifest.shards[i];
        out += (i == 0) ? "\n    {" : ",\n    {";
        out += "\n      \"path\": ";
        append_escaped(out, e.path);
        out += ",\n      \"num_files\": ";
        out += dstd::to_string(e.num_files);
        out += ",\n      \"num_events\": ";
        out += dstd::to_string(e.num_events);
        out += "\n    }";
    }
    out += manifest.shards.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

IndexShardManifest parse_manifest(dstd::StringView json) {
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto err = parser.parse(simdjson::padded_string(json)).get(doc);
    if (err) {
        throw common::DFTUtilsException(
            common::ErrorCode::Parse,
            dstd::String("shard manifest parse failed: ") +
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
        dstd::StringView path;
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

void write_shard_manifest(dstd::StringView root,
                          const IndexShardManifest& manifest) {
    // A directory that cannot be made fails the write below.
    (void)dstd::fs::create_directories(dstd::PathView(root));

    const dstd::Path final_path{shard_manifest_path(root)};
    const dstd::Path tmp_path =
        dstd::PathView(root) /
        common::str_cat(SHARD_MANIFEST, ".tmp.", ::getpid());

    const dstd::String body = to_json(manifest);
    if (!dstd::fs::write_file(tmp_path.view(), body)) {
        (void)dstd::fs::remove(tmp_path.view());
        throw common::DFTUtilsException(
            common::ErrorCode::Io,
            common::str_cat("failed to write shard manifest: ",
                            tmp_path.str()));
    }

    const auto renamed = dstd::fs::rename(tmp_path.view(), final_path.view());
    if (!renamed) {
        (void)dstd::fs::remove(tmp_path.view());
        throw common::DFTUtilsException(
            common::ErrorCode::Io,
            common::str_cat("failed to publish shard manifest to ",
                            final_path.str(), ": ", renamed.error().message()));
    }
}

dstd::Optional<IndexShardManifest> read_shard_manifest(dstd::StringView root) {
    const dstd::Path path{shard_manifest_path(root)};
    // An unreadable path reads as no manifest.
    const auto found = dstd::fs::exists(path.view());
    if (!found || !*found) return dstd::nullopt;

    simdjson::padded_string contents;
    auto load_err = simdjson::padded_string::load(path.str()).get(contents);
    if (load_err) {
        throw common::DFTUtilsException(
            common::ErrorCode::Io,
            common::str_cat("failed to read shard manifest ", path.str(), ": ",
                            simdjson::error_message(load_err)));
    }
    return parse_manifest(dstd::StringView(contents.data(), contents.size()));
}

bool has_shard_manifest(dstd::StringView root) {
    const auto found =
        dstd::fs::exists(dstd::PathView(shard_manifest_path(root)));
    return found && *found;
}

dstd::String resolve_shard_set_root(dstd::StringView target) {
    if (target.empty()) return "";
    const dstd::String root = internal::normalize_index_root(target);
    if (has_shard_manifest(root)) return root;
    return "";
}

}  // namespace dftracer::utils::index::store
