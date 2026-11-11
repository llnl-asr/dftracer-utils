// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/store/shard_manifest.h>
#include <simdjson.h>
#include <unistd.h>

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
                    out +=
                        dstd::fmt<"\\u{:04x}">(static_cast<unsigned char>(c));
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

}  // namespace

dstd::String to_json(const IndexShardManifest& manifest) {
    if (!manifest.format_version)
        dstd::fatal("shard manifest: the format version is not set");
    dstd::String out;
    out += "{\n  \"format_version\": ";
    out += dstd::to_string(*manifest.format_version);
    out += ",\n  \"shards\": [";
    for (std::size_t i = 0; i < manifest.shards.size(); ++i) {
        const IndexShardEntry& e = manifest.shards[i];
        out += (i == 0) ? "\n    {" : ",\n    {";
        out += "\n      \"path\": ";
        append_escaped(out, e.path);
        out += ",\n      \"num_files\": ";
        out += dstd::to_string(e.file_count);
        out += ",\n      \"num_events\": ";
        out += dstd::to_string(e.event_count);
        out += "\n    }";
    }
    out += manifest.shards.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

dstd::Result<IndexShardManifest> parse_manifest(dstd::StringView json) {
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto err = parser.parse(simdjson::padded_string(json)).get(doc);
    if (err) {
        return common::make_error<"shard manifest parse failed: {}">(
            common::ErrorCode::Parse, simdjson::error_message(err));
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
            return common::make_error(common::ErrorCode::Parse,
                                      "shard manifest entry missing path");
        }
        e.path.assign(path);

        std::uint64_t u64 = 0;
        if (s["num_files"].get(u64) == simdjson::SUCCESS) e.file_count = u64;
        if (s["num_events"].get(u64) == simdjson::SUCCESS) e.event_count = u64;
        manifest.shards.push_back(std::move(e));
    }
    return manifest;
}

dstd::Result<void> write_shard_manifest(dstd::StringView root,
                                        const IndexShardManifest& manifest) {
    // A directory that cannot be made fails the write below.
    dstd::fs::create_directories(dstd::PathView(root)).discard();

    const dstd::Path final_path{shard_manifest_path(root)};
    const dstd::Path tmp_path =
        dstd::PathView(root) /
        dstd::fmt<"{}.tmp.{}">(SHARD_MANIFEST, ::getpid());

    const dstd::String body = to_json(manifest);
    if (!dstd::fs::write_file(tmp_path.view(), body)) {
        dstd::fs::remove(tmp_path.view()).discard();
        return common::make_error<"failed to write shard manifest: {}">(
            common::ErrorCode::Io, tmp_path.str());
    }

    const auto renamed = dstd::fs::rename(tmp_path.view(), final_path.view());
    if (!renamed) {
        dstd::fs::remove(tmp_path.view()).discard();
        return common::make_error<"failed to publish shard manifest to {}: {}">(
            common::ErrorCode::Io, final_path.str(), renamed.error().message());
    }
    return {};
}

dstd::Result<dstd::Optional<IndexShardManifest>> read_shard_manifest(
    dstd::StringView root) {
    const dstd::Path path{shard_manifest_path(root)};
    // An unreadable path reads as no manifest.
    const auto found = dstd::fs::exists(path.view());
    if (!found.has_value() || !*found) return dstd::nullopt;

    simdjson::padded_string contents;
    auto load_err = simdjson::padded_string::load(path.str()).get(contents);
    if (load_err) {
        return common::make_error<"failed to read shard manifest {}: {}">(
            common::ErrorCode::Io, path.str(),
            simdjson::error_message(load_err));
    }
    auto parsed =
        parse_manifest(dstd::StringView(contents.data(), contents.size()));
    if (!parsed) return parsed.error();
    return dstd::Optional<IndexShardManifest>(std::move(*parsed));
}

bool has_shard_manifest(dstd::StringView root) {
    const auto found =
        dstd::fs::exists(dstd::PathView(shard_manifest_path(root)));
    return found.has_value() && *found;
}

dstd::String resolve_shard_set_root(dstd::StringView target) {
    if (target.empty()) return "";
    const dstd::String root = normalize_index_root(target);
    if (has_shard_manifest(root)) return root;
    return "";
}

}  // namespace dftracer::utils::index::store
