// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/domains/workload/dlio/trace_loader.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/result.h>
#include <yaml-cpp/yaml.h>

#include <string>

namespace dftracer::utils::domains::workload::dlio {

namespace {

void overlay_selector(const YAML::Node& root, const char* key,
                      EventSelector& sel) {
    const auto node = root[key];
    if (!node || !node.IsMap()) return;
    if (const auto cat = node["cat"]) {
        const auto text = cat.as<std::string>();
        sel.cat = dstd::String(text.data(), text.size());
    }
    if (const auto name = node["name"]) {
        const auto text = name.as<std::string>();
        sel.name = dstd::String(text.data(), text.size());
    }
}

}  // namespace

dstd::Result<void> load_event_map(dstd::StringView path,
                                  TraceLoaderOptions& options) {
    try {
        const YAML::Node root = YAML::LoadFile(std::string(path));
        if (!root.IsMap()) {
            return common::make_error<
                "dlio: event map {} must be a mapping of component names">(
                common::ErrorCode::InvalidArgument, path);
        }
        overlay_selector(root, "fetch_block", options.fetch_block);
        overlay_selector(root, "fetch_iter", options.fetch_iter);
        overlay_selector(root, "preprocess", options.preprocess);
        overlay_selector(root, "item", options.item);
    } catch (const YAML::Exception& e) {
        return common::make_error<"dlio: failed to parse event map {}: {}">(
            common::ErrorCode::InvalidArgument, path, e.what());
    }
    return {};
}

}  // namespace dftracer::utils::domains::workload::dlio
