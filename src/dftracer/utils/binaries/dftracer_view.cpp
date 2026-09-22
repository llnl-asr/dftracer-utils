#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/binaries/json_cell_printer.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/memory_budget.h>  // NO_SPILL_BUDGET
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/trace/aggregators/aggregation_config.h>
#include <dftracer/utils/trace/indexing/resolve_and_build.h>
#include <dftracer/utils/trace/indexing/shard_manifest.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/result_batch.h>
#include <dftracer/utils/trace/views/sharded_view.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// MPI is compiled in only when the build enabled it (config.h macro). The same
// source then produces a distribution-capable dftracer_view; without MPI it is
// the plain single-node binary.
#ifdef DFTRACER_UTILS_ENABLE_MPI
#include <dftracer/utils/core/distributed/mpi_transport.h>
#include <dftracer/utils/utilities/fileio/parallel/merge.h>
#endif

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::trace;
using namespace dftracer::utils::trace::views;
using namespace dftracer::utils::utilities::filesystem;

namespace {
// Owns a FILE* opened for output and closes it on scope exit, so a throw from
// the export ladder cannot leak it. A null handle closes nothing; stdout is
// written through the raw pointer and never owned here.
class FileHandle {
   public:
    FileHandle() = default;
    explicit FileHandle(std::FILE* f) noexcept : f_(f) {}
    ~FileHandle() {
        if (f_) std::fclose(f_);
    }
    FileHandle(FileHandle&& o) noexcept : f_(o.f_) { o.f_ = nullptr; }
    FileHandle& operator=(FileHandle&& o) noexcept {
        if (this != &o) {
            if (f_) std::fclose(f_);
            f_ = o.f_;
            o.f_ = nullptr;
        }
        return *this;
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    std::FILE* get() const noexcept { return f_; }

   private:
    std::FILE* f_ = nullptr;
};
}  // namespace

#ifdef DFTRACER_UTILS_ENABLE_MPI
// The files owned by `rank` of `size`: a deterministic round-robin split, so
// every rank derives the same assignment from the same sorted file list.
static std::vector<std::string> shard_files(
    const std::vector<std::string>& all_files, int rank, int size) {
    std::vector<std::string> shard;
    for (std::size_t i = 0; i < all_files.size(); ++i)
        if (static_cast<int>(i % size) == rank) shard.push_back(all_files[i]);
    return shard;
}
#endif

class ViewArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory{cli::DirMode::DEFAULT_EMPTY};
    cli::FilesArgs files_args;
    cli::PipelineArgs pipeline;
    cli::IndexingArgs indexing;
    cli::QueryArgs query_args;

    std::string preset;
    std::string recipe;
    std::string save_recipe;
    std::string time_range;
    double min_duration = 0.0;
    double max_duration = 0.0;
    std::string output;
    bool stream = false;
    bool no_metadata = false;
    bool no_auto_index = false;
    std::string group_by;
    std::string agg;
    std::uint64_t time_bucket = 0;
    std::uint64_t occ_cell = 0;
    bool counters = false;
    bool call_tree = false;
    bool flamegraph = false;
    std::string ct_partition;
    std::string phase;
    bool merge = false;
    bool no_index = false;
    bool verify = false;
    std::uint64_t limit = 0;
    std::uint64_t offset = 0;
    std::string select;
    double time_scale = 0.0;
    std::uint64_t memory_budget = 0;
    bool no_spill = false;
    bool agg_numeric_args = false;
    bool collect_typed = false;
    bool materialize = false;

    explicit ViewArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        indexing.with_force = false;
        indexing.index_dir_help =
            "Directory where .dftindex stores are created";
        schema(directory, files_args, pipeline, indexing, query_args);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("--preset")
            .help("Predefined view: io, compute, dlio")
            .default_value<std::string>("");

        parser()
            .add_argument("--recipe")
            .help("Custom view JSON file path")
            .default_value<std::string>("");

        parser()
            .add_argument("--save-recipe")
            .help("Save the constructed view to a JSON file")
            .default_value<std::string>("");

        parser()
            .add_argument("--time-range")
            .help(
                "Timestamp filter as min,max in microseconds (e.g., "
                "1000000,2000000)")
            .default_value<std::string>("");

        parser()
            .add_argument("--min-duration")
            .help(
                "Minimum event duration (a bare number is microseconds; "
                "suffixed values like 5ms are converted)")
            .default_value(std::string("0"));

        parser()
            .add_argument("--max-duration")
            .help(
                "Maximum event duration (a bare number is microseconds; "
                "suffixed values like 5ms are converted)")
            .default_value(std::string("0"));

        parser()
            .add_argument("-o", "--output")
            .help("Output file path (default: stdout)")
            .default_value<std::string>("");

        parser()
            .add_argument("--stream")
            .help("Stream matching events to stdout as NDJSON")
            .flag();

        parser()
            .add_argument("--no-metadata")
            .help("Exclude metadata events (ph=M) from output")
            .flag();

        parser()
            .add_argument("--no-auto-index")
            .help(
                "Disable automatic index building for files missing .dftindex")
            .flag();

        parser()
            .add_argument("--group-by")
            .help(
                "Aggregate: group by columns, comma-separated. Any field name "
                "works (top-level then args); named dims name/cat/pid/tid/"
                "fhash/hhash/io_cat/acc_pat/file_path/file_name/host_name/"
                "rank, "
                "or arg:KEY / args.KEY for an args field")
            .default_value<std::string>("");

        parser()
            .add_argument("--agg")
            .help(
                "Aggregate: reducers, comma-separated (count, sum:FIELD, "
                "min:FIELD, max:FIELD, mean:FIELD, var:FIELD, std:FIELD, "
                "skew:FIELD, kurt:FIELD, pNN:FIELD e.g. p99:dur, pct:FIELD:Q)")
            .default_value<std::string>("");

        parser()
            .add_argument("--time-bucket")
            .help(
                "Aggregate into time buckets of this width (a bare number is "
                "microseconds; suffixed values like 1ms are converted)")
            .default_value(std::string("0"));

        parser()
            .add_argument("--occ-cell")
            .help(
                "Occupancy cell size (busy quantum) for busy/concurrency/"
                "utilization; a bare number is microseconds, 0 = default. "
                "Finer resolves overlap on short events (honored with "
                "--time-range)")
            .default_value(std::string("0"));

        parser()
            .add_argument("--counters")
            .help("Emit the aggregation as ph=C counter events")
            .flag();

        parser()
            .add_argument("--call-tree")
            .help("Containment call tree: events plus level/parent_id per lane")
            .flag();
        parser()
            .add_argument("--flamegraph")
            .help(
                "Folded call tree (flamegraph) node frame; distributes under "
                "MPI via arena partials")
            .flag();
        parser()
            .add_argument("--ct-partition")
            .help(
                "Comma-separated lane keys for --call-tree/--flamegraph "
                "(default pid,tid)")
            .default_value<std::string>("pid,tid");

        parser()
            .add_argument("--phase")
            .help("Select events by phase: events (ph=X), counters (ph=C), any")
            .default_value<std::string>("");

        parser()
            .add_argument("--merge")
            .help("Merge all inputs into one trace written to --output")
            .flag();

        parser()
            .add_argument("--no-index")
            .help(
                "Do not build an index for the written trace (default: index)")
            .flag();

        parser()
            .add_argument("--verify")
            .help(
                "Re-scan the exported output and confirm the event count "
                "round-trips")
            .flag();

        parser()
            .add_argument("--limit")
            .help("Cap the output to N rows/events (0 = unlimited)")
            .scan<'i', std::uint64_t>()
            .default_value<std::uint64_t>(0);

        parser()
            .add_argument("--offset")
            .help("Skip the first N rows/events before applying --limit")
            .scan<'i', std::uint64_t>()
            .default_value<std::uint64_t>(0);

        parser()
            .add_argument("--select")
            .help("Project the result to these columns, comma-separated")
            .default_value<std::string>("");

        parser()
            .add_argument("--time-scale")
            .help(
                "Scale timestamps/durations by this ns-per-unit ratio "
                "(0 = leave as stored)")
            .scan<'g', double>()
            .default_value(static_cast<double>(0.0));

        parser()
            .add_argument("--memory-budget")
            .help(
                "Spill aggregation to disk past this many in-core bytes (0 = "
                "auto: ~1/3 of available memory). Accepts units, e.g. 512MB, "
                "4GB")
            .default_value(std::string("0"));

        parser()
            .add_argument("--no-spill")
            .help(
                "Keep aggregation fully in memory (disable the default spill)")
            .flag();

        parser()
            .add_argument("--agg-numeric-args")
            .help("Aggregate every numeric args.* field automatically")
            .flag();

        parser()
            .add_argument("--collect-typed")
            .help(
                "Dump the aggregation index's regular/aggregated/counters "
                "families (one pass)")
            .flag();

        parser()
            .add_argument("--materialize")
            .help(
                "Persist this aggregation as a rollup for instant reuse "
                "(needs --group-by/--agg)")
            .flag();
    }

    void post_parse() override {
        preset = parser().get<std::string>("--preset");
        recipe = parser().get<std::string>("--recipe");
        save_recipe = parser().get<std::string>("--save-recipe");
        time_range = parser().get<std::string>("--time-range");
        min_duration = cli::get_duration_arg(parser(), "--min-duration", 1e6);
        max_duration = cli::get_duration_arg(parser(), "--max-duration", 1e6);
        output = parser().get<std::string>("--output");
        stream = parser().get<bool>("--stream");
        no_metadata = parser().get<bool>("--no-metadata");
        no_auto_index = parser().get<bool>("--no-auto-index");
        group_by = parser().get<std::string>("--group-by");
        agg = parser().get<std::string>("--agg");
        time_bucket = static_cast<std::uint64_t>(std::llround(
            cli::get_duration_arg(parser(), "--time-bucket", 1e6)));
        occ_cell = static_cast<std::uint64_t>(
            std::llround(cli::get_duration_arg(parser(), "--occ-cell", 1e6)));
        counters = parser().get<bool>("--counters");
        call_tree = parser().get<bool>("--call-tree");
        flamegraph = parser().get<bool>("--flamegraph");
        ct_partition = parser().get<std::string>("--ct-partition");
        phase = parser().get<std::string>("--phase");
        merge = parser().get<bool>("--merge");
        no_index = parser().get<bool>("--no-index");
        verify = parser().get<bool>("--verify");
        limit = parser().get<std::uint64_t>("--limit");
        offset = parser().get<std::uint64_t>("--offset");
        select = parser().get<std::string>("--select");
        time_scale = parser().get<double>("--time-scale");
        memory_budget = cli::get_bytes_arg(parser(), "--memory-budget");
        no_spill = parser().get<bool>("--no-spill");
        agg_numeric_args = parser().get<bool>("--agg-numeric-args");
        collect_typed = parser().get<bool>("--collect-typed");
        materialize = parser().get<bool>("--materialize");
    }
};

class FileSink : public ExportSink {
   public:
    explicit FileSink(FILE* f) : f_(f) {}
    void write(std::string_view data) override {
        std::fwrite(data.data(), 1, data.size(), f_);
    }

   private:
    FILE* f_;
};

// Discards everything. Used by --verify to drive a scan for its event count
// without materializing output.
class NullSink : public ExportSink {
   public:
    void write(std::string_view) override {}
};

// Re-scan a freshly written trace and confirm the event count round-trips.
template <class Configure>
static coro::CoroTask<void> verify_output(
    CoroScope& ctx, const std::string& final_output,
    const std::string& index_dir, std::size_t checkpoint_size,
    Configure&& configure, std::uint64_t expected_events, bool& verify_failed) {
    co_await indexing::ensure_index_fresh(&ctx, "", final_output, index_dir);
    ViewFile ovf;
    ovf.file_path = final_output;
    ovf.index_path = internal::determine_index_path(final_output, index_dir);
    ovf.checkpoint_size = checkpoint_size;
    NullSink ns;
    View v = configure(View::from_files({ovf}));
    ExportStats vs = co_await v.export_json(ns);
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

static bool parse_group_by(const std::string& spec,
                           std::vector<GroupKey>& out) {
    std::size_t pos = 0;
    while (pos < spec.size()) {
        auto comma = spec.find(',', pos);
        std::string tok = spec.substr(pos, comma - pos);
        pos = (comma == std::string::npos) ? spec.size() : comma + 1;
        if (tok.empty()) continue;
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
        else if (tok.rfind("arg:", 0) == 0)
            out.push_back(GroupKey::of_arg(tok.substr(4)));
        else
            // Schemaless fallback: a bare name resolves top-level then args; a
            // dotted/bracketed path (a.b, a[0], args.a.b) resolves that path.
            out.push_back(GroupKey::field(tok));
    }
    return true;
}

static bool parse_agg(const std::string& spec, std::vector<AggSpec>& out) {
    std::size_t pos = 0;
    while (pos < spec.size()) {
        auto comma = spec.find(',', pos);
        std::string tok = spec.substr(pos, comma - pos);
        pos = (comma == std::string::npos) ? spec.size() : comma + 1;
        if (tok.empty()) continue;
        auto colon = tok.find(':');
        std::string op = tok.substr(0, colon);
        std::string field =
            colon == std::string::npos ? "" : tok.substr(colon + 1);
        if (op == "count")
            out.push_back({AggOp::Count, "", ""});
        else if (op == "sum")
            out.push_back({AggOp::Sum, field, ""});
        else if (op == "min")
            out.push_back({AggOp::Min, field, ""});
        else if (op == "max")
            out.push_back({AggOp::Max, field, ""});
        else if (op == "mean")
            out.push_back({AggOp::Mean, field, ""});
        else if (op == "var")
            out.push_back({AggOp::Var, field, ""});
        else if (op == "std")
            out.push_back({AggOp::Std, field, ""});
        else if (op == "skew")
            out.push_back({AggOp::Skew, field, ""});
        else if (op == "kurt")
            out.push_back({AggOp::Kurt, field, ""});
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
            auto c2 = field.find(':');
            const double q = c2 == std::string::npos
                                 ? 0.0
                                 : std::strtod(field.c_str() + c2 + 1, nullptr);
            if (c2 == std::string::npos || c2 == 0 || q <= 0.0 || q >= 1.0) {
                DFTRACER_UTILS_LOG_ERROR(
                    "pct needs pct:FIELD:Q with 0 < Q < 1 (e.g. pct:dur:0.99)");
                return false;
            }
            out.push_back(AggSpec(AggOp::Pct, field.substr(0, c2), "", "", q));
        } else if (op.size() >= 2 && op[0] == 'p' &&
                   op.find_first_not_of("0123456789", 1) == std::string::npos) {
            // pNN shorthand: p50/p90/p99/p999 -> the 0.NN quantile of FIELD.
            if (field.empty()) {
                DFTRACER_UTILS_LOG_ERROR("%s needs a field, e.g. %s:dur",
                                         op.c_str(), op.c_str());
                return false;
            }
            double denom = 1.0;
            for (std::size_t i = 1; i < op.size(); ++i) denom *= 10.0;
            out.push_back(
                AggSpec(AggOp::Pct, field, op + "_" + field, "",
                        std::strtod(op.c_str() + 1, nullptr) / denom));
        } else {
            DFTRACER_UTILS_LOG_ERROR("Unknown --agg reducer: %s", tok.c_str());
            return false;
        }
    }
    return true;
}

// Print a collect() result as one JSON object per row.
static void print_table(FILE* out, const dataframe::DataFrame& table) {
    const std::int64_t nrows = table.num_rows();
    for (std::int64_t r = 0; r < nrows; ++r) {
        std::string s = "{";
        bool first = true;
        for (std::size_t c = 0; c < table.columns.size(); ++c) {
            if (!binaries::cli_emittable(table.columns[c].type())) continue;
            if (!first) s += ",";
            first = false;
            s += "\"" + table.names[c] + "\":";
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
    const std::pair<const char*, const dataframe::DataFrame*> families[] = {
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
    const auto& directory = cli->directory.value;
    const auto& index_dir = cli->indexing.index_dir;
    const auto& preset = cli->preset;
    const auto& recipe_path = cli->recipe;
    const auto& save_recipe = cli->save_recipe;
    const auto& output_path = cli->output;
    const auto& time_range_str = cli->time_range;
    const auto min_duration = cli->min_duration;
    const auto max_duration = cli->max_duration;
    const auto stream_mode = cli->stream;
    const auto no_metadata = cli->no_metadata;
    const auto no_auto_index = cli->no_auto_index;
    const auto checkpoint_size = cli->indexing.checkpoint_size;
    const auto& query_str = cli->query_args.query;

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
                preset.c_str());
            co_return 1;
        }
    } else if (!recipe_path.empty()) {
        if (!fs::exists(recipe_path)) {
            DFTRACER_UTILS_LOG_ERROR("Recipe file not found: %s",
                                     recipe_path.c_str());
            co_return 1;
        }
        std::ifstream recipe_file(recipe_path);
        std::string json_content((std::istreambuf_iterator<char>(recipe_file)),
                                 std::istreambuf_iterator<char>());
        view = ViewDefinition::from_json(json_content);
    } else {
        view.name = "custom";
        view.description = "Custom inline view";
    }

    using query::Query;
    std::optional<Query> query;
    if (!query_str.empty()) {
        auto result = Query::from_string(query_str);
        if (!result) {
            DFTRACER_UTILS_LOG_ERROR("Invalid --query: %s",
                                     result.error().format().c_str());
            co_return 1;
        }
        query = std::move(*result);
    }

    std::optional<std::pair<double, double>> time_range;
    if (!time_range_str.empty()) {
        auto comma = time_range_str.find(',');
        if (comma != std::string::npos) {
            double min_ts = std::stod(time_range_str.substr(0, comma));
            double max_ts = std::stod(time_range_str.substr(comma + 1));
            time_range = std::make_pair(min_ts, max_ts);
        } else {
            DFTRACER_UTILS_LOG_ERROR(
                "Invalid --time-range format. Use: min,max (e.g., "
                "1000000,2000000)");
            co_return 1;
        }
    }

    // --time-range folds a ts predicate here for exact per-event filtering; it
    // ALSO drives View::time_range() in `configure`, which prunes whole chunks
    // by their ts range (a speed hint, not an exact filter on its own).
    if (time_range || min_duration > 0 || max_duration > 0) {
        std::string extra;
        if (time_range) {
            extra += "ts >= " +
                     std::to_string(static_cast<uint64_t>(time_range->first));
            extra += " and ts <= " +
                     std::to_string(static_cast<uint64_t>(time_range->second));
        }
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
        if (query) {
            std::string combined = "(" + query->source() + ") and " + extra;
            query = query::parse_or_throw(combined);
        } else {
            query = query::parse_or_throw(extra);
        }
    }

    if (query) {
        view.with_query(std::move(*query));
    }

    if (no_metadata) {
        view.with_include_metadata(false);
    }

    std::vector<GroupKey> group_keys;
    std::vector<AggSpec> agg_specs;
    if (!parse_group_by(cli->group_by, group_keys)) co_return 1;
    if (!parse_agg(cli->agg, agg_specs)) co_return 1;
    const std::uint64_t time_bucket = cli->time_bucket;
    const bool counters = cli->counters;
    const bool ct_mode = cli->call_tree;
    const bool fg_mode = cli->flamegraph;
    const std::vector<std::string> ct_partition =
        cli::split_csv(cli->ct_partition);
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
    const bool write_trace = !aggregate && !counters && !typed_mode &&
                             !mv_mode && !ct_mode && !fg_mode &&
                             !cli->output.empty();

    if (mv_mode && !aggregate) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s", "--materialize needs an aggregation (--group-by/--agg).");
        co_return 1;
    }

    // --phase selects the input phase; the default spans all phases.
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
                "Unknown --phase: %s. Use events, counters, or any.",
                cli->phase.c_str());
            co_return 1;
        }
    }

    if (!view.query && !aggregate && !merge && !typed_mode && !mv_mode &&
        !ct_mode && !fg_mode) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s",
            "Nothing to do. Use --preset, --recipe, --query, --merge, "
            "--call-tree, --flamegraph, or --group-by/--agg.");
        co_return 1;
    }

    if (!save_recipe.empty()) {
        std::ofstream out(save_recipe);
        out << view.to_json();
        out.close();
        std::printf("View recipe saved to: %s\n", save_recipe.c_str());
    }

    // A shard set is a directory holding a shards.json manifest. When the
    // target is one, the query runs over the immutable shards via ShardedView
    // and needs no trace-file arguments (the shards are self-describing).
    std::string shard_set_root;
    if (!directory.empty()) shard_set_root = resolve_shard_set_root(directory);
    if (shard_set_root.empty() && !index_dir.empty())
        shard_set_root = resolve_shard_set_root(index_dir);
    if (shard_set_root.empty() && directory.empty() &&
        cli->files_args.value.size() == 1 &&
        fs::is_directory(cli->files_args.value[0]))
        shard_set_root = resolve_shard_set_root(cli->files_args.value[0]);

    if (!shard_set_root.empty() && !aggregate && !counters) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s",
            "A shard set supports only aggregate/counter queries "
            "(--group-by/--agg/--counters).");
        co_return 1;
    }

    std::vector<std::string> files;
    if (!shard_set_root.empty()) {
        // Self-describing: ShardedView enumerates files from each shard index.
    } else if (!directory.empty()) {
        if (!fs::exists(directory)) {
            DFTRACER_UTILS_LOG_ERROR("Directory does not exist: %s",
                                     directory.c_str());
            co_return 1;
        }

        PatternDirectoryScannerUtility scanner;
        PatternDirectoryScannerUtilityInput scan_input{
            directory, {".pfw.gz"}, false};
        auto matched = co_await scanner(scan_input);

        for (const auto& entry : matched) {
            files.push_back(entry.path.string());
        }

        if (files.empty()) {
            DFTRACER_UTILS_LOG_ERROR("No .pfw.gz files found in: %s",
                                     directory.c_str());
            co_return 1;
        }
    } else {
        files = cli->files_args.value;

        if (files.empty()) {
            DFTRACER_UTILS_LOG_ERROR(
                "%s", "No files or directory specified. Use --help for usage.");
            co_return 1;
        }
    }

    // Normalize single-member inputs to bounded multi-member gzip (shared with
    // every ingest path); index and reader both use the returned split copies.
    std::vector<std::pair<std::string, std::string>> auto_split;
    if (!no_auto_index) {
        auto norm = co_await indexing::normalize_members_for_ingest(
            std::move(files), checkpoint_size);
        files = std::move(norm.files);
        auto_split = std::move(norm.split);
    }

    std::vector<std::string> files_needing_index;
    for (const auto& file_path : files) {
        std::string index_path =
            internal::determine_index_path(file_path, index_dir);
        if (!fs::exists(index_path)) {
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

    (void)stream_mode;  // events always stream to the sink now

    // Trace output is always compressed; ensure the .gz extension.
    std::string final_output = output_path;
    if (write_trace && !final_output.empty()) {
        const std::string gz = ".gz";
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
    FileHandle out_owner(out_file);  // closes out_file on any exit; not stdout
    FILE* out_target = out_file ? out_file : stdout;
    FileSink sink(out_target);
    bool verify_failed = false;

    // One View over every file; the executor plans and scans them in parallel.
    std::vector<ViewFile> view_files;
    view_files.reserve(files.size());
    for (const auto& file_path : files) {
        ViewFile vf;
        vf.file_path = file_path;
        vf.index_path = internal::determine_index_path(file_path, index_dir);
        vf.checkpoint_size = checkpoint_size;
        view_files.push_back(std::move(vf));
    }

    auto pipeline_config =
        cli::build_pipeline_config("DFTracer View", cli->pipeline);
    Pipeline pipeline(pipeline_config);

    ExportStats stats;
    // Apply the parsed view options to a base View; shared by the single-node
    // and distributed paths so both aggregate identically.
    auto configure = [&](View v) {
        if (view.query) v = v.filter(*view.query);
        v = v.phase(view_phase);
        v = v.metadata(view.include_metadata);
        if (time_range) v = v.time_range(time_range->first, time_range->second);
        if (cli->time_scale > 0) v = v.time_scale(cli->time_scale);
        if (time_bucket > 0) v = v.time_bucket(time_bucket);
        if (cli->occ_cell > 0) v = v.occ_cell(cli->occ_cell);
        if (!group_keys.empty()) v = v.group_by(group_keys);
        if (!agg_specs.empty()) v = v.agg(agg_specs);
        if (cli->agg_numeric_args) v = v.agg_numeric_args();
        if (!cli->select.empty()) v = v.select(cli::split_csv(cli->select));
        if (cli->no_spill)
            v = v.memory_budget(NO_SPILL_BUDGET);
        else if (cli->memory_budget > 0)
            v = v.memory_budget(cli->memory_budget);
        // else: leave the plan default (0), which resolves to auto (~1/3 RAM).
        if (cli->offset > 0) v = v.offset(cli->offset);
        if (cli->limit > 0) v = v.limit(cli->limit);
        return v;
    };
    auto combined_task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
#ifdef DFTRACER_UTILS_ENABLE_MPI
            distributed::MpiTransport transport;
            // Distributed aggregation: each rank aggregates its shard into a
            // serialized partial, the partials are gathered, and rank 0 merges
            // and emits (counters -> ph="C" events; aggregate -> collect
            // table). Only these reductions distribute; other modes fall
            // through to rank 0 (single-node) below.
            if ((counters || aggregate || fg_mode) && transport.size() > 1 &&
                shard_set_root.empty()) {
                auto shard =
                    shard_files(files, transport.rank(), transport.size());
                // Default: nest the shard set under a hidden dir so it does not
                // clutter (or get scanned from) the trace directory. An
                // explicit
                // --index-dir is used verbatim.
                // Not a conditional expression: GCC 12 frees its temporaries
                // twice in a coroutine.
                std::string idx_base = index_dir;
                if (idx_base.empty())
                    idx_base = (fs::path(files.front()).parent_path() /
                                SHARD_SET_DIRNAME)
                                   .string();
                std::string rank_idx =
                    idx_base + "/rank_" + std::to_string(transport.rank());
                if (!no_auto_index) {
                    if (counters || fg_mode) {
                        // Event-only scans (counter/flamegraph partials) need
                        // only the base index, not the aggregation tier.
                        co_await indexing::ensure_indexes_fresh(&ctx, "", shard,
                                                                rank_idx);
                    } else {
                        // Build the aggregation tier per rank so the partial -
                        // and any later read of the auto-written shard set - is
                        // answered from the index instead of re-scanning
                        // traces.
                        indexing::ResolveAndBuildInput bin;
                        bin.files = shard;
                        bin.index_dir = rank_idx;
                        bin.checkpoint_size = checkpoint_size;
                        bin.require_checkpoints = true;
                        bin.require_aggregation = true;
                        bin.aggregation_config =
                            aggregators::AggregationConfig{};
                        co_await indexing::resolve_and_build_index(
                            &ctx, std::move(bin));
                    }
                }
                std::vector<ViewFile> shard_files_vf;
                for (const auto& f : shard) {
                    ViewFile vf;
                    vf.file_path = f;
                    vf.index_path = internal::determine_index_path(f, rank_idx);
                    vf.checkpoint_size = checkpoint_size;
                    shard_files_vf.push_back(std::move(vf));
                }
                View shard_view =
                    configure(View::from_files(std::move(shard_files_vf)));
                std::string partial;
                if (fg_mode)
                    partial =
                        co_await shard_view.flamegraph_partial(ct_partition);
                else
                    partial = co_await shard_view.aggregate_partial();
                auto partials = transport.all_gather(partial);
                if (transport.rank() == 0) {
                    std::vector<std::string_view> pv(partials.begin(),
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
                        dataframe::DataFrame table =
                            merger.merge_partials_to_table(pv);
                        print_table(out_target, table);
                        stats.events_matched = table.num_rows();
                    }

                    // Catalog the per-rank shards so a later query autodetects
                    // and reads the set without rebuilding. Best-effort: a
                    // write failure never fails the query.
                    if (!no_auto_index) {
                        std::vector<std::string> shard_dirs;
                        for (int r = 0; r < transport.size(); ++r) {
                            std::string d = internal::determine_index_path(
                                "x", idx_base + "/rank_" + std::to_string(r));
                            if (fs::exists(d))
                                shard_dirs.push_back(std::move(d));
                        }
                        try {
                            if (!shard_dirs.empty())
                                write_shard_set(idx_base, shard_dirs);
                        } catch (const std::exception& e) {
                            DFTRACER_UTILS_LOG_WARN(
                                "shard-set manifest not written: %s", e.what());
                        }
                    }
                }
                co_return;
            }
            // Distributed: each rank writes its shard, rank 0 concatenates.
            if (write_trace && transport.size() > 1) {
                auto shard =
                    shard_files(files, transport.rank(), transport.size());
                std::string idx_base = index_dir;
                if (idx_base.empty())
                    idx_base = fs::path(files.front()).parent_path().string();
                std::string rank_idx =
                    idx_base + "/rank_" + std::to_string(transport.rank());
                if (!no_auto_index) {
                    co_await indexing::ensure_indexes_fresh(&ctx, "", shard,
                                                            rank_idx);
                }
                std::vector<ViewFile> shard_vf;
                for (const auto& f : shard) {
                    ViewFile vf;
                    vf.file_path = f;
                    vf.index_path = internal::determine_index_path(f, rank_idx);
                    vf.checkpoint_size = checkpoint_size;
                    shard_vf.push_back(std::move(vf));
                }
                const std::string rank_out =
                    final_output + ".rank_" + std::to_string(transport.rank());
                TraceWriteOptions topts;
                topts.output_path = rank_out;
                topts.member_size = member_size;
                topts.num_workers = cli->pipeline.executor_threads;
                topts.compress = true;
                View shard_view =
                    configure(View::from_files(std::move(shard_vf)));
                stats = co_await shard_view.export_trace(topts);
                auto rank_outs = transport.all_gather(rank_out);
                if (transport.rank() == 0) {
                    std::vector<std::string> parts(rank_outs.begin(),
                                                   rank_outs.end());
                    if (co_await fileio::parallel::merge_shards(final_output,
                                                                parts) != 0) {
                        DFTRACER_UTILS_LOG_ERROR("merge_shards failed for %s",
                                                 final_output.c_str());
                    } else if (!no_index) {
                        co_await indexing::ensure_index_fresh(
                            &ctx, "", final_output, index_dir);
                    }
                }
                co_return;
            }
            // Non-distributed modes run on rank 0 only (correct, not sharded).
            if (transport.rank() != 0) co_return;
#endif
            // Shard set: merge the query across the immutable shards read-only,
            // no trace-file scan when each shard's aggregation tier covers it.
            if (!shard_set_root.empty()) {
                ShardedView sv = ShardedView::from_manifest(shard_set_root);
                if (counters) {
                    stats = co_await sv.aggregate_counters(configure, sink);
                } else {
                    dataframe::DataFrame table =
                        co_await sv.aggregate(configure);
                    print_table(out_target, table);
                    stats.events_matched = table.num_rows();
                }
                co_return;
            }

            View v = configure(View::from_files(view_files));

            // Skip the eager pre-build when the query would take the raw-gzip
            // bootstrap (answer the query and build the index in one pass);
            // pre-building first would make the index exist so the bootstrap
            // never fires. Only the plain export / collect paths bootstrap.
            const bool will_bootstrap =
                (aggregate && v.collect_would_bootstrap()) ||
                (!counters && !aggregate && !write_trace &&
                 v.export_would_bootstrap());
            if (!no_auto_index && !will_bootstrap) {
                co_await indexing::ensure_indexes_fresh(&ctx, "", files,
                                                        index_dir);
            }

            if (ct_mode || fg_mode) {
                dataframe::DataFrame table;
                if (fg_mode)
                    table = co_await v.flamegraph(ct_partition);
                else
                    table = co_await v.call_tree(ct_partition);
                print_table(out_target, table);
                stats.events_matched = table.num_rows();
            } else if (mv_mode) {
                // Build-only terminal: compute the aggregation and persist it
                // as a rollup so a later matching query hits the fast path.
                stats = co_await v.materialize().run();
                std::fprintf(stderr, "Materialized rollup (%llu events).\n",
                             (unsigned long long)stats.events_matched);
            } else if (typed_mode) {
                auto tr = co_await v.collect_typed();
                emit_typed(out_target, tr.regular, tr.aggregated, tr.counters);
                stats.events_matched = static_cast<std::uint64_t>(
                    tr.regular.num_rows() + tr.aggregated.num_rows() +
                    tr.counters.num_rows());
            } else if (counters) {
                stats = co_await v.export_counters(sink);
            } else if (aggregate) {
                dataframe::DataFrame table = co_await collect_batch(v);
                print_table(out_target, table);
                stats.events_matched = table.num_rows();
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
                stats = co_await v.export_trace(topts);
            } else {
                stats = co_await v.export_json(sink);
            }

            if (out_file) std::fflush(out_file);

            if (verify && write_trace) {
                co_await verify_output(ctx, final_output, index_dir,
                                       checkpoint_size, configure,
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

    if (!auto_split.empty()) {
        std::fprintf(
            stderr,
            "\n"
            "============================================================\n"
            "WARNING: %zu single-member trace(s) were auto-split into a\n"
            "sibling split/ directory so every checkpoint is a real gzip\n"
            "member. Your original files were NOT modified; the split\n"
            "copies were indexed and read:\n",
            auto_split.size());
        for (const auto& [src, dst] : auto_split)
            std::fprintf(stderr, "  %s\n    -> %s\n", src.c_str(), dst.c_str());
        std::fprintf(
            stderr,
            "Re-run against the split/ copies (or `dftracer_split` output) to\n"
            "skip this step next time.\n"
            "============================================================\n");
    }

    co_return verify_failed ? 1 : 0;
}

int main(int argc, char** argv) {
#ifdef DFTRACER_UTILS_ENABLE_MPI
    MPI_Init(&argc, &argv);
    // Local ranks share the node's cores (Dask-style) before any runtime work.
    distributed::set_local_thread_budget();
#endif
    int rc = cli::cli_main<ViewArgParse>(
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
