// SPDX-License-Identifier: MIT
#include <dftracer/utils/cli/plugin_info.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

namespace dftracer::utils::cli {
namespace {

dstd::String join_or_none(dstd::Span<const dstd::String> names) {
    if (names.empty()) return "(none)";
    dstd::String out;
    dstd::fmt_append_join<", ">(out, names);
    return out;
}

// One line per fact, so the output stays greppable.
void line(dstd::String& out, dstd::StringView key, dstd::StringView value) {
    out += "  ";
    out += key;
    out += ": ";
    out.append(value.data(), value.size());
    out += '\n';
}

}  // namespace

dstd::String describe_plugin(const plugins::host::Plugins::PluginInfo& info) {
    dstd::String out;
    out += "plugin: ";
    out.append(info.path.data(), info.path.size());
    out += '\n';
    line(out, "name", info.name.empty() ? "(unnamed)" : info.name);
    line(out, "version", info.version.empty() ? "(none)" : info.version);
    out += dstd::fmt<"  abi_version: {}\n">(info.abi_version);
    line(out, "plan_duql", info.has_plan_duql ? "yes" : "no");
    line(out, "fold", info.has_fold ? "yes" : "no");
    line(out, "provides", join_or_none(info.provides));
    line(out, "consumes", join_or_none(info.consumes));
    line(out, "ops", join_or_none(info.ops));
    line(out, "udfs", join_or_none(info.udfs));
    line(out, "udafs", join_or_none(info.udafs));
    line(out, "states", join_or_none(info.states));
    line(out, "providers", join_or_none(info.providers));
    line(out, "nodes", join_or_none(info.nodes));
    line(out, "index_extensions", join_or_none(info.index_extensions));
    line(out, "file_formats", join_or_none(info.file_formats));
    line(out, "sinks", join_or_none(info.sinks));
    // "(all columns)" means the plugin declared no projection, so it is handed
    // every batch column - not that it reads nothing.
    line(out, "reads",
         info.reads.empty() ? "(all columns)" : join_or_none(info.reads));
    if (info.config_keys.empty()) {
        line(out, "config", "(undeclared)");
    } else {
        for (const auto& key : info.config_keys) line(out, "config", key);
    }
    out += '\n';
    return out;
}

void print_plugin_info(std::FILE* out,
                       const plugins::host::Plugins::PluginInfo& info) {
    const dstd::String text = describe_plugin(info);
    std::fwrite(text.data(), 1, text.size(), out);
}

}  // namespace dftracer::utils::cli
