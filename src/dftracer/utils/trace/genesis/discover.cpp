#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/hash/fnv1a.h>
#include <dftracer/utils/core/common/hash/hex64.h>
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
std::optional<std::int64_t> slice_index(const std::string& file_name) {
    const auto chunk = file_name.rfind("_chunk");
    const auto dash = file_name.rfind('-', chunk);
    if (chunk == std::string::npos || dash == std::string::npos)
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
void keys_from_path(const fs::path& dir, RunKeys& k) {
    const fs::path input = dir.parent_path().parent_path();
    const fs::path system = input.parent_path();
    fs::path app = system.parent_path();
    if (app.filename() == system.filename()) app = app.parent_path();
    k.unique_input = input.filename().string();
    k.system = system.filename().string();
    k.app = app.filename().string();
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
        const auto i = slice_index(fs::path(f).filename().string());
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

void discover_v1(const fs::path& dir, const DirFiles& files,
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
        const std::string file =
            (dir / "compacted" / (set + ".pfw.gz")).string();
        const auto it =
            std::find_if(files.traces.begin(), files.traces.end(),
                         [&](const auto& t) { return t.first == file; });
        if (it == files.traces.end()) {
            out.skips.push_back(
                {base.dir, file, "set listed but file missing"});
            continue;
        }
        RunGroup g = base;
        set_files(g, {*it});
        SetSpec spec;
        spec.keys = k;
        spec.keys.papi_set = set;
        if (const auto c = files.gpu_csvs.find(set); c != files.gpu_csvs.end())
            spec.gpu_csvs = c->second;
        g.sets.push_back(std::move(spec));
        out.groups.push_back(std::move(g));
    }
}

void discover_v2(const fs::path& dir, const DirFiles& files,
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
void discover_path_only(const fs::path& dir, const DirFiles& files,
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

void discover_dir(const fs::path& root, const fs::path& dir,
                  const DirFiles& files, Discovery& out) {
    const auto ppn = suffix_number(dir.filename().string(), "ppn_");
    const auto nodes =
        suffix_number(dir.parent_path().filename().string(), "nodes_");
    if (!ppn || !nodes) return;

    RunGroup base;
    base.dir = dir.string();
    base.rel_dir = fs::relative(dir, root).string();

    const fs::path summary = dir / "summary.json";
    if (!files.summary) {
        discover_path_only(dir, files, *nodes, *ppn, std::move(base), out);
        return;
    }
    simdjson::dom::parser parser;
    simdjson::padded_string text;
    simdjson::dom::object s;
    if (simdjson::padded_string::load(summary.string()).get(text) !=
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

}  // namespace

std::string run_id(const RunKeys& k) {
    const std::string key = k.app + "|" + k.system + "|" + k.unique_input +
                            "|" + std::to_string(k.nodes) + "|" +
                            std::to_string(k.ppn) + "|" + k.papi_set;
    return hash::format_hex64(hash::fnv1a_hash(key));
}

coro::CoroTask<Discovery> discover(CoroScope& ctx,
                                   std::vector<std::string> roots) {
    Discovery out;
    const utilities::filesystem::PatternDirectoryScannerUtility scan;
    for (const auto& r : roots) {
        const fs::path root = fs::absolute(r);
        const auto entries = co_await scan(
            ctx, utilities::filesystem::PatternDirectoryScannerUtilityInput(
                     root.string(), {".pfw.gz", "summary.json", ".csv"}, true,
                     true));
        std::map<fs::path, DirFiles> dirs;
        for (const auto& e : entries) {
            const fs::path parent = e.path.parent_path();
            if (e.path.filename() == "summary.json") {
                dirs[parent].summary = true;
            } else if (parent.filename() == "compacted") {
                dirs[parent.parent_path()].traces.emplace_back(e.path.string(),
                                                               e.size);
            } else if (parent.filename() == "dftracer_service" &&
                       e.path.filename().string().rfind("gpu_power_", 0) == 0) {
                // <run>/raw/<set>/dftracer_service/gpu_power_<host>.csv
                const fs::path set = parent.parent_path();
                dirs[set.parent_path().parent_path()]
                    .gpu_csvs[set.filename().string()]
                    .push_back(e.path.string());
            }
        }
        for (auto& [dir, files] : dirs) {
            std::sort(files.traces.begin(), files.traces.end());
            for (auto& [_, csvs] : files.gpu_csvs)
                std::sort(csvs.begin(), csvs.end());
            discover_dir(root, dir, files, out);
        }
    }
    co_return out;
}

}  // namespace dftracer::utils::trace::genesis
