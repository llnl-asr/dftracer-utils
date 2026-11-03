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
#include <dftracer/utils/runtime/pipeline/pipeline.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/runtime/tasks/task.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/sharded_view.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>

// MPI is compiled in only when the build enabled it (config.h macro). The same
// source then produces a distribution-capable dftracer_view; without MPI it is
// the plain single-node binary.
#ifdef DFTRACER_UTILS_ENABLE_MPI
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/runtime/distributed/mpi_transport.h>
#include <dftracer/utils/utilities/fileio/parallel/merge.h>

#include <string>
#include <vector>
#endif

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::trace;
using namespace dftracer::utils::index::store;
using namespace dftracer::utils::trace::views;
using namespace dftracer::utils::utilities::filesystem;

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
    binaries::PipelineArgs pipeline;
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
        schema(inputs, pipeline, indexing, duql_args);
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

    void post_parse() override {
        time_bucket = static_cast<std::uint64_t>(std::llround(time_bucket_us));
        occ_cell = static_cast<std::uint64_t>(std::llround(occ_cell_us));
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

// Re-scan a freshly written trace and confirm the event count round-trips.
template <class Configure>
static coro::CoroTask<void> verify_output(const dstd::String& final_output,
                                          const dstd::String& index_dir,
                                          std::size_t checkpoint_size,
                                          Configure&& configure,
                                          std::uint64_t expected_events,
                                          bool& verify_failed) {
    dstd::String output_arg{dstd::StringView(final_output)};
    dstd::String index_dir_arg{dstd::StringView(index_dir)};
    co_await dftracer::utils::index::build::ensure_index_fresh(
        "", std::move(output_arg), std::move(index_dir_arg));
    ViewFile ovf;
    ovf.file_path = final_output;
    const dstd::String ovf_index =
        internal::determine_index_path(final_output, index_dir);
    ovf.index_path.assign(ovf_index.data(), ovf_index.size());
    ovf.checkpoint_size = checkpoint_size;
    NullSink ns;
    View v = configure(View::from_files({ovf}));
    ExportStats vs = co_await v.sink_json(ns);
    if (vs.events_matched != expected_events) {
        DFTRACER_UTILS_LOG_ERROR(
            "Verify failed: wrote %llu events but re-scan found %llu",
            (unsigned long long)expected_events,
            (unsigned long long)vs.events_matched);
        verify_failed = true;
    } else {
        std::fprintf(stderr, "Verify OK: %llu events round-tripped\n",
                     (unsigned long long)expected_events);
    }
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
                DFTRACER_UTILS_LOG_ERROR("%.*s needs a field, e.g. %.*s:dur",
                                         static_cast<int>(op.size()), op.data(),
                                         static_cast<int>(op.size()),
                                         op.data());
                return false;
            }
            double denom = 1.0;
            for (std::size_t i = 1; i < op.size(); ++i) denom *= 10.0;
            const double digits =
                dstd::from_chars<double>(op.substr(1)).value_or(0.0);
            out.push_back(AggSpec(AggOp::Pct, text(field),
                                  text(op) + "_" + text(field), "",
                                  digits / denom));
        } else {
            DFTRACER_UTILS_LOG_ERROR("Unknown --agg reducer: %.*s",
                                     static_cast<int>(tok.size()), tok.data());
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

static coro::CoroTask<int> run_view(const ViewArgParse* cli) {
    const dstd::StringView directory = cli->inputs.directory;
    const dstd::StringView index_dir = cli->indexing.index_dir;
    const dstd::StringView preset = cli->preset;
    const dstd::StringView recipe_path = cli->recipe;
    const dstd::StringView save_recipe = cli->save_recipe;
    const dstd::StringView output_path = cli->output;
    const dstd::StringView time_range_str = cli->time_range;
    const auto min_duration = cli->min_duration;
    const auto max_duration = cli->max_duration;
    const auto no_auto_index = cli->no_auto_index;
    const auto checkpoint_size = cli->indexing.checkpoint_size;
    const dstd::StringView duql_str = cli->duql_args.duql;

    ViewDefinition view;

    if (!preset.empty()) {
        if (preset == "io") {
            view = ViewDefinition::io_view();
        } else if (preset == "compute") {
            view = ViewDefinition::compute_view();
        } else if (preset == "dlio") {
            view = ViewDefinition::dlio_view();
        } else {
            DFTRACER_UTILS_LOG_ERROR(
                "Unknown preset: %s. Use: io, compute, "
                "dlio",
                dstd::String(preset).c_str());
            co_return 1;
        }
    } else if (!recipe_path.empty()) {
        if (!common::unwrap(dstd::fs::exists(dstd::PathView(recipe_path)))) {
            DFTRACER_UTILS_LOG_ERROR("Recipe file not found: %s",
                                     dstd::String(recipe_path).c_str());
            co_return 1;
        }
        auto json_content =
            common::unwrap(dstd::fs::read_file(dstd::PathView(recipe_path)));
        view = ViewDefinition::from_json(dstd::String(json_content));
    } else {
        view.name = "custom";
        view.description = "Custom inline view";
    }

    using duql::Query;
    dstd::Optional<Query> query;
    duql::Params duql_params;
    for (const auto param : cli->params) {
        const dstd::String kv(param);
        const auto eq = kv.find('=');
        if (eq == dstd::String::npos || eq == 0) {
            DFTRACER_UTILS_LOG_ERROR("Invalid --param '%s': use name=value",
                                     kv.c_str());
            co_return 1;
        }
        auto value = duql::parse_param(dstd::StringView(kv).substr(eq + 1));
        if (!value) {
            DFTRACER_UTILS_LOG_ERROR("Invalid --param '%s': %s", kv.c_str(),
                                     value.error().message.c_str());
            co_return 1;
        }
        duql_params.insert_or_assign(kv.substr(0, eq), std::move(*value));
    }
    if (cli->explain && duql_str.empty()) {
        DFTRACER_UTILS_LOG_ERROR("%s", "--explain needs --query.");
        co_return 1;
    }
    if (!duql_str.empty() && !save_recipe.empty()) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s",
            "--save-recipe does not store --duql; write the filter "
            "into the recipe instead.");
        co_return 1;
    }

    dstd::Optional<dstd::Pair<double, double>> time_range;
    if (!time_range_str.empty()) {
        auto comma = time_range_str.find(',');
        const auto min_ts =
            comma == dstd::StringView::npos
                ? dstd::nullopt
                : dstd::from_chars<double>(time_range_str.substr(0, comma));
        const auto max_ts =
            comma == dstd::StringView::npos
                ? dstd::nullopt
                : dstd::from_chars<double>(time_range_str.substr(comma + 1));
        if (min_ts && max_ts) {
            time_range = dstd::Pair<double, double>(*min_ts, *max_ts);
        } else {
            DFTRACER_UTILS_LOG_ERROR(
                "Invalid --time-range format. Use: min,max (e.g., "
                "1000000,2000000)");
            co_return 1;
        }
    }

    if (min_duration > 0 || max_duration > 0) {
        dstd::String extra;
        if (min_duration > 0) {
            if (!extra.empty()) extra += " and ";
            extra +=
                "dur >= " + std::to_string(static_cast<uint64_t>(min_duration));
        }
        if (max_duration > 0) {
            if (!extra.empty()) extra += " and ";
            extra +=
                "dur <= " + std::to_string(static_cast<uint64_t>(max_duration));
        }
        query = duql::parse_or_throw(extra);
    }

    if (query) {
        view.with_query(std::move(*query));
    }

    dstd::Vector<GroupKey> group_keys;
    dstd::Vector<AggSpec> agg_specs;
    if (!parse_group_by(cli->group_by, group_keys)) co_return 1;
    if (!parse_agg(cli->agg, agg_specs)) co_return 1;
    const std::uint64_t time_bucket = cli->time_bucket;
    const bool counters = cli->counters;
    const bool ct_mode = cli->call_tree;
    const bool fg_mode = cli->flamegraph;
    const dstd::Vector<dstd::String> ct_partition =
        texts(binaries::split_csv(cli->ct_partition));
    const bool aggregate = !group_keys.empty() || !agg_specs.empty() ||
                           counters || time_bucket > 0 || cli->agg_numeric_args;
    const bool typed_mode = cli->collect_typed;
    const bool mv_mode = cli->materialize;
    const bool merge = cli->merge;
    const bool no_index = cli->no_index;
    const bool verify = cli->verify;
    // One knob: gzip member size == checkpoint size (a checkpoint is a member).
    const std::uint64_t member_size = checkpoint_size;
    // Event export to a file -> compressed, indexed trace via the parallel
    // writer. Aggregate/counter/containment tables and stdout keep their own
    // paths.
    bool write_trace = !aggregate && !counters && !typed_mode && !mv_mode &&
                       !ct_mode && !fg_mode && !cli->output.empty();

    if (mv_mode && !aggregate) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s", "--materialize needs an aggregation (--group-by/--agg).");
        co_return 1;
    }

    // --phase selects the input phase; "any" reads every record, metadata
    // included. Without it the view reads the source's data.
    Phase view_phase = Phase::Any;
    if (!cli->phase.empty()) {
        if (cli->phase == "events")
            view_phase = Phase::Events;
        else if (cli->phase == "counters")
            view_phase = Phase::Counters;
        else if (cli->phase == "aggregated")
            view_phase = Phase::Aggregated;
        else if (cli->phase == "metadata")
            view_phase = Phase::Metadata;
        else if (cli->phase == "any")
            view_phase = Phase::Any;
        else {
            DFTRACER_UTILS_LOG_ERROR(
                "Unknown --phase: %s. Use events, counters, aggregated, "
                "metadata, or any.",
                dstd::String(cli->phase).c_str());
            co_return 1;
        }
    }

    if (!view.query && duql_str.empty() && !time_range && !aggregate &&
        !merge && !typed_mode && !mv_mode && !ct_mode && !fg_mode) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s",
            "Nothing to do. Use --preset, --recipe, --duql, --time-range, "
            "--merge, --call-tree, --flamegraph, or --group-by/--agg.");
        co_return 1;
    }

    if (!save_recipe.empty()) {
        common::unwrap(
            dstd::fs::write_file(dstd::PathView(save_recipe), view.to_json()));
        std::printf("View recipe saved to: %.*s\n",
                    static_cast<int>(save_recipe.size()), save_recipe.data());
    }

    // A shard set is a directory holding a shards.json manifest. When the
    // target is one, the query runs over the immutable shards via ShardedView
    // and needs no trace-file arguments (the shards are self-describing).
    const dstd::String directory_std(directory);
    const dstd::String index_dir_std(index_dir);
    dstd::String shard_set_root;
    if (!directory.empty())
        shard_set_root = resolve_shard_set_root(directory_std);
    if (shard_set_root.empty() && !index_dir.empty())
        shard_set_root = resolve_shard_set_root(index_dir_std);

    if (!shard_set_root.empty() && !aggregate && !counters) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s",
            "A shard set supports only aggregate/counter queries "
            "(--group-by/--agg/--counters).");
        co_return 1;
    }

    dstd::Vector<dstd::String> files;
    if (!shard_set_root.empty()) {
        // Self-describing: ShardedView enumerates files from each shard index.
    } else if (!directory.empty()) {
        if (!common::input_exists(dstd::CStringView(directory.data()))) {
            DFTRACER_UTILS_LOG_ERROR("Directory does not exist: %s",
                                     dstd::String(directory).c_str());
            co_return 1;
        }

        auto matched =
            binaries::scan_trace_files_on_pool(cli->pipeline, directory,
                                               /*populate_size=*/false);

        for (const auto& entry : matched) {
            files.emplace_back(entry.path.str());
        }
        std::sort(files.begin(), files.end());

        if (files.empty()) {
            DFTRACER_UTILS_LOG_ERROR("No trace files found in: %s",
                                     dstd::String(directory).c_str());
            co_return 1;
        }
    }
    for (const auto file : cli->inputs.files) files.emplace_back(file);
    if (shard_set_root.empty() && files.empty()) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s", "No files or folder specified. Use --help for usage.");
        co_return 1;
    }

    // One View over every file; the executor plans and scans them in parallel.
    dstd::Vector<ViewFile> view_files;
    view_files.reserve(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        ViewFile vf;
        vf.file_path = files[i];
        vf.index_path = internal::determine_index_path(files[i], index_dir);
        vf.checkpoint_size = checkpoint_size;
        view_files.push_back(std::move(vf));
    }

    // Stages after the scan give a table, not events; --explain prints the
    // plan without scanning.
    bool duql_rows = false;
    if (!duql_str.empty()) {
        // duql types fields from the index catalog, so a missing index is
        // built before the query compiles. A distributed aggregation builds
        // its shards' indexes itself.
        if (!no_auto_index && !aggregate && !counters && !fg_mode)
            binaries::run_on_pool(
                cli->pipeline, "dftracer_view index",
                [&](coro::Scope&) -> coro::CoroTask<void> {
                    auto to_check = files;
                    dstd::String dir_arg{dstd::StringView(index_dir_std)};
                    co_await dftracer::utils::index::build::
                        ensure_indexes_fresh("", std::move(to_check),
                                             std::move(dir_arg));
                });
        dstd::String plan;
        try {
            View probe = View::from_files(view_files);
            if (!cli->record_schema.empty())
                probe = probe.record_schema(dstd::String(cli->record_schema));
            plan = probe.explain_duql(duql_str, duql_params);
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_ERROR("Invalid --duql: %s", e.what());
            co_return 1;
        }
        if (cli->explain) {
            std::fputs(plan.c_str(), stdout);
            co_return 0;
        }
        // Sides only feed the scan filter; their lines are indented or
        // start with "side " or "reads ".
        for (std::size_t at = 0; at < plan.size();) {
            const dstd::StringView line(plan.data() + at,
                                        plan.find('\n', at) - at);
            if (!line.starts_with("scan ") && !line.starts_with("side ") &&
                !line.starts_with("reads ") && !line.starts_with(" "))
                duql_rows = true;
            at += line.size() + 1;
        }
    }
    if (duql_rows && (aggregate || counters || typed_mode || mv_mode ||
                      ct_mode || fg_mode || merge)) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s",
            "--duql stages after the scan do not combine with "
            "aggregation, counters, typed, materialize, call-tree, "
            "flamegraph or merge output.");
        co_return 1;
    }
    write_trace = write_trace && !duql_rows;

    dstd::Vector<dstd::String> files_needing_index;
    for (const auto& file_path : files) {
        const dstd::String index_path =
            internal::determine_index_path(file_path, index_dir);
        if (!common::unwrap(dstd::fs::exists(dstd::PathView(
                dftracer::utils::index::store_dir(index_path))))) {
            files_needing_index.push_back(file_path);
        }
    }

    if (!files_needing_index.empty()) {
        if (no_auto_index) {
            DFTRACER_UTILS_LOG_ERROR(
                "Missing .dftindex store for %zu file(s) and --no-auto-index "
                "is "
                "set. Run dftracer_index first.",
                files_needing_index.size());
            for (const auto& f : files_needing_index) {
                std::fprintf(stderr, "  Missing index: %s\n", f.c_str());
            }
            co_return 1;
        }

        // Progress goes to stderr so it never pollutes the data on stdout.
        std::fprintf(stderr, "Auto-building index for %zu file(s)...\n",
                     files_needing_index.size());
    }

    // Trace output is always compressed; ensure the .gz extension.
    dstd::String final_output(output_path);
    if (write_trace && !final_output.empty()) {
        const dstd::String gz = ".gz";
        if (final_output.size() < gz.size() ||
            final_output.compare(final_output.size() - gz.size(), gz.size(),
                                 gz) != 0)
            final_output += gz;
    }

    // The trace-writing path owns its output through the parallel writer; only
    // the FileSink path (stdout / plain non-merge export) opens a FILE here.
    FILE* out_file = nullptr;
    if (!final_output.empty() && !write_trace) {
        out_file = std::fopen(final_output.c_str(), "w");
        if (!out_file) {
            DFTRACER_UTILS_LOG_ERROR("Failed to open output file: %s",
                                     final_output.c_str());
            co_return 1;
        }
    }
    dstd::ScopedFile out_owner(
        out_file);  // closes out_file on any exit; not stdout
    FILE* out_target = out_file ? out_file : stdout;
    FileSink sink(out_target);
    bool verify_failed = false;
    bool index_merge_failed = false;

    auto pipeline_config =
        binaries::build_pipeline_config("DFTracer View", cli->pipeline);
    Pipeline pipeline(pipeline_config);

    ExportStats stats;
    // collect() returns only its table, so its scans count here.
    auto scans = std::make_shared<ScanCounter>();
    // Apply the parsed view options to a base View; shared by the single-node
    // and distributed paths so both aggregate identically. The projection and
    // row window (--select/--offset/--limit) are applied by `windowed`.
    auto configure = [&](View v) {
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
    };
    const bool has_window = cli->offset > 0 || cli->limit > 0;
    const std::int64_t window_len =
        cli->limit > 0 ? static_cast<std::int64_t>(cli->limit)
                       : std::numeric_limits<std::int64_t>::max();
    auto windowed = [&](View v) {
        if (!cli->select.empty())
            v = v.select(texts(binaries::split_csv(cli->select)));
        if (has_window)
            v = v.slice(static_cast<std::int64_t>(cli->offset), window_len);
        return v;
    };
    // The same projection and window over a table merged from partials.
    auto windowed_table = [&](dataframe::DataFrame table)
        -> coro::CoroTask<dataframe::DataFrame> {
        if (cli->select.empty() && !has_window) co_return table;
        dataframe::LazyFrame rows = dataframe::lazy(std::move(table));
        if (!cli->select.empty()) {
            std::vector<std::string> cols;  // keep std: LazyFrame::select
            for (const auto c : binaries::split_csv(cli->select))
                cols.emplace_back(c);
            rows = rows.select(std::move(cols));
        }
        if (has_window)
            rows =
                rows.slice(static_cast<std::int64_t>(cli->offset), window_len);
        co_return co_await rows.collect();
    };
    auto combined_task = make_task(
        [&](CoroScope&) -> coro::CoroTask<void> {
#ifdef DFTRACER_UTILS_ENABLE_MPI
            distributed::MpiTransport transport;
            // Distributed aggregation: each rank aggregates its shard into
            // a serialized partial, the partials are gathered, and rank 0
            // merges and emits (counters -> ph="C" events; aggregate ->
            // collect table). Only these reductions distribute; other modes
            // fall through to rank 0 (single-node) below.
            if ((counters || aggregate || fg_mode) && transport.size() > 1 &&
                shard_set_root.empty()) {
                auto shard =
                    shard_files(files, transport.rank(), transport.size());
                // Each rank builds its own shard of the root, on node-local
                // scratch when the destination warrants it, and publishes
                // it as <root>/shards/<rank>. Rank 0 merges the shards once
                // every rank published its own.
                const dstd::String idx_root =
                    internal::determine_index_path(files.front(), index_dir);
                const dstd::String rank_idx =
                    dftracer::utils::index::shard_root(
                        idx_root, static_cast<std::size_t>(transport.rank()));
                if (!no_auto_index) {
                    // Build each rank's index with its aggregation tier.
                    dftracer::utils::index::build::ResolveAndBuildInput bin;
                    bin.files = shard;
                    bin.index_dir = rank_idx;
                    bin.checkpoint_size = checkpoint_size;
                    bin.require_checkpoints = true;
                    co_await dftracer::utils::index::build::
                        resolve_and_build_index(std::move(bin));
                }
                dstd::Vector<ViewFile> shard_files_vf;
                for (const auto& f : shard) {
                    ViewFile vf;
                    vf.file_path = f;
                    vf.index_path = rank_idx;
                    vf.checkpoint_size = checkpoint_size;
                    shard_files_vf.push_back(std::move(vf));
                }
                View shard_view =
                    configure(View::from_files(std::move(shard_files_vf)));
                dataframe::LazyResult<dstd::String> partial_plan =
                    fg_mode ? shard_view.flamegraph_partial(ct_partition)
                            : shard_view.aggregate_partial();
                dstd::String partial = co_await partial_plan.collect();
                auto partials = transport.all_gather(partial);
                if (transport.rank() == 0) {
                    // Every rank published its shard before the gather, so
                    // the set is complete: catalog it and merge the shards
                    // into the root's own store, leaving one index for a
                    // later query. A failed merge keeps the manifest and
                    // the shards, so a later call retries it without a
                    // rebuild.
                    if (!no_auto_index) {
                        dstd::Vector<dstd::String> shard_dirs;
                        for (int r = 0; r < transport.size(); ++r) {
                            dstd::String d(dftracer::utils::index::shard_root(
                                idx_root, static_cast<std::size_t>(r)));
                            if (common::unwrap(dstd::fs::exists(dstd::PathView(
                                    dftracer::utils::index::store_dir(d)))))
                                shard_dirs.push_back(std::move(d));
                        }
                        if (!shard_dirs.empty()) {
                            try {
                                write_shard_set(idx_root, shard_dirs);
                                consolidate_shard_set(idx_root);
                            } catch (const std::exception& e) {
                                index_merge_failed = true;
                                DFTRACER_UTILS_LOG_ERROR(
                                    "shards not merged into %s: %s",
                                    idx_root.c_str(), e.what());
                            }
                        }
                    }

                    dstd::Vector<dstd::StringView> pv(partials.begin(),
                                                      partials.end());
                    View merger = configure(View::from_files({}));
                    if (fg_mode) {
                        dataframe::DataFrame table =
                            View::merge_flamegraph_partials(pv);
                        print_table(out_target, table);
                        stats.events_matched = table.num_rows();
                    } else if (counters) {
                        stats = merger.merge_counter_partials(pv, sink);
                    } else {
                        dataframe::DataFrame merged = merger.merge_partials(pv);
                        dataframe::DataFrame table =
                            co_await windowed_table(std::move(merged));
                        print_table(out_target, table);
                        stats.events_matched = table.num_rows();
                    }
                }
                co_return;
            }
            // Distributed: each rank writes its shard, rank 0 concatenates.
            if (write_trace && transport.size() > 1) {
                auto shard =
                    shard_files(files, transport.rank(), transport.size());
                dstd::String idx_base(index_dir);
                if (idx_base.empty())
                    idx_base = dstd::String(
                        dstd::PathView(files.front()).parent().str());
                dstd::String rank_idx =
                    idx_base + "/rank_" + dstd::to_string(transport.rank());
                if (!no_auto_index) {
                    auto to_check = shard;
                    dstd::String dir_arg{dstd::StringView(rank_idx)};
                    co_await dftracer::utils::index::build::
                        ensure_indexes_fresh("", std::move(to_check),
                                             std::move(dir_arg));
                }
                dstd::Vector<ViewFile> shard_vf;
                for (const auto& f : shard) {
                    ViewFile vf;
                    vf.file_path = f;
                    vf.index_path = internal::determine_index_path(f, rank_idx);
                    vf.checkpoint_size = checkpoint_size;
                    shard_vf.push_back(std::move(vf));
                }
                const dstd::String rank_out =
                    final_output + ".rank_" + dstd::to_string(transport.rank());
                TraceWriteOptions topts;
                topts.output_path = rank_out;
                topts.member_size = member_size;
                topts.num_workers = cli->pipeline.executor_threads;
                topts.compress = true;
                View shard_view =
                    windowed(configure(View::from_files(std::move(shard_vf))));
                stats = co_await shard_view.sink_trace(topts);
                auto rank_outs = transport.all_gather(rank_out);
                if (transport.rank() == 0) {
                    const auto merge_rc =
                        co_await fileio::parallel::merge_shards(final_output,
                                                                rank_outs);
                    if (merge_rc != 0) {
                        DFTRACER_UTILS_LOG_ERROR("merge_shards failed for %s",
                                                 final_output.c_str());
                    } else if (!no_index) {
                        dstd::String file_arg{dstd::StringView(final_output)};
                        dstd::String dir_arg{dstd::StringView(index_dir)};
                        co_await dftracer::utils::index::build::
                            ensure_index_fresh("", std::move(file_arg),
                                               std::move(dir_arg));
                    }
                }
                co_return;
            }
            // Non-distributed modes run on rank 0 only (correct, not
            // sharded).
            if (transport.rank() != 0) co_return;
#endif
            // Shard set: merge the query across the immutable shards
            // read-only, no trace-file scan when each shard's aggregation
            // tier covers it.
            if (!shard_set_root.empty()) {
                ShardedView sv = ShardedView::from_manifest(shard_set_root);
                if (counters) {
                    stats = co_await sv.aggregate_counters(configure, sink);
                } else {
                    dataframe::DataFrame merged =
                        co_await sv.aggregate(configure);
                    dataframe::DataFrame table =
                        co_await windowed_table(std::move(merged));
                    print_table(out_target, table);
                    stats.events_matched = table.num_rows();
                }
                co_return;
            }

            // The typed, rollup and counter terminals read the whole
            // aggregation; the projection and row window apply to the other
            // outputs.
            View base = configure(View::from_files(view_files));
            View v = windowed(base);

            // Skip the eager pre-build when the query would take the
            // raw-gzip bootstrap (answer the query and build the index in
            // one pass); pre-building first would make the index exist so
            // the bootstrap never fires. Only the plain export / collect
            // paths bootstrap.
            const bool will_bootstrap =
                (aggregate && base.collect_would_bootstrap()) ||
                (!counters && !aggregate && !write_trace && !duql_rows &&
                 v.export_would_bootstrap());
            if (!no_auto_index && !will_bootstrap) {
                auto to_check = files;
                dstd::String dir_arg{dstd::StringView(index_dir_std)};
                co_await dftracer::utils::index::build::ensure_indexes_fresh(
                    "", std::move(to_check), std::move(dir_arg));
            }

            if (ct_mode || fg_mode) {
                dataframe::LazyFrame tree = fg_mode ? v.flamegraph(ct_partition)
                                                    : v.call_tree(ct_partition);
                dataframe::DataFrame table = co_await tree.collect();
                print_table(out_target, table);
                stats.events_matched = table.num_rows();
            } else if (mv_mode) {
                // Build-only terminal: compute the aggregation and persist
                // it as a rollup so a later matching query hits the fast
                // path.
                stats = co_await base.materialize();
                std::fprintf(stderr, "Materialized rollup (%llu events).\n",
                             (unsigned long long)stats.events_matched);
            } else if (typed_mode) {
                auto tr = co_await base.collect_typed();
                emit_typed(out_target, tr.regular, tr.aggregated, tr.counters);
                stats.events_matched = static_cast<std::uint64_t>(
                    tr.regular.num_rows() + tr.aggregated.num_rows() +
                    tr.counters.num_rows());
            } else if (counters) {
                stats = co_await base.sink_counters(sink);
            } else if (aggregate || duql_rows) {
                dataframe::DataFrame table = co_await v.collect();
                print_table(out_target, table);
                stats.events_matched = table.num_rows();
                stats.events_scanned = scans->events_scanned.load();
                stats.chunks_scanned = scans->chunks_scanned.load();
                stats.chunks_skipped = scans->chunks_skipped.load();
            } else if (write_trace) {
                // Fused: export_trace builds the index during the write (no
                // re-inflate of the output) unless --no-index opts out.
                TraceWriteOptions topts;
                topts.output_path = final_output;
                topts.member_size = member_size;
                topts.num_workers = cli->pipeline.executor_threads;
                topts.compress = true;
                topts.build_index = !no_index;
                topts.index_path = index_dir;
                stats = co_await v.sink_trace(topts);
            } else {
                stats = co_await v.sink_json(sink);
            }

            if (out_file) std::fflush(out_file);

            if (verify && write_trace) {
                co_await verify_output(
                    final_output, index_dir_std, checkpoint_size,
                    [&](View b) { return windowed(configure(b)); },
                    stats.events_matched, verify_failed);
            }
            co_return;
        },
        "DFTracerView");

    pipeline.set_source(combined_task);
    pipeline.set_destination(combined_task);
    try {
        pipeline.execute();
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Pipeline failed: %s", e.what());
        co_return 1;
    }

    std::fprintf(stderr,
                 "View: %s | Files: %zu | Chunks: scanned=%llu skipped=%llu | "
                 "Events/rows: %llu (scanned=%llu)\n",
                 view.name.c_str(), files.size(),
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

    co_return (verify_failed || index_merge_failed) ? 1 : 0;
}

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
            try {
                return run_view(&cli).get();
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_ERROR("Fatal: %s", e.what());
                return 1;
            }
        });
#ifdef DFTRACER_UTILS_ENABLE_MPI
    MPI_Finalize();
#endif
    return rc;
}
