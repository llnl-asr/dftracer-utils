// SPDX-License-Identifier: MIT
#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/binaries/json_cell_printer.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/pipeline.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/store/shard_manifest.h>
#include <dftracer/utils/memory/budget.h>  // NO_SPILL_BUDGET
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/sharded_view.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_definition.h>

#include <cstdio>
#include <cstdlib>
#include <limits>

// MPI is compiled in only when the build enabled it (config.h macro). The same
// source then produces a distribution-capable dftracer_view; without MPI it is
// the plain single-node binary.
#ifdef DFTRACER_UTILS_ENABLE_MPI
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/runtime/distributed/mpi_transport.h>
#include <dftracer/utils/utilities/fileio/parallel/merge.h>

#endif

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::trace;
using namespace dftracer::utils::index::store;
using namespace dftracer::utils::trace::views;

#ifdef DFTRACER_UTILS_ENABLE_MPI
// The files owned by `rank` of `size`: a deterministic round-robin split, so
// every rank derives the same assignment from the same sorted file list.
static dstd::Vector<dstd::String> shard_files(
    const dstd::Vector<dstd::String>& all_files, int rank, int size) {
    dstd::Vector<dstd::String> shard;
    for (std::size_t i = 0; i < all_files.size(); ++i)
        if (static_cast<int>(i % size) == rank) shard.push_back(all_files[i]);
    return shard;
}
#endif

class ViewArgParse : public binaries::ArgParse {
   public:
    binaries::InputArgs inputs{
        binaries::DirMode::DefaultEmpty,
        "Trace files, a folder of trace files, or a shard set"};
    binaries::RuntimeArgs runtime_args;
    binaries::IndexingArgs indexing;
    binaries::DuqlArgs duql_args;

    dstd::StringView preset;
    dstd::StringView recipe;
    dstd::StringView save_recipe;
    dstd::StringView time_range;
    double min_duration = 0.0;
    double max_duration = 0.0;
    dstd::StringView output;
    bool no_auto_index = false;
    dstd::StringView group_by;
    dstd::StringView agg;
    std::uint64_t time_bucket = 0;
    std::uint64_t occ_cell = 0;
    double time_bucket_us = 0.0;
    double occ_cell_us = 0.0;
    bool counters = false;
    bool call_tree = false;
    bool flamegraph = false;
    dstd::StringView ct_partition;
    dstd::StringView phase;
    dstd::StringView record_schema;
    bool merge = false;
    bool no_index = false;
    bool verify = false;
    std::uint64_t limit = 0;
    std::uint64_t offset = 0;
    dstd::StringView select;
    dstd::Vector<dstd::StringView> params;
    bool explain = false;
    double time_scale = 0.0;
    std::uint64_t memory_budget = 0;
    bool no_spill = false;
    bool agg_numeric_args = false;
    bool collect_typed = false;
    bool materialize = false;

    explicit ViewArgParse(binaries::ArgParser& p) : ArgParse(p) {
        indexing.with_force = false;
        indexing.index_dir_help =
            "Directory where .dftindex stores are created";
        duql_args.help =
            "duql filter or pipeline (e.g., 'cat == \"POSIX\" | derive ms = "
            "dur / 1000 | sort -ms | take 5')";
        schema(inputs, runtime_args, indexing, duql_args);
    }

   protected:
    void register_args() override {
        parser().option(preset, {"--preset"},
                        "Predefined view: io, compute, dlio");

        parser().option(recipe, {"--recipe"}, "Custom view JSON file path");

        parser().option(save_recipe, {"--save-recipe"},
                        "Save the constructed view to a JSON file");

        parser().option(
            time_range, {"--time-range"},
            "Keep events starting in [min, max), microseconds (e.g., "
            "1000000,2000000)");

        parser().duration(
            min_duration, 1e6, {"--min-duration"},
            "Minimum event duration (a bare number is microseconds; "
            "suffixed values like 5ms are converted)",
            "0");

        parser().duration(
            max_duration, 1e6, {"--max-duration"},
            "Maximum event duration (a bare number is microseconds; "
            "suffixed values like 5ms are converted)",
            "0");

        parser().option(output, {"-o", "--output"},
                        "Output file path (default: stdout)");

        parser().flag(
            no_auto_index, {"--no-auto-index"},
            "Disable automatic index building for files missing .dftindex");

        parser().option(
            group_by, {"--group-by"},
            "Aggregate: group by columns, comma-separated. Any field name "
            "works (top-level then args); named dims name/cat/pid/tid/"
            "fhash/hhash/io_cat/acc_pat/file_path/file_name/host_name/"
            "rank, "
            "or arg:KEY / args.KEY for an args field");

        parser().option(
            agg, {"--agg"},
            "Aggregate: reducers, comma-separated (count, sum:FIELD, "
            "min:FIELD, max:FIELD, mean:FIELD, var:FIELD, std:FIELD, "
            "skew:FIELD, kurt:FIELD, pNN:FIELD e.g. p99:dur, pct:FIELD:Q)");

        parser().duration(
            time_bucket_us, 1e6, {"--time-bucket"},
            "Aggregate into time buckets of this width (a bare number is "
            "microseconds; suffixed values like 1ms are converted)",
            "0");

        parser().duration(
            occ_cell_us, 1e6, {"--occ-cell"},
            "Occupancy cell size (busy quantum) for busy/concurrency/"
            "utilization; a bare number is microseconds, 0 = the exact "
            "union. Finer resolves overlap on short events",
            "0");

        parser().flag(counters, {"--counters"},
                      "Emit the aggregation as ph=C counter events");

        parser().flag(
            call_tree, {"--call-tree"},
            "Containment call tree: events plus level/parent_id per lane");

        parser().flag(
            flamegraph, {"--flamegraph"},
            "Folded call tree (flamegraph) node frame; distributes under "
            "MPI via arena partials");

        parser().option(
            ct_partition, {"--ct-partition"},
            "Comma-separated lane keys for --call-tree/--flamegraph "
            "(default pid,tid)");

        parser().option(
            phase, {"--phase"},
            "Select events by phase: events (ph=X), counters (ph=C), "
            "aggregated, "
            "metadata (ph=M), any (every record, metadata included)");

        parser().option(record_schema, {"--schema"},
                        "Read records as this registered schema instead of the "
                        "detected one");

        parser().flag(merge, {"--merge"},
                      "Merge all inputs into one trace written to --output");

        parser().flag(
            no_index, {"--no-index"},
            "Do not build an index for the written trace (default: index)");

        parser().flag(verify, {"--verify"},
                      "Re-scan the exported output and confirm the event count "
                      "round-trips");

        parser().option(limit, {"--limit"},
                        "Cap the output to N rows/events (0 = unlimited)");

        parser().option(offset, {"--offset"},
                        "Skip the first N rows/events before applying --limit");

        parser().option(select, {"--select"},
                        "Project the result to these columns, comma-separated");

        parser()
            .option(params, {"--param"},
                    "Bind a --duql parameter: name=value, the value a duql "
                    "literal such as 5, 1.5, \"read\" or true (repeatable)")
            .append();

        parser().flag(explain, {"--explain"},
                      "Print the plan of --duql and exit without scanning");

        parser().option(time_scale, {"--time-scale"},
                        "Scale timestamps/durations by this ns-per-unit ratio "
                        "(0 = leave as stored)");

        parser().bytes(
            memory_budget, {"--memory-budget"},
            "Spill aggregation to disk past this many in-core bytes (0 = "
            "auto: ~1/3 of available memory). Accepts units, e.g. 512MB, "
            "4GB",
            "0");

        parser().flag(
            no_spill, {"--no-spill"},
            "Keep aggregation fully in memory (disable the default spill)");

        parser().flag(agg_numeric_args, {"--agg-numeric-args"},
                      "Aggregate every numeric args.* field automatically");

        parser().flag(
            collect_typed, {"--collect-typed"},
            "Dump the aggregation index's regular/aggregated/counters "
            "families (one pass)");

        parser().flag(materialize, {"--materialize"},
                      "Persist this aggregation as a rollup for instant reuse "
                      "(needs --group-by/--agg)");
    }

    dstd::Result<void> post_parse() override {
        time_bucket = static_cast<std::uint64_t>(std::llround(time_bucket_us));
        occ_cell = static_cast<std::uint64_t>(std::llround(occ_cell_us));
        return {};
    }
};

class FileSink : public ExportSink {
   public:
    explicit FileSink(FILE* f) : f_(f) {}
    void write(dstd::StringView data) override {
        std::fwrite(data.data(), 1, data.size(), f_);
    }

   private:
    FILE* f_;
};

// Discards everything. Used by --verify to drive a scan for its event count
// without materializing output.
class NullSink : public ExportSink {
   public:
    void write(dstd::StringView) override {}
};

static ViewFile make_view_file(const dstd::String& file,
                               dstd::String index_path,
                               std::size_t checkpoint_size) {
    ViewFile vf;
    vf.file_path = file;
    vf.index_path = std::move(index_path);
    vf.checkpoint_size = checkpoint_size;
    return vf;
}

// Logs a failure outside the pool run; the exit code is 1.
static bool fatal(const dstd::Error& error) {
    DFTRACER_UTILS_LOG_ERROR("Fatal: {}", error.c_str());
    return false;
}

// Re-scan a freshly written trace and confirm the event count round-trips.
template <class Configure>
static coro::CoroTask<dstd::Result<void>> verify_output(
    const dstd::String& final_output, const dstd::String& index_dir,
    std::size_t checkpoint_size, Configure&& configure,
    std::uint64_t expected_events, bool& verify_failed) {
    dstd::String output_arg{dstd::StringView(final_output)};
    dstd::String index_dir_arg{dstd::StringView(index_dir)};
    auto fresh = co_await dftracer::utils::index::build::ensure_index_fresh(
        "", std::move(output_arg), std::move(index_dir_arg));
    if (!fresh) co_return fresh.error();
    ViewFile ovf = make_view_file(
        final_output, internal::determine_index_path(final_output, index_dir),
        checkpoint_size);
    NullSink ns;
    View v = configure(View::from_files({ovf}));
    auto exported = co_await v.sink_json(ns);
    if (!exported) co_return exported.error();
    const ExportStats vs = *exported;
    if (vs.events_matched != expected_events) {
        DFTRACER_UTILS_LOG_ERROR(
            "Verify failed: wrote {} events but re-scan found {}",
            (unsigned long long)expected_events,
            (unsigned long long)vs.events_matched);
        verify_failed = true;
    } else {
        std::fprintf(stderr, "Verify OK: %llu events round-tripped\n",
                     (unsigned long long)expected_events);
    }
    co_return dstd::Result<void>{};
}

// Spec keys and reducers hold their names as owned text.
static dstd::String text(dstd::StringView v) { return dstd::String(v); }

static dstd::Vector<dstd::String> texts(
    const dstd::Vector<dstd::StringView>& views) {
    dstd::Vector<dstd::String> out;
    out.reserve(views.size());
    for (const auto v : views) out.emplace_back(v);
    return out;
}

static bool parse_group_by(dstd::StringView spec, dstd::Vector<GroupKey>& out) {
    for (const auto tok : binaries::split_csv(spec)) {
        if (tok == "name")
            out.push_back(GroupKey::name());
        else if (tok == "cat")
            out.push_back(GroupKey::cat());
        else if (tok == "pid")
            out.push_back(GroupKey::pid());
        else if (tok == "tid")
            out.push_back(GroupKey::tid());
        else if (tok == "fhash")
            out.push_back(GroupKey::fhash());
        else if (tok == "hhash")
            out.push_back(GroupKey::hhash());
        else if (tok == "io_cat")
            out.push_back(GroupKey::io_cat());
        else if (tok == "acc_pat")
            out.push_back(GroupKey::acc_pat());
        else if (tok == "file_path")
            out.push_back(GroupKey::file_path());
        else if (tok == "file_name")
            out.push_back(GroupKey::file_name());
        else if (tok == "host_name")
            out.push_back(GroupKey::host_name());
        else if (tok == "rank")
            out.push_back(GroupKey::rank());
        else if (tok.starts_with("arg:"))
            out.push_back(GroupKey::of_arg(text(tok.substr(4))));
        else
            // Schemaless fallback: a bare name resolves top-level then args; a
            // dotted/bracketed path (a.b, a[0], args.a.b) resolves that path.
            out.push_back(GroupKey::field(text(tok)));
    }
    return true;
}

static bool parse_agg(dstd::StringView spec, dstd::Vector<AggSpec>& out) {
    for (const auto tok : binaries::split_csv(spec)) {
        const auto colon = tok.find(':');
        const dstd::StringView op = tok.substr(0, colon);
        const dstd::StringView field = colon == dstd::StringView::npos
                                           ? dstd::StringView()
                                           : tok.substr(colon + 1);
        if (op == "count")
            out.push_back({AggOp::Count, "", ""});
        else if (op == "sum")
            out.push_back({AggOp::Sum, text(field), ""});
        else if (op == "min")
            out.push_back({AggOp::Min, text(field), ""});
        else if (op == "max")
            out.push_back({AggOp::Max, text(field), ""});
        else if (op == "mean")
            out.push_back({AggOp::Mean, text(field), ""});
        else if (op == "var")
            out.push_back({AggOp::Var, text(field), ""});
        else if (op == "std")
            out.push_back({AggOp::Std, text(field), ""});
        else if (op == "skew")
            out.push_back({AggOp::Skew, text(field), ""});
        else if (op == "kurt")
            out.push_back({AggOp::Kurt, text(field), ""});
        else if (op == "busy")
            out.push_back({AggOp::Busy, "dur", ""});
        else if (op == "concurrency")
            out.push_back({AggOp::Concurrency, "dur", ""});
        else if (op == "utilization")
            out.push_back({AggOp::Utilization, "dur", ""});
        else if (op == "active")
            out.push_back({AggOp::Active, "dur", ""});
        else if (op == "pct") {
            // pct:FIELD:Q  (explicit quantile, 0 < Q < 1; e.g. pct:dur:0.99)
            const auto c2 = field.find(':');
            const double q =
                c2 == dstd::StringView::npos
                    ? 0.0
                    : dstd::from_chars<double>(field.substr(c2 + 1))
                          .value_or(0.0);
            if (c2 == dstd::StringView::npos || c2 == 0 || q <= 0.0 ||
                q >= 1.0) {
                DFTRACER_UTILS_LOG_ERROR(
                    "pct needs pct:FIELD:Q with 0 < Q < 1 (e.g. pct:dur:0.99)");
                return false;
            }
            out.push_back(
                AggSpec(AggOp::Pct, text(field.substr(0, c2)), "", "", q));
        } else if (op.size() >= 2 && op[0] == 'p' &&
                   op.find_first_not_of("0123456789", 1) ==
                       dstd::StringView::npos) {
            // pNN shorthand: p50/p90/p99/p999 -> the 0.NN quantile of FIELD.
            if (field.empty()) {
                DFTRACER_UTILS_LOG_ERROR("{} needs a field, e.g. {}:dur", op,
                                         op);
                return false;
            }
            double denom = 1.0;
            for (std::size_t i = 1; i < op.size(); ++i) denom *= 10.0;
            const double digits =
                dstd::from_chars<double>(op.substr(1)).value_or(0.0);
            out.push_back(AggSpec(AggOp::Pct, text(field),
                                  dstd::fmt<"{}_{}">(op, field), "",
                                  digits / denom));
        } else {
            DFTRACER_UTILS_LOG_ERROR("Unknown --agg reducer: {}", tok);
            return false;
        }
    }
    return true;
}

// Print a collect() result as one JSON object per row.
static void print_table(FILE* out, const dataframe::DataFrame& table) {
    const std::int64_t nrows = table.num_rows();
    for (std::int64_t r = 0; r < nrows; ++r) {
        dftracer::utils::dstd::String s = "{";
        bool first = true;
        for (std::size_t c = 0; c < table.columns.size(); ++c) {
            if (!binaries::cli_emittable(table.columns[c].type())) continue;
            if (!first) s += ",";
            first = false;
            s += '"';
            s += table.names[c];
            s += "\":";
            binaries::append_cell_json(s, table.columns[c], r);
        }
        s += "}\n";
        std::fwrite(s.data(), 1, s.size(), out);
    }
}

// Emit the three collect_typed() families to `out` as NDJSON rows, each family
// preceded by a header on stderr so the sections stay identifiable when the
// data stream is redirected.
static void emit_typed(FILE* out, const dataframe::DataFrame& regular,
                       const dataframe::DataFrame& aggregated,
                       const dataframe::DataFrame& counters) {
    const dstd::Pair<const char*, const dataframe::DataFrame*> families[] = {
        {"regular", &regular},
        {"aggregated", &aggregated},
        {"counters", &counters},
    };
    for (const auto& [name, table] : families) {
        std::fprintf(stderr, "== %s (%lld rows) ==\n", name,
                     static_cast<long long>(table->num_rows()));
        print_table(out, *table);
    }
}

namespace {

// Rebuilds the stale indexes of `files` under `index_dir`.
static coro::CoroTask<dstd::Result<void>> refresh_indexes(
    const dstd::Vector<dstd::String>& files, const dstd::String& index_dir) {
    auto to_check = files;
    dstd::String dir_arg{dstd::StringView(index_dir)};
    auto fresh = co_await dftracer::utils::index::build::ensure_indexes_fresh(
        "", std::move(to_check), std::move(dir_arg));
    if (!fresh) co_return fresh.error();
    co_return dstd::Result<void>{};
}

static dstd::Vector<dstd::String> select_columns(dstd::StringView select) {
    return texts(binaries::split_csv(select));
}

static bool resolve_view_definition(const ViewArgParse& cli,
                                    ViewDefinition& view) {
    const dstd::StringView preset = cli.preset;
    const dstd::StringView recipe_path = cli.recipe;
    if (!preset.empty()) {
        if (preset == "io") {
            view = ViewDefinition::io_view();
        } else if (preset == "compute") {
            view = ViewDefinition::compute_view();
        } else if (preset == "dlio") {
            view = ViewDefinition::dlio_view();
        } else {
            DFTRACER_UTILS_LOG_ERROR(
                "Unknown preset: {}. Use: io, compute, dlio",
                dstd::String(preset));
            return false;
        }
    } else if (!recipe_path.empty()) {
        auto recipe_exists = dstd::fs::exists(dstd::PathView(recipe_path));
        if (!recipe_exists.has_value()) return fatal(recipe_exists.error());
        if (!*recipe_exists) {
            DFTRACER_UTILS_LOG_ERROR("Recipe file not found: {}",
                                     dstd::String(recipe_path));
            return false;
        }
        auto json_content = dstd::fs::read_file(dstd::PathView(recipe_path));
        if (!json_content) return fatal(json_content.error());
        view = ViewDefinition::from_json(dstd::String(*json_content));
    } else {
        view.name = "custom";
        view.description = "Custom inline view";
    }
    return true;
}

static bool parse_params(const ViewArgParse& cli, duql::Params& out) {
    for (const auto param : cli.params) {
        const dstd::String kv(param);
        const auto eq = kv.find('=');
        if (eq == dstd::String::npos || eq == 0) {
            DFTRACER_UTILS_LOG_ERROR("Invalid --param '{}': use name=value",
                                     kv);
            return false;
        }
        auto value = duql::parse_param(dstd::StringView(kv).substr(eq + 1));
        if (!value) {
            DFTRACER_UTILS_LOG_ERROR("Invalid --param '{}': {}", kv,
                                     value.error().message);
            return false;
        }
        out.insert_or_assign(kv.substr(0, eq), std::move(*value));
    }
    return true;
}

static bool parse_time_range(dstd::StringView time_range_str,
                             dstd::Optional<dstd::Pair<double, double>>& out) {
    auto comma = time_range_str.find(',');
    const auto min_ts =
        comma == dstd::StringView::npos
            ? dstd::nullopt
            : dstd::from_chars<double>(time_range_str.substr(0, comma));
    const auto max_ts =
        comma == dstd::StringView::npos
            ? dstd::nullopt
            : dstd::from_chars<double>(time_range_str.substr(comma + 1));
    if (!min_ts || !max_ts) {
        DFTRACER_UTILS_LOG_ERROR(
            "Invalid --time-range format. Use: min,max (e.g., "
            "1000000,2000000)");
        return false;
    }
    out = dstd::Pair<double, double>(*min_ts, *max_ts);
    return true;
}

static bool add_duration_filter(const ViewArgParse& cli, ViewDefinition& view) {
    const auto min_duration = cli.min_duration;
    const auto max_duration = cli.max_duration;
    if (min_duration <= 0 && max_duration <= 0) return true;
    dstd::String extra;
    if (min_duration > 0) {
        if (!extra.empty()) extra += " and ";
        extra += dstd::fmt<"dur >= {}">(static_cast<uint64_t>(min_duration));
    }
    if (max_duration > 0) {
        if (!extra.empty()) extra += " and ";
        extra += dstd::fmt<"dur <= {}">(static_cast<uint64_t>(max_duration));
    }
    auto query = duql::Query::from_string(extra);
    if (!query) {
        DFTRACER_UTILS_LOG_ERROR("Fatal: {}", query.error().format());
        return false;
    }
    view.with_query(std::move(*query));
    return true;
}

// --phase selects the input phase; "any" reads every record, metadata
// included.
static bool parse_phase(dstd::StringView phase, Phase& out) {
    if (phase == "events")
        out = Phase::Events;
    else if (phase == "counters")
        out = Phase::Counters;
    else if (phase == "aggregated")
        out = Phase::Aggregated;
    else if (phase == "metadata")
        out = Phase::Metadata;
    else if (phase == "any")
        out = Phase::Any;
    else {
        DFTRACER_UTILS_LOG_ERROR(
            "Unknown --phase: {}. Use events, counters, aggregated, metadata, "
            "or any.",
            dstd::String(phase));
        return false;
    }
    return true;
}

static bool collect_input_files(const ViewArgParse& cli,
                                const dstd::String& shard_set_root,
                                dstd::Vector<dstd::String>& files) {
    const dstd::StringView directory = cli.inputs.directory;
    if (!shard_set_root.empty()) {
        // Self-describing: ShardedView enumerates files from each shard index.
    } else if (!directory.empty()) {
        if (!common::input_exists(dstd::CStringView(directory.data()))) {
            DFTRACER_UTILS_LOG_ERROR("Directory does not exist: {}",
                                     dstd::String(directory));
            return false;
        }

        auto matched =
            binaries::scan_trace_files_on_pool(cli.runtime_args, directory,
                                               /*populate_size=*/false);
        if (!matched) {
            DFTRACER_UTILS_LOG_ERROR("Fatal: {}", matched.error().c_str());
            return false;
        }

        for (const auto& entry : *matched) {
            files.emplace_back(entry.path.str());
        }
        std::sort(files.begin(), files.end());

        if (files.empty()) {
            DFTRACER_UTILS_LOG_ERROR("No trace files found in: {}",
                                     dstd::String(directory));
            return false;
        }
    }
    for (const auto file : cli.inputs.files) files.emplace_back(file);
    if (shard_set_root.empty() && files.empty()) {
        DFTRACER_UTILS_LOG_ERROR(
            "{}", "No files or folder specified. Use --help for usage.");
        return false;
    }
    return true;
}

// False when a file lacks its index and --no-auto-index forbids building it.
static bool check_missing_indexes(const dstd::Vector<dstd::String>& files,
                                  dstd::StringView index_dir,
                                  bool no_auto_index) {
    dstd::Vector<dstd::String> files_needing_index;
    for (const auto& file_path : files) {
        const dstd::String index_path =
            internal::determine_index_path(file_path, index_dir);
        auto store_exists = dstd::fs::exists(
            dstd::PathView(dftracer::utils::index::store_dir(index_path)));
        if (!store_exists.has_value()) return fatal(store_exists.error());
        if (!*store_exists) files_needing_index.push_back(file_path);
    }

    if (!files_needing_index.empty()) {
        if (no_auto_index) {
            DFTRACER_UTILS_LOG_ERROR(
                "Missing .dftindex store for {} file(s) and --no-auto-index is "
                "set. Run dftracer_index first.",
                files_needing_index.size());
            for (const auto& f : files_needing_index) {
                std::fprintf(stderr, "  Missing index: %s\n", f.c_str());
            }
            return false;
        }

        // Progress goes to stderr so it never pollutes the data on stdout.
        std::fprintf(stderr, "Auto-building index for %zu file(s)...\n",
                     files_needing_index.size());
    }
    return true;
}

static void print_summary(const ViewDefinition& view, std::size_t file_count,
                          const ExportStats& stats) {
    std::fprintf(stderr,
                 "View: %s | Files: %zu | Chunks: scanned=%llu skipped=%llu | "
                 "Events/rows: %llu (scanned=%llu)\n",
                 view.name.c_str(), file_count,
                 (unsigned long long)stats.chunks_scanned,
                 (unsigned long long)stats.chunks_skipped,
                 (unsigned long long)stats.events_matched,
                 (unsigned long long)stats.events_scanned);
    if (stats.lines_invalid > 0 || stats.values_unconverted > 0)
        std::fprintf(stderr,
                     "Skipped %llu line(s) that are not JSON objects; %llu "
                     "value(s) did not convert to their declared type and "
                     "read as null\n",
                     (unsigned long long)stats.lines_invalid,
                     (unsigned long long)stats.values_unconverted);
}

// What one run of dftracer_view does: the resolved options, the outputs and
// the sinks. It must outlive the pipeline run that executes `run`.
struct ViewRun {
    const ViewArgParse* cli;
    ViewDefinition view;
    dstd::StringView duql_str;
    duql::Params duql_params;
    dstd::Optional<dstd::Pair<double, double>> time_range;
    dstd::Vector<GroupKey> group_keys;
    dstd::Vector<AggSpec> agg_specs;
    dstd::Vector<dstd::String> ct_partition;
    Phase view_phase = Phase::Any;
    std::uint64_t time_bucket = 0;
    bool counters = false;
    bool ct_mode = false;
    bool fg_mode = false;
    bool aggregate = false;
    bool typed_mode = false;
    bool mv_mode = false;
    bool merge = false;
    bool no_index = false;
    bool no_auto_index = false;
    bool verify = false;
    bool write_trace = false;
    bool duql_rows = false;
    bool has_window = false;
    std::int64_t window_size = 0;
    std::uint64_t member_size = 0;
    std::uint64_t checkpoint_size = 0;
    dstd::StringView index_dir;
    dstd::String index_dir_std;
    dstd::String shard_set_root;
    dstd::Vector<dstd::String> files;
    dstd::Vector<ViewFile> view_files;
    dstd::String final_output;
    dstd::SharedPtr<ScanCounter> scans;
    FILE* out_file = nullptr;
    FILE* out_target = nullptr;
    FileSink* sink = nullptr;
    ExportStats stats;
    bool verify_failed = false;
    bool index_merge_failed = false;

    explicit ViewRun(const ViewArgParse* c);

    View configure(View v) const;
    View windowed(View v) const;
    coro::CoroTask<dstd::Result<dataframe::DataFrame>> windowed_table(
        dataframe::DataFrame table) const;
    // The exit code when the probe ends the run, else nothing.
    dstd::Optional<int> probe_duql();
#ifdef DFTRACER_UTILS_ENABLE_MPI
    coro::CoroTask<dstd::Result<void>> run_distributed_aggregate(
        distributed::MpiTransport& transport);
    coro::CoroTask<dstd::Result<void>> run_distributed_write(
        distributed::MpiTransport& transport);
#endif
    coro::CoroTask<dstd::Result<void>> run_shard_set();
    coro::CoroTask<dstd::Result<void>> run_local();
    coro::CoroTask<dstd::Result<void>> run();
};

ViewRun::ViewRun(const ViewArgParse* c)
    : cli(c),
      duql_str(c->duql_args.duql),
      time_bucket(c->time_bucket),
      counters(c->counters),
      ct_mode(c->call_tree),
      fg_mode(c->flamegraph),
      typed_mode(c->collect_typed),
      mv_mode(c->materialize),
      merge(c->merge),
      no_index(c->no_index),
      no_auto_index(c->no_auto_index),
      verify(c->verify),
      has_window(c->offset > 0 || c->limit > 0),
      window_size(c->limit > 0 ? static_cast<std::int64_t>(c->limit)
                               : std::numeric_limits<std::int64_t>::max()),
      // One knob: gzip member size == checkpoint size (a checkpoint is a
      // member).
      member_size(c->indexing.checkpoint_size),
      checkpoint_size(c->indexing.checkpoint_size),
      index_dir(c->indexing.index_dir),
      index_dir_std(c->indexing.index_dir),
      // collect() returns only its table, so its scans count here.
      scans(dstd::make_shared<ScanCounter>()) {
    ct_partition = texts(binaries::split_csv(c->ct_partition));
}

// Apply the parsed view options to a base View; shared by the single-node
// and distributed paths so both aggregate identically. The projection and
// row window (--select/--offset/--limit) are applied by `windowed`.
View ViewRun::configure(View v) const {
    v = v.count_scans(scans);
    if (!cli->record_schema.empty())
        v = v.record_schema(dstd::String(cli->record_schema));
    if (view.query) v = v.filter(*view.query);
    if (!cli->phase.empty()) v = v.phase(view_phase);
    if (time_range) v = v.time_range(time_range->first, time_range->second);
    if (cli->time_scale > 0) v = v.time_scale(cli->time_scale);
    if (time_bucket > 0) v = v.time_bucket(time_bucket);
    if (cli->occ_cell > 0) v = v.resolution(cli->occ_cell);
    if (cli->no_spill)
        v = v.memory_budget(memory::NO_SPILL_BUDGET);
    else if (cli->memory_budget > 0)
        v = v.memory_budget(cli->memory_budget);
    // else: leave the plan default (0), which resolves to auto (~1/3 RAM).
    if (!duql_str.empty()) v = v.duql(duql_str, duql_params);
    if (!group_keys.empty()) v = v.group_by(group_keys);
    if (!agg_specs.empty()) v = v.agg(agg_specs);
    if (cli->agg_numeric_args) v = v.agg_numeric_args();
    return v;
}

View ViewRun::windowed(View v) const {
    if (!cli->select.empty()) v = v.select(select_columns(cli->select));
    if (has_window)
        v = v.slice(static_cast<std::int64_t>(cli->offset), window_size);
    return v;
}

// The same projection and window over a table merged from partials.
coro::CoroTask<dstd::Result<dataframe::DataFrame>> ViewRun::windowed_table(
    dataframe::DataFrame table) const {
    if (cli->select.empty() && !has_window) co_return std::move(table);
    dataframe::LazyFrame rows = dataframe::lazy(std::move(table));
    if (!cli->select.empty()) rows = rows.select(select_columns(cli->select));
    if (has_window)
        rows = rows.slice(static_cast<std::int64_t>(cli->offset), window_size);
    auto collected = co_await rows.collect();
    if (!collected) co_return collected.error();
    co_return std::move(*collected);
}

// Stages after the scan give a table, not events; --explain prints the
// plan without scanning.
dstd::Optional<int> ViewRun::probe_duql() {
    if (duql_str.empty()) return dstd::nullopt;
    // duql types fields from the index catalog, so a missing index is built
    // before the query compiles. A distributed aggregation builds its shards'
    // indexes itself.
    if (!no_auto_index && !aggregate && !counters && !fg_mode) {
        auto refreshed = binaries::run_on_pool(
            "dftracer_view index", cli->runtime_args,
            [&](coro::Scope&) -> coro::CoroTask<dstd::Result<void>> {
                co_return co_await refresh_indexes(files, index_dir_std);
            });
        if (!refreshed) {
            DFTRACER_UTILS_LOG_ERROR("Fatal: {}", refreshed.error().c_str());
            return 1;
        }
    }
    View probe = View::from_files(view_files);
    if (!cli->record_schema.empty())
        probe = probe.record_schema(dstd::String(cli->record_schema));
    auto explained = probe.explain_duql(duql_str, duql_params);
    if (!explained) {
        DFTRACER_UTILS_LOG_ERROR("Invalid --duql: {}",
                                 explained.error().c_str());
        return 1;
    }
    const dstd::String plan = std::move(*explained);
    if (cli->explain) {
        std::fputs(plan.c_str(), stdout);
        return 0;
    }
    // Sides only feed the scan filter; their lines are indented or
    // start with "side " or "reads ".
    for (std::size_t at = 0; at < plan.size();) {
        const dstd::StringView line(plan.data() + at, plan.find('\n', at) - at);
        if (!line.starts_with("scan ") && !line.starts_with("side ") &&
            !line.starts_with("reads ") && !line.starts_with(" "))
            duql_rows = true;
        at += line.size() + 1;
    }
    return dstd::nullopt;
}

#ifdef DFTRACER_UTILS_ENABLE_MPI
// Distributed aggregation: each rank aggregates its shard into a serialized
// partial, the partials are gathered, and rank 0 merges and emits.
coro::CoroTask<dstd::Result<void>> ViewRun::run_distributed_aggregate(
    distributed::MpiTransport& transport) {
    auto shard = shard_files(files, transport.rank(), transport.size());
    // Each rank builds its own shard of the root, on node-local scratch when
    // the destination warrants it, and publishes it as <root>/shards/<rank>.
    // Rank 0 merges the shards once every rank published its own.
    const dstd::String idx_root =
        internal::determine_index_path(files.front(), index_dir);
    const dstd::String rank_idx = dftracer::utils::index::shard_root(
        idx_root, static_cast<std::size_t>(transport.rank()));
    if (!no_auto_index) {
        // Build each rank's index with its aggregation tier.
        dftracer::utils::index::build::ResolveAndBuildInput bin;
        bin.files = shard;
        bin.index_dir = rank_idx;
        bin.checkpoint_size = checkpoint_size;
        bin.require_checkpoints = true;
        auto built =
            co_await dftracer::utils::index::build::resolve_and_build_index(
                std::move(bin));
        if (!built) co_return built.error();
    }
    dstd::Vector<ViewFile> shard_files_vf;
    for (const auto& f : shard) {
        shard_files_vf.push_back(make_view_file(f, rank_idx, checkpoint_size));
    }
    View shard_view = configure(View::from_files(std::move(shard_files_vf)));
    dataframe::LazyResult<dstd::String> partial_plan =
        fg_mode ? shard_view.flamegraph_partial(ct_partition)
                : shard_view.aggregate_partial();
    auto collected = co_await partial_plan.collect();
    if (!collected) co_return collected.error();
    dstd::String partial = std::move(*collected);
    auto partials = transport.all_gather(partial);
    if (transport.rank() == 0) {
        // Every rank published its shard before the gather, so the set is
        // complete: catalog it and merge the shards into the root's own store,
        // leaving one index for a later query. A failed merge keeps the
        // manifest and the shards, so a later call retries it without a
        // rebuild.
        if (!no_auto_index) {
            dstd::Vector<dstd::String> shard_dirs;
            for (int r = 0; r < transport.size(); ++r) {
                dstd::String d(dftracer::utils::index::shard_root(
                    idx_root, static_cast<std::size_t>(r)));
                auto shard_exists = dstd::fs::exists(
                    dstd::PathView(dftracer::utils::index::store_dir(d)));
                if (!shard_exists.has_value()) co_return shard_exists.error();
                if (*shard_exists) shard_dirs.push_back(std::move(d));
            }
            if (!shard_dirs.empty()) {
                dstd::Optional<dstd::Error> merge_error;
                auto written = write_shard_set(idx_root, shard_dirs);
                if (!written) {
                    merge_error = written.error();
                } else {
                    auto consolidated = consolidate_shard_set(idx_root);
                    if (!consolidated) merge_error = consolidated.error();
                }
                if (merge_error) {
                    index_merge_failed = true;
                    DFTRACER_UTILS_LOG_ERROR("shards not merged into {}: {}",
                                             idx_root, merge_error->c_str());
                }
            }
        }

        dstd::Vector<dstd::StringView> pv(partials.begin(), partials.end());
        View merger = configure(View::from_files({}));
        if (fg_mode) {
            dataframe::DataFrame table = View::merge_flamegraph_partials(pv);
            print_table(out_target, table);
            stats.events_matched = table.num_rows();
        } else if (counters) {
            auto counted = merger.merge_counter_partials(pv, *sink);
            if (!counted) co_return counted.error();
            stats = *counted;
        } else {
            auto merged = merger.merge_partials(pv);
            if (!merged) co_return merged.error();
            auto windowed_result = co_await windowed_table(std::move(*merged));
            if (!windowed_result) co_return windowed_result.error();
            const dataframe::DataFrame& table = *windowed_result;
            print_table(out_target, table);
            stats.events_matched = table.num_rows();
        }
    }
    co_return dstd::Result<void>{};
}

// Distributed: each rank writes its shard, rank 0 concatenates.
coro::CoroTask<dstd::Result<void>> ViewRun::run_distributed_write(
    distributed::MpiTransport& transport) {
    auto shard = shard_files(files, transport.rank(), transport.size());
    dstd::String idx_base(index_dir);
    if (idx_base.empty())
        idx_base = dstd::String(dstd::PathView(files.front()).parent().str());
    dstd::String rank_idx = dstd::fmt<"{}/rank_{}">(idx_base, transport.rank());
    if (!no_auto_index) {
        auto refreshed = co_await refresh_indexes(shard, rank_idx);
        if (!refreshed) co_return refreshed.error();
    }
    dstd::Vector<ViewFile> shard_vf;
    for (const auto& f : shard) {
        shard_vf.push_back(make_view_file(
            f, internal::determine_index_path(f, rank_idx), checkpoint_size));
    }
    const dstd::String rank_out =
        dstd::fmt<"{}.rank_{}">(final_output, transport.rank());
    TraceWriteOptions topts;
    topts.output_path = rank_out;
    topts.member_size = member_size;
    topts.worker_count = cli->runtime_args.executor_threads;
    topts.compress = true;
    View shard_view =
        windowed(configure(View::from_files(std::move(shard_vf))));
    auto written = co_await shard_view.sink_trace(topts);
    if (!written) co_return written.error();
    stats = *written;
    auto rank_outs = transport.all_gather(rank_out);
    if (transport.rank() == 0) {
        const auto merge_rc =
            co_await fileio::parallel::merge_shards(final_output, rank_outs);
        if (merge_rc != 0) {
            DFTRACER_UTILS_LOG_ERROR("merge_shards failed for {}",
                                     final_output);
        } else if (!no_index) {
            dstd::String file_arg{dstd::StringView(final_output)};
            dstd::String dir_arg{dstd::StringView(index_dir)};
            auto fresh =
                co_await dftracer::utils::index::build::ensure_index_fresh(
                    "", std::move(file_arg), std::move(dir_arg));
            if (!fresh) co_return fresh.error();
        }
    }
    co_return dstd::Result<void>{};
}
#endif

// Shard set: merge the query across the immutable shards read-only, no
// trace-file scan when each shard's aggregation tier covers it.
coro::CoroTask<dstd::Result<void>> ViewRun::run_shard_set() {
    const auto configure_view = [this](View v) { return configure(v); };
    auto manifest = ShardedView::from_manifest(shard_set_root);
    if (!manifest) co_return manifest.error();
    ShardedView sv = std::move(*manifest);
    if (counters) {
        auto counted = co_await sv.aggregate_counters(configure_view, *sink);
        if (!counted) co_return counted.error();
        stats = *counted;
    } else {
        auto merged = co_await sv.aggregate(configure_view);
        if (!merged) co_return merged.error();
        auto windowed_result = co_await windowed_table(std::move(*merged));
        if (!windowed_result) co_return windowed_result.error();
        const dataframe::DataFrame& table = *windowed_result;
        print_table(out_target, table);
        stats.events_matched = table.num_rows();
    }
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> ViewRun::run_local() {
    // The typed, rollup and counter terminals read the whole aggregation; the
    // projection and row window apply to the other outputs.
    View base = configure(View::from_files(view_files));
    View v = windowed(base);

    // Skip the eager pre-build when the query would take the raw-gzip
    // bootstrap (answer the query and build the index in one pass);
    // pre-building first would make the index exist so the bootstrap never
    // fires. Only the plain export / collect paths bootstrap.
    bool will_bootstrap = false;
    if (aggregate) {
        auto bootstraps = base.collect_would_bootstrap();
        if (!bootstraps.has_value()) co_return bootstraps.error();
        will_bootstrap = *bootstraps;
    }
    if (!will_bootstrap && !counters && !aggregate && !write_trace &&
        !duql_rows) {
        auto bootstraps = v.export_would_bootstrap();
        if (!bootstraps.has_value()) co_return bootstraps.error();
        will_bootstrap = *bootstraps;
    }
    if (!no_auto_index && !will_bootstrap) {
        auto refreshed = co_await refresh_indexes(files, index_dir_std);
        if (!refreshed) co_return refreshed.error();
    }

    if (ct_mode || fg_mode) {
        dataframe::LazyFrame tree =
            fg_mode ? v.flamegraph(ct_partition) : v.call_tree(ct_partition);
        auto collected = co_await tree.collect();
        if (!collected) co_return collected.error();
        const dataframe::DataFrame& table = *collected;
        print_table(out_target, table);
        stats.events_matched = table.num_rows();
    } else if (mv_mode) {
        // Build-only terminal: compute the aggregation and persist it as a
        // rollup so a later matching query hits the fast path.
        auto materialized = co_await base.materialize();
        if (!materialized) co_return materialized.error();
        stats = *materialized;
        std::fprintf(stderr, "Materialized rollup (%llu events).\n",
                     (unsigned long long)stats.events_matched);
    } else if (typed_mode) {
        auto typed = co_await base.collect_typed();
        if (!typed) co_return typed.error();
        auto& tr = *typed;
        emit_typed(out_target, tr.regular, tr.aggregated, tr.counters);
        stats.events_matched = static_cast<std::uint64_t>(
            tr.regular.num_rows() + tr.aggregated.num_rows() +
            tr.counters.num_rows());
    } else if (counters) {
        auto counted = co_await base.sink_counters(*sink);
        if (!counted) co_return counted.error();
        stats = *counted;
    } else if (aggregate || duql_rows) {
        auto collected = co_await v.collect();
        if (!collected) co_return collected.error();
        const dataframe::DataFrame& table = *collected;
        print_table(out_target, table);
        stats.events_matched = table.num_rows();
        stats.events_scanned = scans->events_scanned.load();
        stats.chunks_scanned = scans->chunks_scanned.load();
        stats.chunks_skipped = scans->chunks_skipped.load();
    } else if (write_trace) {
        // Fused: export_trace builds the index during the write (no re-inflate
        // of the output) unless --no-index opts out.
        TraceWriteOptions topts;
        topts.output_path = final_output;
        topts.member_size = member_size;
        topts.worker_count = cli->runtime_args.executor_threads;
        topts.compress = true;
        topts.build_index = !no_index;
        topts.index_path = index_dir;
        auto written = co_await v.sink_trace(topts);
        if (!written) co_return written.error();
        stats = *written;
    } else {
        auto exported = co_await v.sink_json(*sink);
        if (!exported) co_return exported.error();
        stats = *exported;
    }

    if (out_file) std::fflush(out_file);

    if (verify && write_trace) {
        const auto verify_configure = [this](View b) {
            return windowed(configure(b));
        };
        auto verified = co_await verify_output(
            final_output, index_dir_std, checkpoint_size, verify_configure,
            stats.events_matched, verify_failed);
        if (!verified) co_return verified.error();
    }
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> ViewRun::run() {
#ifdef DFTRACER_UTILS_ENABLE_MPI
    distributed::MpiTransport transport;
    // Only the reductions distribute; other modes fall through to rank 0
    // (single-node) below.
    if ((counters || aggregate || fg_mode) && transport.size() > 1 &&
        shard_set_root.empty()) {
        co_return co_await run_distributed_aggregate(transport);
    }
    if (write_trace && transport.size() > 1) {
        co_return co_await run_distributed_write(transport);
    }
    // Non-distributed modes run on rank 0 only (correct, not sharded).
    if (transport.rank() != 0) co_return dstd::Result<void>{};
#endif
    if (!shard_set_root.empty()) {
        co_return co_await run_shard_set();
    }
    co_return co_await run_local();
}

static coro::CoroTask<int> run_view(const ViewArgParse* cli) {
    ViewRun run(cli);
    const dstd::StringView save_recipe = cli->save_recipe;

    if (!resolve_view_definition(*cli, run.view)) co_return 1;

    if (!parse_params(*cli, run.duql_params)) co_return 1;
    if (cli->explain && run.duql_str.empty()) {
        DFTRACER_UTILS_LOG_ERROR("{}", "--explain needs --query.");
        co_return 1;
    }
    if (!run.duql_str.empty() && !save_recipe.empty()) {
        DFTRACER_UTILS_LOG_ERROR(
            "{}",
            "--save-recipe does not store --duql; write the filter "
            "into the recipe instead.");
        co_return 1;
    }

    if (!cli->time_range.empty() &&
        !parse_time_range(cli->time_range, run.time_range))
        co_return 1;

    if (!add_duration_filter(*cli, run.view)) co_return 1;

    if (!parse_group_by(cli->group_by, run.group_keys)) co_return 1;
    if (!parse_agg(cli->agg, run.agg_specs)) co_return 1;
    run.aggregate = !run.group_keys.empty() || !run.agg_specs.empty() ||
                    run.counters || run.time_bucket > 0 ||
                    cli->agg_numeric_args;
    // Event export to a file -> compressed, indexed trace via the parallel
    // writer. Aggregate/counter/containment tables and stdout keep their own
    // paths.
    run.write_trace = !run.aggregate && !run.counters && !run.typed_mode &&
                      !run.mv_mode && !run.ct_mode && !run.fg_mode &&
                      !cli->output.empty();

    if (run.mv_mode && !run.aggregate) {
        DFTRACER_UTILS_LOG_ERROR(
            "{}", "--materialize needs an aggregation (--group-by/--agg).");
        co_return 1;
    }

    if (!cli->phase.empty() && !parse_phase(cli->phase, run.view_phase))
        co_return 1;

    if (!run.view.query && run.duql_str.empty() && !run.time_range &&
        !run.aggregate && !run.merge && !run.typed_mode && !run.mv_mode &&
        !run.ct_mode && !run.fg_mode) {
        DFTRACER_UTILS_LOG_ERROR(
            "{}",
            "Nothing to do. Use --preset, --recipe, --duql, --time-range, "
            "--merge, --call-tree, --flamegraph, or --group-by/--agg.");
        co_return 1;
    }

    if (!save_recipe.empty()) {
        auto saved = dstd::fs::write_file(dstd::PathView(save_recipe),
                                          run.view.to_json());
        if (!saved) {
            fatal(saved.error());
            co_return 1;
        }
        std::printf("View recipe saved to: %.*s\n",
                    static_cast<int>(save_recipe.size()), save_recipe.data());
    }

    // A shard set is a directory holding a shards.json manifest. When the
    // target is one, the query runs over the immutable shards via ShardedView
    // and needs no trace-file arguments (the shards are self-describing).
    const dstd::StringView directory = cli->inputs.directory;
    const dstd::String directory_std(directory);
    if (!directory.empty())
        run.shard_set_root = resolve_shard_set_root(directory_std);
    if (run.shard_set_root.empty() && !run.index_dir.empty())
        run.shard_set_root = resolve_shard_set_root(run.index_dir_std);

    if (!run.shard_set_root.empty() && !run.aggregate && !run.counters) {
        DFTRACER_UTILS_LOG_ERROR(
            "{}",
            "A shard set supports only aggregate/counter queries "
            "(--group-by/--agg/--counters).");
        co_return 1;
    }

    if (!collect_input_files(*cli, run.shard_set_root, run.files)) co_return 1;

    // One View over every file; the executor plans and scans them in parallel.
    run.view_files.reserve(run.files.size());
    for (const auto& file : run.files) {
        run.view_files.push_back(make_view_file(
            file, internal::determine_index_path(file, run.index_dir),
            run.checkpoint_size));
    }

    if (const auto probe_rc = run.probe_duql()) co_return *probe_rc;
    if (run.duql_rows &&
        (run.aggregate || run.counters || run.typed_mode || run.mv_mode ||
         run.ct_mode || run.fg_mode || run.merge)) {
        DFTRACER_UTILS_LOG_ERROR(
            "{}",
            "--duql stages after the scan do not combine with "
            "aggregation, counters, typed, materialize, call-tree, "
            "flamegraph or merge output.");
        co_return 1;
    }
    run.write_trace = run.write_trace && !run.duql_rows;

    if (!check_missing_indexes(run.files, run.index_dir, run.no_auto_index))
        co_return 1;

    // Trace output is always compressed; ensure the .gz extension.
    run.final_output = dstd::String(dstd::StringView(cli->output));
    if (run.write_trace && !run.final_output.empty()) {
        const dstd::String gz = ".gz";
        if (run.final_output.size() < gz.size() ||
            run.final_output.compare(run.final_output.size() - gz.size(),
                                     gz.size(), gz) != 0)
            run.final_output += gz;
    }

    // The trace-writing path owns its output through the parallel writer; only
    // the FileSink path (stdout / plain non-merge export) opens a FILE here.
    if (!run.final_output.empty() && !run.write_trace) {
        run.out_file = std::fopen(run.final_output.c_str(), "w");
        if (!run.out_file) {
            DFTRACER_UTILS_LOG_ERROR("Failed to open output file: {}",
                                     run.final_output);
            co_return 1;
        }
    }
    dstd::ScopedFile out_owner(
        run.out_file);  // closes out_file on any exit; not stdout
    run.out_target = run.out_file ? run.out_file : stdout;
    FileSink sink(run.out_target);
    run.sink = &sink;

    auto ran = binaries::run_on_pool(
        "DFTracer View", cli->runtime_args,
        [&run](coro::Scope&) -> coro::CoroTask<dstd::Result<void>> {
            co_return co_await run.run();
        });
    if (!ran) {
        DFTRACER_UTILS_LOG_ERROR("Run failed: {}", ran.error().c_str());
        co_return 1;
    }

    print_summary(run.view, run.files.size(), run.stats);

    co_return (run.verify_failed || run.index_merge_failed) ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef DFTRACER_UTILS_ENABLE_MPI
    MPI_Init(&argc, &argv);
    // Local ranks share the node's cores (Dask-style) before any runtime work.
    distributed::set_local_thread_budget();
#endif
    int rc = binaries::cli_main<ViewArgParse>(
        argc, argv, "dftracer_view",
        "Apply filtered views to DFTracer trace files. Uses bloom filter "
        "indices for efficient chunk-skipping. Supports predefined views "
        "(io, compute, dlio), custom recipes, and inline queries. Built with "
        "MPI, run under mpirun to aggregate counters distributively.",
        [](ViewArgParse& cli) -> int {
            auto code = run_view(&cli).get();
            if (!code) {
                DFTRACER_UTILS_LOG_ERROR("Fatal: {}", code.error().format());
                return 1;
            }
            return *code;
        });
#ifdef DFTRACER_UTILS_ENABLE_MPI
    MPI_Finalize();
#endif
    return rc;
}
