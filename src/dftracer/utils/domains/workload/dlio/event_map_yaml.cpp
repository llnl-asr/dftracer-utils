// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/domains/workload/dlio/trace_loader.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/yaml/document.h>

namespace dftracer::utils::domains::workload::dlio {

namespace {

using yaml::Document;
using yaml::Value;

dstd::Result<void> overlay_field(const Value& selector, const char* field,
                                 dstd::StringView component,
                                 dstd::StringView path, dstd::String& out) {
    const auto node = selector.find(field);
    if (!node) return {};
    const auto text = node->as_string();
    if (!text) {
        return common::make_error<"dlio: event map {}: {}.{} must be a string">(
            common::ErrorCode::InvalidArgument, path, component, field);
    }
    out = dstd::String(*text);
    return {};
}

dstd::Result<void> overlay_selector(const Value& root, const char* key,
                                    dstd::StringView path, EventSelector& sel) {
    const auto node = root.find(key);
    if (!node || !node->is_object()) return {};
    if (auto ok = overlay_field(*node, "cat", key, path, sel.cat); !ok)
        return ok;
    return overlay_field(*node, "name", key, path, sel.name);
}

}  // namespace

dstd::Result<void> load_event_map(dstd::StringView path,
                                  TraceLoaderOptions& options) {
    const auto document = Document::load_file(path);
    if (!document) return document.error();
    const Value root = document->root();
    if (!root.is_object()) {
        return common::make_error<
            "dlio: event map {} must be a mapping of component names">(
            common::ErrorCode::InvalidArgument, path);
    }
    if (auto ok =
            overlay_selector(root, "fetch_block", path, options.fetch_block);
        !ok)
        return ok;
    if (auto ok =
            overlay_selector(root, "fetch_iter", path, options.fetch_iter);
        !ok)
        return ok;
    if (auto ok =
            overlay_selector(root, "preprocess", path, options.preprocess);
        !ok)
        return ok;
    return overlay_selector(root, "item", path, options.item);
}

}  // namespace dftracer::utils::domains::workload::dlio
