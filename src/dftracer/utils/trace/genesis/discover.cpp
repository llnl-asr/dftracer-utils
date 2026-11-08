// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash/hex64.h>
#include <dftracer/utils/dstd/hash/xxh3.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/trace/genesis/genesis.h>
#include <simdjson.h>

#include <algorithm>
#include <charconv>

namespace dftracer::utils::trace::genesis {

namespace {

dstd::Optional<std::int64_t> suffix_number(dstd::StringView name,
                                           dstd::StringView prefix) {
    if (name.substr(0, prefix.size()) != prefix) return dstd::nullopt;
    return dstd::from_chars<std::int64_t>(name.substr(prefix.size()));
}

// `<stem>-<k>_chunk<c>.pfw.gz` -> k.
dstd::Optional<std::int64_t> slice_index(dstd::StringView file_name) {
    const auto chunk = file_name.rfind("_chunk");
    const auto dash = file_name.rfind('-', chunk);
    if (chunk == dstd::StringView::npos || dash == dstd::StringView::npos)
        return dstd::nullopt;
    return dstd::from_chars<std::int64_t>(
        file_name.substr(dash + 1, chunk - dash - 1));
}

struct DirFiles {
    bool summary = false;
    dstd::Vector<dstd::Pair<dstd::String, std::uint64_t>> traces;
    // Traces directly in the run directory, used when it has no compacted/.
    dstd::Vector<dstd::Pair<dstd::String, std::uint64_t>> run_traces;
    // raw/<set>/*.pfw.gz, used for a set whose compacted file is missing.
    dstd::HashMap<dstd::String,
                  dstd::Vector<dstd::Pair<dstd::String, std::uint64_t>>>
        raw;
    dstd::HashMap<dstd::String, dstd::Vector<dstd::String>> gpu_csvs;
};

void set_files(
    RunGroup& g,
    const dstd::Vector<dstd::Pair<dstd::String, std::uint64_t>>& fs) {
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
    k.unique_input = dstd::String(input.filename());
    k.system = dstd::String(system.filename());
    k.app = dstd::String(app.filename());
}

dstd::String get_string(simdjson::dom::object o, dstd::StringView key) {
    dstd::StringView v;
    if (o[key].get(v) != simdjson::SUCCESS) return {};
    return dstd::String(v);
}

dstd::Optional<std::int64_t> get_int(simdjson::dom::object o,
                                     dstd::StringView key) {
    std::int64_t v = 0;
    if (o[key].get(v) != simdjson::SUCCESS) return dstd::nullopt;
    return v;
}

// Time slices `-<k>` must run 1..max without gaps.
bool slices_complete(RunGroup& base, const DirFiles& files, Discovery& out) {
    set_files(base, files.traces);
    if (base.files.empty()) {
        out.skips.push_back({base.dir, "compacted", "no trace files"});
        return false;
    }
    dstd::Vector<std::int64_t> idx;
    for (const auto& f : base.files) {
        const auto i = slice_index(dstd::PathView(f).filename());
        if (!i) {
            out.skips.push_back({base.dir, f, "unrecognized trace file name"});
            return false;
        }
        idx.push_back(*i);
    }
    std::sort(idx.begin(), idx.end());
    dstd::String missing;
    std::int64_t expect = 1;
    for (const auto i : idx) {
        if (i > expect) {
            if (!missing.empty()) missing += ',';
            missing += dstd::to_string(expect);
            if (i - 1 > expect) missing += "-" + dstd::to_string(i - 1);
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
    dstd::Vector<dstd::String> names;
    for (auto v : sets) {
        dstd::StringView set;
        if (v.get(set) == simdjson::SUCCESS) names.emplace_back(set);
    }
    std::sort(names.begin(), names.end());
    for (const auto& set : names) {
        const dstd::String file(
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

    dstd::Map<dstd::String, SetSpec> ordered;
    for (auto [set, run] : runs) {
        simdjson::dom::object r;
        if (run.get(r) != simdjson::SUCCESS) continue;
        bool completed = true;
        if (r["completed_ok"].get(completed) == simdjson::SUCCESS &&
            !completed) {
            out.skips.push_back(
                {base.dir, dstd::String(set), "completed_ok is false"});
            continue;
        }
        SetSpec spec;
        spec.keys = k;
        spec.keys.papi_set = dstd::String(set);
        simdjson::dom::array counters;
        if (r["papi_counters"].get(counters) == simdjson::SUCCESS) {
            for (auto c : counters) {
                dstd::StringView name;
                if (c.get(name) == simdjson::SUCCESS)
                    spec.papi_counters.emplace_back(name);
            }
        }
        std::sort(spec.papi_counters.begin(), spec.papi_counters.end());
        ordered.emplace(dstd::String(set), std::move(spec));
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
    base.dir = dstd::String(dir.str());
    // Relative to the resolved root, as std::filesystem::relative was.
    base.rel_dir = dstd::String(
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
    base.summary_json = dstd::String(simdjson::minify(s));
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

dstd::String run_id(const RunKeys& k) {
    const dstd::String key = k.app + "|" + k.system + "|" + k.unique_input +
                             "|" + dstd::to_string(k.nodes) + "|" +
                             dstd::to_string(k.ppn) + "|" + k.papi_set;
    dstd::String id(16, '\0');
    dstd::hash::format_hex64(dstd::hash::xxh3_64(key), id.data());
    return id;
}

coro::CoroTask<Discovery> discover(coro::Scope& ctx,
                                   dstd::Vector<dstd::String> roots) {
    Discovery out;
    for (const auto& r : roots) {
        const dstd::Path root =
            common::unwrap(dstd::fs::absolute(dstd::PathView(r)));
        runtime::fs::FindOptions options;
        options.patterns = {"*.pfw.gz", "summary.json", "*.csv"};
        options.skip_dirs =
            runtime::fs::names_of(common::constants::indexer::SCAN_SKIP_DIRS);
        options.skip_unreadable = true;
        auto found = co_await runtime::fs::find_all(ctx, root.view(),
                                                    std::move(options));
        if (!found) throw common::DFTUtilsException(found.error());
        const auto& entries = *found;
        DirMap dirs;
        for (const auto& e : entries) {
            const dstd::PathView path = e.path.view();
            const dstd::PathView parent = path.parent();
            if (path.filename() == "summary.json") {
                files_of(dirs, parent).summary = true;
            } else if (parent.filename() == "compacted") {
                files_of(dirs, parent.parent())
                    .traces.emplace_back(dstd::String(path.str()), e.size);
            } else if (path.extension() == ".gz" &&
                       parent.filename().starts_with("ppn_")) {
                // <run>/<stem>-<k>_chunk<c>.pfw.gz: slices kept in the run
                // directory itself rather than in compacted/.
                files_of(dirs, parent)
                    .run_traces.emplace_back(dstd::String(path.str()), e.size);
            } else if (path.extension() == ".gz" &&
                       parent.parent().filename() == "raw") {
                // <run>/raw/<set>/<file>.pfw.gz
                files_of(dirs, parent.parent().parent())
                    .raw[dstd::String(parent.filename())]
                    .emplace_back(dstd::String(path.str()), e.size);
            } else if (parent.filename() == "dftracer_service" &&
                       path.filename().starts_with("gpu_power_")) {
                // <run>/raw/<set>/dftracer_service/gpu_power_<host>.csv
                const dstd::PathView set = parent.parent();
                files_of(dirs, set.parent().parent())
                    .gpu_csvs[dstd::String(set.filename())]
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
