// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/dstd/hash/hex64.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/trace/genesis/genesis.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <simdjson.h>

#include <algorithm>
#include <charconv>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::trace::genesis {

namespace {

std::optional<std::int64_t> suffix_number(std::string_view name,
                                          std::string_view prefix) {
    if (name.substr(0, prefix.size()) != prefix) return std::nullopt;
    std::int64_t v = 0;
    const char* first = name.data() + prefix.size();
    const char* last = name.data() + name.size();
    auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec != std::errc() || ptr != last || first == last) return std::nullopt;
    return v;
}

// `<stem>-<k>_chunk<c>.pfw.gz` -> k.
std::optional<std::int64_t> slice_index(dstd::StringView file_name) {
    const auto chunk = file_name.rfind("_chunk");
    const auto dash = file_name.rfind('-', chunk);
    if (chunk == dstd::StringView::npos || dash == dstd::StringView::npos)
        return std::nullopt;
    std::int64_t v = 0;
    const char* first = file_name.data() + dash + 1;
    const char* last = file_name.data() + chunk;
    auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec != std::errc() || ptr != last || first == last) return std::nullopt;
    return v;
}

struct DirFiles {
    bool summary = false;
    std::vector<std::pair<std::string, std::uint64_t>> traces;
    // Traces directly in the run directory, used when it has no compacted/.
    std::vector<std::pair<std::string, std::uint64_t>> run_traces;
    // raw/<set>/*.pfw.gz, used for a set whose compacted file is missing.
    std::map<std::string, std::vector<std::pair<std::string, std::uint64_t>>>
        raw;
    std::map<std::string, std::vector<std::string>> gpu_csvs;
};

void set_files(RunGroup& g,
               const std::vector<std::pair<std::string, std::uint64_t>>& fs) {
    g.files.clear();
    g.compressed_bytes = 0;
    for (const auto& [f, size] : fs) {
        g.files.push_back(f);
        g.compressed_bytes += size;
    }
}

// <app>/<system>[/<system>]/<input>/nodes_<N>/ppn_<M>
void keys_from_path(dstd::PathView dir, RunKeys& k) {
    const dstd::PathView input = dir.parent().parent();
    const dstd::PathView system = input.parent();
    dstd::PathView app = system.parent();
    if (app.filename() == system.filename()) app = app.parent();
    k.unique_input = std::string(input.filename());
    k.system = std::string(system.filename());
    k.app = std::string(app.filename());
}

std::string get_string(simdjson::dom::object o, std::string_view key) {
    std::string_view v;
    if (o[key].get(v) != simdjson::SUCCESS) return {};
    return std::string(v);
}

std::optional<std::int64_t> get_int(simdjson::dom::object o,
                                    std::string_view key) {
    std::int64_t v = 0;
    if (o[key].get(v) != simdjson::SUCCESS) return std::nullopt;
    return v;
}

// Time slices `-<k>` must run 1..max without gaps.
bool slices_complete(RunGroup& base, const DirFiles& files, Discovery& out) {
    set_files(base, files.traces);
    if (base.files.empty()) {
        out.skips.push_back({base.dir, "compacted", "no trace files"});
        return false;
    }
    std::vector<std::int64_t> idx;
    for (const auto& f : base.files) {
        const auto i = slice_index(dstd::PathView(f).filename());
        if (!i) {
            out.skips.push_back({base.dir, f, "unrecognized trace file name"});
            return false;
        }
        idx.push_back(*i);
    }
    std::sort(idx.begin(), idx.end());
    std::string missing;
    std::int64_t expect = 1;
    for (const auto i : idx) {
        if (i > expect) {
            if (!missing.empty()) missing += ',';
            missing += std::to_string(expect);
            if (i - 1 > expect) missing += "-" + std::to_string(i - 1);
        }
        expect = i + 1;
    }
    if (!missing.empty()) {
        out.skips.push_back(
            {base.dir, "compacted", "missing slices " + missing});
        return false;
    }
    return true;
}

void discover_v1(dstd::PathView dir, const DirFiles& files,
                 simdjson::dom::object s, RunGroup base, Discovery& out) {
    bool ok = true;
    if (s["ok"].get(ok) == simdjson::SUCCESS && !ok) {
        out.skips.push_back({base.dir, "summary.json", "\"ok\" is false"});
        return;
    }
    RunKeys k;
    keys_from_path(dir, k);
    if (auto app = get_string(s, "app"); !app.empty()) k.app = std::move(app);
    if (auto sys = get_string(s, "system"); !sys.empty())
        k.system = std::move(sys);
    const auto nodes = get_int(s, "nodes");
    const auto ppn = get_int(s, "ppn");
    simdjson::dom::array sets;
    if (!nodes || !ppn || s["sets"].get(sets) != simdjson::SUCCESS) {
        out.skips.push_back(
            {base.dir, "summary.json", "missing nodes, ppn or sets"});
        return;
    }
    k.nodes = *nodes;
    k.ppn = *ppn;
    std::vector<std::string> names;
    for (auto v : sets) {
        std::string_view set;
        if (v.get(set) == simdjson::SUCCESS) names.emplace_back(set);
    }
    std::sort(names.begin(), names.end());
    for (const auto& set : names) {
        const std::string file(
            ((dir / "compacted").view() / (set + ".pfw.gz")).str());
        const auto it =
            std::find_if(files.traces.begin(), files.traces.end(),
                         [&](const auto& t) { return t.first == file; });
        const auto raw = files.raw.find(set);
        if (it == files.traces.end() && raw == files.raw.end()) {
            out.skips.push_back(
                {base.dir, file, "set listed but file missing"});
            continue;
        }
        RunGroup g = base;
        if (it != files.traces.end())
            set_files(g, {*it});
        else
            set_files(g, raw->second);
        SetSpec spec;
        spec.keys = k;
        spec.keys.papi_set = set;
        if (const auto c = files.gpu_csvs.find(set); c != files.gpu_csvs.end())
            spec.gpu_csvs = c->second;
        g.sets.push_back(std::move(spec));
        out.groups.push_back(std::move(g));
    }
}

void discover_v2(dstd::PathView dir, const DirFiles& files,
                 simdjson::dom::object s, RunGroup base, Discovery& out) {
    RunKeys k;
    keys_from_path(dir, k);
    if (auto app = get_string(s, "application"); !app.empty())
        k.app = std::move(app);
    const auto nodes = get_int(s, "nodes");
    const auto ppn = get_int(s, "ppn");
    simdjson::dom::object runs;
    if (!nodes || !ppn || s["runs"].get(runs) != simdjson::SUCCESS) {
        out.skips.push_back(
            {base.dir, "summary.json", "missing nodes, ppn or runs"});
        return;
    }
    k.nodes = *nodes;
    k.ppn = *ppn;
    if (!slices_complete(base, files, out)) return;

    std::map<std::string, SetSpec> ordered;
    for (auto [set, run] : runs) {
        simdjson::dom::object r;
        if (run.get(r) != simdjson::SUCCESS) continue;
        bool completed = true;
        if (r["completed_ok"].get(completed) == simdjson::SUCCESS &&
            !completed) {
            out.skips.push_back(
                {base.dir, std::string(set), "completed_ok is false"});
            continue;
        }
        SetSpec spec;
        spec.keys = k;
        spec.keys.papi_set = std::string(set);
        simdjson::dom::array counters;
        if (r["papi_counters"].get(counters) == simdjson::SUCCESS) {
            for (auto c : counters) {
                std::string_view name;
                if (c.get(name) == simdjson::SUCCESS)
                    spec.papi_counters.emplace_back(name);
            }
        }
        std::sort(spec.papi_counters.begin(), spec.papi_counters.end());
        ordered.emplace(std::string(set), std::move(spec));
    }
    for (auto& [_, spec] : ordered) base.sets.push_back(std::move(spec));
    if (!base.sets.empty()) out.groups.push_back(std::move(base));
}

// No summary: the slices mix one execution per PAPI set, so the sets are
// taken from the counters each process reports (process_group).
void discover_path_only(dstd::PathView dir, const DirFiles& files,
                        const std::int64_t nodes, const std::int64_t ppn,
                        RunGroup base, Discovery& out) {
    if (!slices_complete(base, files, out)) return;
    SetSpec spec;
    keys_from_path(dir, spec.keys);
    spec.keys.nodes = nodes;
    spec.keys.ppn = ppn;
    base.sets.push_back(std::move(spec));
    base.sets_from_counters = true;
    out.groups.push_back(std::move(base));
}

void discover_dir(dstd::PathView root, dstd::PathView dir,
                  const DirFiles& files, Discovery& out) {
    const auto ppn = suffix_number(dir.filename(), "ppn_");
    const auto nodes = suffix_number(dir.parent().filename(), "nodes_");
    if (!ppn || !nodes) return;

    RunGroup base;
    base.dir = std::string(dir.str());
    // Relative to the resolved root, as std::filesystem::relative was.
    base.rel_dir = std::string(
        dstd::relative(common::unwrap(dstd::fs::weakly_canonical(dir)).view(),
                       common::unwrap(dstd::fs::weakly_canonical(root)).view())
            .str());

    const dstd::Path summary = dir / "summary.json";
    if (!files.summary) {
        discover_path_only(dir, files, *nodes, *ppn, std::move(base), out);
        return;
    }
    simdjson::dom::parser parser;
    simdjson::padded_string text;
    simdjson::dom::object s;
    if (simdjson::padded_string::load(summary.str()).get(text) !=
            simdjson::SUCCESS ||
        parser.parse(text).get(s) != simdjson::SUCCESS) {
        out.skips.push_back({base.dir, "summary.json", "unparsable JSON"});
        return;
    }
    base.summary_json = std::string(simdjson::minify(s));
    simdjson::dom::object runs;
    simdjson::dom::array sets;
    if (s["runs"].get(runs) == simdjson::SUCCESS) {
        discover_v2(dir, files, s, std::move(base), out);
    } else if (s["sets"].get(sets) == simdjson::SUCCESS) {
        discover_v1(dir, files, s, std::move(base), out);
    } else {
        out.skips.push_back(
            {base.dir, "summary.json", "neither sets nor runs key"});
    }
}

// Orders paths component by component, as std::filesystem::path does, so a
// separator sorts before every other byte.
struct ComponentLess {
    using is_transparent = void;
    static unsigned rank(char c) {
        return c == '/' ? 0u : static_cast<unsigned char>(c) + 1u;
    }
    bool operator()(dstd::StringView a, dstd::StringView b) const noexcept {
        const std::size_t n = std::min(a.size(), b.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (a[i] != b[i]) return rank(a[i]) < rank(b[i]);
        }
        return a.size() < b.size();
    }
};

using DirMap = dstd::Map<dstd::String, DirFiles, ComponentLess>;

DirFiles& files_of(DirMap& dirs, dstd::PathView dir) {
    const auto found = dirs.find(dir.str());
    if (found != dirs.end()) return found->second;
    return dirs.emplace(dstd::String(dir.str()), DirFiles{}).first->second;
}

}  // namespace

std::string run_id(const RunKeys& k) {
    const std::string key = k.app + "|" + k.system + "|" + k.unique_input +
                            "|" + std::to_string(k.nodes) + "|" +
                            std::to_string(k.ppn) + "|" + k.papi_set;
    return dstd::hash::format_hex64(dstd::hash::fnv1a_hash(key));
}

coro::CoroTask<Discovery> discover(coro::Scope& ctx,
                                   std::vector<std::string> roots) {
    Discovery out;
    const utilities::filesystem::PatternDirectoryScannerUtility scan;
    for (const auto& r : roots) {
        const dstd::Path root =
            common::unwrap(dstd::fs::absolute(dstd::PathView(r)));
        // A braced list inside the co_await expression crashes GCC 12.
        const std::vector<std::string> patterns{".pfw.gz", "summary.json",
                                                ".csv"};
        utilities::filesystem::PatternDirectoryScannerUtilityInput input(
            root.str(), patterns, true, true);
        const auto entries = co_await scan(ctx, input);
        DirMap dirs;
        for (const auto& e : entries) {
            const dstd::PathView path = e.path.view();
            const dstd::PathView parent = path.parent();
            if (path.filename() == "summary.json") {
                files_of(dirs, parent).summary = true;
            } else if (parent.filename() == "compacted") {
                files_of(dirs, parent.parent())
                    .traces.emplace_back(std::string(path.str()), e.size);
            } else if (path.extension() == ".gz" &&
                       parent.filename().starts_with("ppn_")) {
                // <run>/<stem>-<k>_chunk<c>.pfw.gz: slices kept in the run
                // directory itself rather than in compacted/.
                files_of(dirs, parent)
                    .run_traces.emplace_back(std::string(path.str()), e.size);
            } else if (path.extension() == ".gz" &&
                       parent.parent().filename() == "raw") {
                // <run>/raw/<set>/<file>.pfw.gz
                files_of(dirs, parent.parent().parent())
                    .raw[std::string(parent.filename())]
                    .emplace_back(std::string(path.str()), e.size);
            } else if (parent.filename() == "dftracer_service" &&
                       path.filename().starts_with("gpu_power_")) {
                // <run>/raw/<set>/dftracer_service/gpu_power_<host>.csv
                const dstd::PathView set = parent.parent();
                files_of(dirs, set.parent().parent())
                    .gpu_csvs[std::string(set.filename())]
                    .emplace_back(path.str());
            }
        }
        for (auto& [dir, files] : dirs) {
            if (files.traces.empty())
                files.traces = std::move(files.run_traces);
            std::sort(files.traces.begin(), files.traces.end());
            for (auto& [_, raw] : files.raw) std::sort(raw.begin(), raw.end());
            for (auto& [_, csvs] : files.gpu_csvs)
                std::sort(csvs.begin(), csvs.end());
            discover_dir(root.view(), dstd::PathView(dir), files, out);
        }
    }
    co_return out;
}

}  // namespace dftracer::utils::trace::genesis
