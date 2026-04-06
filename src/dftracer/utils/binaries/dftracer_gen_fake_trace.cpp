#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_indexer_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_pruner_utility.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_compressor_utility.h>
#include <dftracer/utils/utilities/fileio/streaming_file_writer_utility.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>

#include <argparse/argparse.hpp>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::indexing;
namespace compression = dftracer::utils::utilities::compression;
namespace util_io = dftracer::utils::utilities::fileio;
using dftracer::utils::utilities::indexer::IndexBuildConfig;
using dftracer::utils::utilities::indexer::IndexBuilderUtility;
using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::internal::get_logical_path;

// ---------------------------------------------------------------------------
// TraceWriter – compresses via ManualStreamingCompressorUtility and writes
//               via StreamingFileWriterUtility.  Natural deflate blocks
//               provide block boundaries for the gzip indexer.
// ---------------------------------------------------------------------------
class TraceWriter {
   public:
    explicit TraceWriter(const std::string& path) : writer_(path) {}

    ~TraceWriter() { close(); }

    TraceWriter(const TraceWriter&) = delete;
    TraceWriter& operator=(const TraceWriter&) = delete;

    void write(const std::string& s) {
        [this, &s]() -> coro::CoroTask<void> {
            auto gen = compressor_.compress(ByteView(s));
            while (auto chunk = co_await gen.next()) {
                co_await writer_.process(*chunk);
            }
        }()
                            .get();
    }

    void close() {
        [this]() -> coro::CoroTask<void> {
            auto gen = compressor_.finalize_stream();
            while (auto chunk = co_await gen.next()) {
                co_await writer_.process(*chunk);
            }
        }()
                        .get();
        writer_.close();
    }

   private:
    compression::zlib::ManualStreamingCompressorUtility compressor_;
    util_io::StreamingFileWriterUtility writer_;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Deterministic 16-char hex hash using the project's HasherUtility
static std::string make_hash(const std::string& name) {
    hash::HasherUtility hasher;
    hasher.reset();
    auto h = hasher.process(name).get();
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(h.value));
    return std::string(buf);
}

// +/-30% random variance around base
static std::uint64_t jitter(std::mt19937_64& rng, std::uint64_t base) {
    std::uniform_real_distribution<double> dist(0.7, 1.3);
    return static_cast<std::uint64_t>(static_cast<double>(base) * dist(rng));
}

// Escape a JSON string value (no surrounding quotes)
static void json_escape(std::string& out, const std::string& s) {
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            default:
                out += c;
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Event serialisers – build one JSON line and write it
// ---------------------------------------------------------------------------

// Metadata event: {"name":"HH"/"FH"/"SH","ph":"M",
//                  "args":{"hhash":"...","name":"...","value":"..."}}
static void emit_metadata(TraceWriter& w, const std::string& kind,
                          const std::string& hhash,
                          const std::string& resolved_name,
                          const std::string& hash_value) {
    std::string buf;
    buf.reserve(256);
    buf += R"({"name":")";
    buf += kind;
    buf += R"(","ph":"M","args":{"hhash":")";
    json_escape(buf, hhash);
    buf += R"(","name":")";
    json_escape(buf, resolved_name);
    buf += R"(","value":")";
    json_escape(buf, hash_value);
    buf += R"("}})";
    buf += '\n';
    w.write(buf);
}

// Regular event (duration, ph=X)
struct EventArgs {
    std::uint64_t id = 0;
    std::uint64_t pid = 0;
    std::uint64_t tid = 0;
    std::string name;
    std::string cat;
    std::uint64_t ts = 0;
    std::uint64_t dur = 0;
    int level = 0;
    std::string hhash;
    std::string fhash;
    std::string cmd_hash;
    // Optional extra args appended verbatim (no leading comma)
    std::string extra;
};

static void emit_event(TraceWriter& w, const EventArgs& a) {
    char num_buf[128];
    std::string buf;
    buf.reserve(512);

    buf += R"({"id":)";
    std::snprintf(num_buf, sizeof(num_buf), "%llu",
                  static_cast<unsigned long long>(a.id));
    buf += num_buf;

    buf += R"(,"name":")";
    json_escape(buf, a.name);
    buf += R"(","cat":")";
    json_escape(buf, a.cat);

    std::snprintf(num_buf, sizeof(num_buf),
                  R"(","pid":%llu,"tid":%llu,"ts":%llu,"dur":%llu,"ph":"X")",
                  static_cast<unsigned long long>(a.pid),
                  static_cast<unsigned long long>(a.tid),
                  static_cast<unsigned long long>(a.ts),
                  static_cast<unsigned long long>(a.dur));
    buf += num_buf;

    buf += R"(,"args":{)";

    buf += R"("hhash":")";
    json_escape(buf, a.hhash);
    buf += '"';

    std::snprintf(num_buf, sizeof(num_buf), R"(,"level":%d)", a.level);
    buf += num_buf;

    if (!a.fhash.empty()) {
        buf += R"(,"fhash":")";
        json_escape(buf, a.fhash);
        buf += '"';
    }
    if (!a.cmd_hash.empty()) {
        buf += R"(,"cmd_hash":")";
        json_escape(buf, a.cmd_hash);
        buf += '"';
    }
    if (!a.extra.empty()) {
        buf += ',';
        buf += a.extra;
    }

    buf += R"(}})";
    buf += '\n';
    w.write(buf);
}

// ---------------------------------------------------------------------------
// Verify: build bloom indices and run queries to prove chunk-skipping works
// ---------------------------------------------------------------------------
struct QuerySpec {
    std::string label;
    std::unordered_map<std::string, std::vector<std::string>> predicates;
};

static coro::CoroTask<int> run_verify(
    const std::vector<std::string>& file_paths,
    const std::vector<QuerySpec>& queries, std::size_t ckpt_size) {
    // Extra dimensions: arbitrary dot-paths into args
    std::vector<std::string> extra_dims = {"ret", "count", "offset", "epoch",
                                           "step"};

    std::vector<std::string> all_dimensions = {"name",  "cat",   "pid", "tid",
                                               "hhash", "fhash", "sref"};
    for (const auto& dim : extra_dims) {
        all_dimensions.push_back(dim);
    }

    ChunkIndexerConfig indexer_config;
    indexer_config.expected_entries_per_chunk = 1024;
    indexer_config.false_positive_rate = 0.01;
    indexer_config.extra_dimensions = extra_dims;

    std::printf("\n==========================================\n");
    std::printf("Verify: building bloom indices\n");
    std::printf("==========================================\n");

    for (const auto& file_path : file_paths) {
        std::string abs_path = fs::absolute(file_path).string();

        // 1. Build gzip index
        std::string index_path = internal::determine_index_path(abs_path, "");
        auto idx_input = IndexBuildConfig::for_file(abs_path)
                             .with_checkpoint_size(ckpt_size)
                             .with_force_rebuild(true);
        co_await IndexBuilderUtility{}.process(idx_input);

        // 2. Collect metadata
        auto meta_input = MetadataCollectorUtilityInput::from_file(abs_path)
                              .with_checkpoint_size(ckpt_size)
                              .with_force_rebuild(false)
                              .with_index(index_path);
        auto metadata = co_await MetadataCollectorUtility{}.process(meta_input);

        if (!metadata.success) {
            std::fprintf(stderr, "  WARN: metadata failed for %s\n",
                         abs_path.c_str());
            continue;
        }

        // 3. Index chunks and write to .idx
        try {
            std::string idx_path_bidx =
                internal::determine_index_path(abs_path, "");
            IndexDatabase idx_db(idx_path_bidx);
            idx_db.init_base_schema();
            idx_db.init_bloom_schema();

            std::uint64_t file_hash_val = 0;
            if (fs::exists(abs_path)) {
                file_hash_val =
                    static_cast<std::uint64_t>(fs::file_size(abs_path));
            }
            int fid = idx_db.get_or_create_file_info(get_logical_path(abs_path),
                                                     file_hash_val);

            std::size_t file_size = metadata.uncompressed_size;
            std::size_t num_ckpts = metadata.num_checkpoints;

            struct ChunkWork {
                std::uint64_t idx;
                std::size_t start;
                std::size_t end;
            };
            std::vector<ChunkWork> chunks;

            if (num_ckpts == 0) {
                chunks.push_back({0, 0, file_size});
            } else {
                std::size_t bytes_per = file_size / num_ckpts;
                for (std::size_t i = 0; i < num_ckpts; ++i) {
                    std::size_t start = i * bytes_per;
                    std::size_t end =
                        (i + 1 == num_ckpts) ? file_size : (i + 1) * bytes_per;
                    chunks.push_back(
                        {static_cast<std::uint64_t>(i), start, end});
                }
            }

            idx_db.begin_transaction();
            std::unordered_map<std::string, BloomFilter> file_blooms;
            HashResolutions all_hr;
            std::size_t total_events = 0;

            for (const auto& chunk : chunks) {
                ChunkIndexerInput ci;
                ci.with_file_path(abs_path)
                    .with_index_path(index_path)
                    .with_checkpoint_size(ckpt_size)
                    .with_checkpoint_idx(chunk.idx)
                    .with_byte_range(chunk.start, chunk.end)
                    .with_config(indexer_config)
                    .with_batch_size(4 * 1024 * 1024);

                ChunkIndexerUtility idx_util;
                auto output = co_await idx_util.process(ci);
                total_events += output.events_processed;

                for (auto& [dim, bloom] : output.bloom_filters) {
                    auto blob = bloom.serialize();
                    idx_db.insert_chunk_bloom_filter(
                        fid, output.checkpoint_idx, dim, blob.data(),
                        static_cast<int>(blob.size()), bloom.num_entries());

                    auto it = file_blooms.find(dim);
                    if (it == file_blooms.end()) {
                        file_blooms.emplace(dim, std::move(bloom));
                    } else {
                        it->second.merge_from(bloom);
                    }
                }

                idx_db.insert_chunk_statistics(fid, output.checkpoint_idx,
                                               output.statistics);

                for (auto& [dim, resolutions] : output.hash_resolutions) {
                    for (auto& [h, resolved] : resolutions) {
                        all_hr[dim][h] = resolved;
                    }
                }
            }

            for (auto& [dim, bloom] : file_blooms) {
                auto blob = bloom.serialize();
                idx_db.insert_file_bloom_filter(fid, dim, blob.data(),
                                                static_cast<int>(blob.size()),
                                                bloom.num_entries());
            }
            for (const auto& [dim, resolutions] : all_hr) {
                for (const auto& [h, resolved] : resolutions) {
                    idx_db.insert_hash_resolution(fid, dim, h, resolved);
                }
            }
            for (const auto& dim : all_dimensions) {
                idx_db.insert_index_dimension(fid, dim);
            }

            idx_db.commit_transaction();

            std::string basename = fs::path(abs_path).filename().string();
            std::printf("  %s: indexed (%zu events, %zu chunks)\n",
                        basename.c_str(), total_events, chunks.size());
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                         "  WARN: bloom index generation failed for %s: %s\n",
                         abs_path.c_str(), e.what());
        }
    }

    // Run queries
    std::printf("\n==========================================\n");
    std::printf("Verify: bloom filter query results\n");
    std::printf("==========================================\n");
    std::printf("  %-40s  %s  %s\n", "Query", "Files matched",
                "Chunks skipped");
    std::printf("  %-40s  %s  %s\n", "----------------------------------------",
                "-------------", "--------------");

    for (const auto& q : queries) {
        std::size_t files_matched = 0;
        std::size_t total_chunks = 0;
        std::size_t chunks_matched = 0;

        for (const auto& file_path : file_paths) {
            std::string abs_path = fs::absolute(file_path).string();
            std::string idx_path_q =
                internal::determine_index_path(abs_path, "");

            try {
                // Convert predicate map to query DSL string
                std::string query_dsl;
                for (const auto& [dim, vals] : q.predicates) {
                    if (!query_dsl.empty()) query_dsl += " and ";
                    if (vals.size() == 1) {
                        query_dsl += dim + " == \"" + vals[0] + "\"";
                    } else {
                        query_dsl += dim + " in [";
                        for (std::size_t vi = 0; vi < vals.size(); ++vi) {
                            if (vi > 0) query_dsl += ", ";
                            query_dsl += "\"" + vals[vi] + "\"";
                        }
                        query_dsl += "]";
                    }
                }

                auto parsed = common::query::Query::from_string(query_dsl);
                if (!parsed) continue;

                ChunkPrunerInput pruner_input{idx_path_q, abs_path,
                                              std::move(*parsed), nullptr};
                ChunkPrunerUtility pruner;
                auto result = co_await pruner.process(pruner_input);

                total_chunks += result.total_checkpoints;
                if (result.file_may_match) {
                    files_matched++;
                    chunks_matched += result.candidate_checkpoints.size();
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr, "  WARN: bloom query failed for %s: %s\n",
                             abs_path.c_str(), e.what());
            }
        }

        std::size_t chunks_skipped =
            total_chunks > 0 ? total_chunks - chunks_matched : 0;
        std::printf("  %-40s  %zu/%zu          %zu/%zu\n", q.label.c_str(),
                    files_matched, file_paths.size(), chunks_skipped,
                    total_chunks);
    }

    std::printf("==========================================\n");
    co_return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_gen_fake_trace",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Generate realistic DFTracer traces modeled after UNet3D training. "
        "Produces per-rank .pfw.gz files with known patterns "
        "suitable for testing bloom-filter indexing.");

    program.add_argument("-o", "--output-dir")
        .help("Output directory for trace files")
        .required();

    program.add_argument("-p", "--num-processes")
        .help("Number of ranks")
        .scan<'d', int>()
        .default_value(8);

    program.add_argument("-H", "--num-hosts")
        .help("Number of hosts")
        .scan<'d', int>()
        .default_value(4);

    program.add_argument("-e", "--num-epochs")
        .help("Training epochs")
        .scan<'d', int>()
        .default_value(500);

    program.add_argument("-s", "--steps-per-epoch")
        .help("Steps per epoch")
        .scan<'d', int>()
        .default_value(1000);

    program.add_argument("--checkpoint-every")
        .help("Checkpoint every N epochs")
        .scan<'d', int>()
        .default_value(5);

    program.add_argument("--validation-every")
        .help("Validate every N epochs")
        .scan<'d', int>()
        .default_value(2);

    program.add_argument("--num-train-files")
        .help("Training data shards")
        .scan<'d', int>()
        .default_value(8);

    program.add_argument("--num-val-files")
        .help("Validation data shards")
        .scan<'d', int>()
        .default_value(2);

    program.add_argument("--step-duration-ms")
        .help("Base step duration in milliseconds")
        .scan<'d', int>()
        .default_value(100);

    program.add_argument("--seed")
        .help("Random seed for duration jitter")
        .scan<'d', std::uint64_t>()
        .default_value(static_cast<std::uint64_t>(42));

    program.add_argument("--verify")
        .help(
            "After generation, build bloom indices and run queries to "
            "verify chunk-skipping works")
        .flag();

    program.add_argument("--checkpoint-size")
        .help(
            "Gzip checkpoint size in bytes for indexing (default: 2 MB). "
            "Smaller values produce more chunks and better demonstrate "
            "chunk-level bloom filter skipping.")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(2 * 1024 * 1024));

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        std::fprintf(stderr, "Error: %s\n", err.what());
        std::fprintf(stderr, "%s\n", program.help().str().c_str());
        return 1;
    }

    const std::string output_dir = program.get<std::string>("--output-dir");
    const int num_ranks = program.get<int>("--num-processes");
    const int num_hosts = program.get<int>("--num-hosts");
    const int num_epochs = program.get<int>("--num-epochs");
    const int steps_per_epoch = program.get<int>("--steps-per-epoch");
    const int checkpoint_every = program.get<int>("--checkpoint-every");
    const int validation_every = program.get<int>("--validation-every");
    const int num_train_files = program.get<int>("--num-train-files");
    const int num_val_files = program.get<int>("--num-val-files");
    const int step_dur_ms = program.get<int>("--step-duration-ms");
    const std::uint64_t base_seed = program.get<std::uint64_t>("--seed");
    const bool verify = program.get<bool>("--verify");
    const std::size_t checkpoint_size =
        program.get<std::size_t>("--checkpoint-size");

    // Convert base step duration to microseconds
    const std::uint64_t step_dur_us =
        static_cast<std::uint64_t>(step_dur_ms) * 1000ULL;

    // Create output directory
    fs::create_directories(output_dir);

    // -----------------------------------------------------------------------
    // Pre-compute hashes using project HasherUtility
    // -----------------------------------------------------------------------
    std::vector<std::string> host_names(num_hosts);
    std::vector<std::string> host_hashes(num_hosts);
    for (int h = 0; h < num_hosts; ++h) {
        host_names[h] = "node-" + std::to_string(h);
        host_hashes[h] = make_hash(host_names[h]);
    }

    std::vector<std::string> train_file_names(num_train_files);
    std::vector<std::string> train_file_hashes(num_train_files);
    for (int f = 0; f < num_train_files; ++f) {
        train_file_names[f] = "/data/train/shard_" + std::to_string(f) + ".h5";
        train_file_hashes[f] = make_hash(train_file_names[f]);
    }

    std::vector<std::string> val_file_names(num_val_files);
    std::vector<std::string> val_file_hashes(num_val_files);
    for (int f = 0; f < num_val_files; ++f) {
        val_file_names[f] = "/data/val/val_" + std::to_string(f) + ".h5";
        val_file_hashes[f] = make_hash(val_file_names[f]);
    }

    const std::string ckpt_file_name = "/checkpoints/model_ckpt.pt";
    const std::string ckpt_file_hash = make_hash(ckpt_file_name);

    const std::string script_name = "python train_unet3d.py";
    const std::string script_hash = make_hash(script_name);

    // -----------------------------------------------------------------------
    // Banner
    // -----------------------------------------------------------------------
    std::printf("==========================================\n");
    std::printf("DFTracer Fake Trace Generator (UNet3D)\n");
    std::printf("==========================================\n");
    std::printf("  Ranks: %d   Hosts: %d\n", num_ranks, num_hosts);
    std::printf("  Epochs: %d   Steps/epoch: %d\n", num_epochs,
                steps_per_epoch);
    std::printf("  Checkpoint every: %d   Validation every: %d\n",
                checkpoint_every, validation_every);
    std::printf("  Train shards: %d   Val files: %d\n", num_train_files,
                num_val_files);
    std::printf("  Step duration: %d ms   Format: .pfw.gz\n", step_dur_ms);
    std::printf("  Seed: %llu   Verify: %s\n",
                static_cast<unsigned long long>(base_seed),
                verify ? "yes" : "no");
    std::printf("  Output: %s\n", output_dir.c_str());
    std::printf("==========================================\n\n");

    std::vector<std::string> generated_files(num_ranks);
    std::vector<std::size_t> rank_event_counts(num_ranks, 0);

    for (int rank = 0; rank < num_ranks; ++rank) {
        generated_files[rank] =
            output_dir + "/rank_" + std::to_string(rank) + ".pfw.gz";
    }

    // -----------------------------------------------------------------------
    // Generate one file per rank (parallel via pipeline)
    // -----------------------------------------------------------------------
    auto pipeline_config = PipelineConfig::default_config().with_name(
        "DFTracer Fake Trace Generator");
    Pipeline pipeline(pipeline_config);

    auto* generated_files_ptr = &generated_files;
    auto* host_hashes_ptr = &host_hashes;
    auto* host_names_ptr = &host_names;
    auto* train_file_names_ptr = &train_file_names;
    auto* train_file_hashes_ptr = &train_file_hashes;
    auto* val_file_names_ptr = &val_file_names;
    auto* val_file_hashes_ptr = &val_file_hashes;
    auto* ckpt_file_name_ptr = &ckpt_file_name;
    auto* ckref_ptr = &ckpt_file_hash;
    auto* script_name_ptr = &script_name;
    auto* sref_ptr = &script_hash;
    auto* rank_event_counts_ptr = &rank_event_counts;
    std::vector<std::shared_ptr<Task>> rank_tasks;
    for (int rank = 0; rank < num_ranks; ++rank) {
        auto task = make_task(
            [rank, base_seed, num_ranks, num_hosts, num_train_files,
             num_val_files, num_epochs, steps_per_epoch, checkpoint_every,
             validation_every, step_dur_us, generated_files_ptr,
             host_hashes_ptr, host_names_ptr, train_file_names_ptr,
             train_file_hashes_ptr, val_file_names_ptr, val_file_hashes_ptr,
             ckpt_file_name_ptr, ckref_ptr, script_name_ptr, sref_ptr,
             rank_event_counts_ptr]([[maybe_unused]] CoroScope& ctx)
                -> coro::CoroTask<std::size_t> {
                const std::string& path = (*generated_files_ptr)[rank];
                TraceWriter writer(path);
                const std::string& sref = *sref_ptr;
                const std::string& ckref = *ckref_ptr;

                std::mt19937_64 rng(
                    base_seed + static_cast<std::uint64_t>(rank) * 10000ULL);

                const int host_idx = rank % num_hosts;
                const std::string& my_hhash = (*host_hashes_ptr)[host_idx];
                const std::uint64_t pid =
                    1000 + static_cast<std::uint64_t>(rank);
                const std::uint64_t tid_main = pid * 10;
                const std::uint64_t tid_io = pid * 10 + 1;

                // Determine which train shards this rank reads (round-robin)
                std::vector<int> my_train_shards;
                for (int f = rank; f < num_train_files; f += num_ranks) {
                    my_train_shards.push_back(f);
                }
                if (my_train_shards.empty()) {
                    my_train_shards.push_back(rank % num_train_files);
                }

                // -------------------------------------------------------------------
                // Metadata header
                // -------------------------------------------------------------------
                emit_metadata(writer, "HH", my_hhash,
                              (*host_names_ptr)[host_idx], my_hhash);

                for (int si : my_train_shards) {
                    emit_metadata(writer, "FH", my_hhash,
                                  (*train_file_names_ptr)[si],
                                  (*train_file_hashes_ptr)[si]);
                }
                for (int vi = 0; vi < num_val_files; ++vi) {
                    emit_metadata(writer, "FH", my_hhash,
                                  (*val_file_names_ptr)[vi],
                                  (*val_file_hashes_ptr)[vi]);
                }
                emit_metadata(writer, "FH", my_hhash, *ckpt_file_name_ptr,
                              *ckref_ptr);
                emit_metadata(writer, "SH", my_hhash, *script_name_ptr,
                              *sref_ptr);

                std::size_t rank_events = 0;
                std::uint64_t next_id = 0;
                std::uint64_t ts = 1000000000ULL;  // 1 second in us

                // Helper: fill common fields and auto-assign id
                auto make_event =
                    [&](const std::string& name, const std::string& cat,
                        std::uint64_t event_tid, std::uint64_t event_ts,
                        std::uint64_t dur, int level) {
                        EventArgs a;
                        a.id = next_id++;
                        a.pid = pid;
                        a.tid = event_tid;
                        a.name = name;
                        a.cat = cat;
                        a.ts = event_ts;
                        a.dur = dur;
                        a.level = level;
                        a.hhash = my_hhash;
                        a.cmd_hash = sref;
                        return a;
                    };

                // -------------------------------------------------------------------
                // Per epoch
                // -------------------------------------------------------------------
                for (int epoch = 0; epoch < num_epochs; ++epoch) {
                    // Training steps
                    for (int step = 0; step < steps_per_epoch; ++step) {
                        char extra[256];
                        std::uint64_t step_start = ts;

                        // -- Data loading I/O (on io thread, level=2) --
                        std::uint64_t io_start = ts;
                        int shard_idx =
                            my_train_shards[step % static_cast<int>(
                                                       my_train_shards.size())];
                        const std::string& data_fhash =
                            (*train_file_hashes_ptr)[shard_idx];
                        std::uint64_t io_size = jitter(rng, 4096);

                        // open
                        {
                            auto a = make_event("open", "POSIX", tid_io, ts,
                                                jitter(rng, 5), 2);
                            a.fhash = data_fhash;
                            std::snprintf(extra, sizeof(extra), R"("ret":3)");
                            a.extra = extra;
                            emit_event(writer, a);
                            ts += a.dur;
                            ++rank_events;
                        }

                        // pread / read / fread (randomly 3-5 calls)
                        int num_reads = 3 + static_cast<int>(rng() % 3);
                        const char* read_ops[] = {"pread", "read", "fread"};
                        for (int r = 0; r < num_reads; ++r) {
                            auto a = make_event(read_ops[r % 3], "POSIX",
                                                tid_io, ts, jitter(rng, 20), 2);
                            a.fhash = data_fhash;
                            std::uint64_t offset =
                                static_cast<std::uint64_t>(r) * io_size;
                            std::snprintf(
                                extra, sizeof(extra),
                                R"("ret":%llu,"count":%llu,"offset":%llu)",
                                static_cast<unsigned long long>(io_size),
                                static_cast<unsigned long long>(io_size),
                                static_cast<unsigned long long>(offset));
                            a.extra = extra;
                            emit_event(writer, a);
                            ts += a.dur;
                            ++rank_events;
                        }

                        // close
                        {
                            auto a = make_event("close", "POSIX", tid_io, ts,
                                                jitter(rng, 3), 2);
                            a.fhash = data_fhash;
                            std::snprintf(extra, sizeof(extra), R"("ret":0)");
                            a.extra = extra;
                            emit_event(writer, a);
                            ts += a.dur;
                            ++rank_events;
                        }

                        // Emit data_loading wrapper (level=1, spans all I/O)
                        {
                            auto a = make_event("data_loading", "IO", tid_io,
                                                io_start, ts - io_start, 1);
                            std::snprintf(extra, sizeof(extra),
                                          R"("epoch":%d,"step":%d)", epoch,
                                          step);
                            a.extra = extra;
                            emit_event(writer, a);
                            ++rank_events;
                        }

                        // -- Forward pass (5 events on main thread, level=2) --
                        std::uint64_t fwd_start = ts;
                        const char* fwd_ops[] = {"conv3d", "batch_norm", "relu",
                                                 "max_pool", "upsample"};
                        for (int f = 0; f < 5; ++f) {
                            auto a =
                                make_event(fwd_ops[f], "APP", tid_main, ts,
                                           jitter(rng, step_dur_us / 5), 2);
                            std::snprintf(extra, sizeof(extra),
                                          R"("epoch":%d,"step":%d)", epoch,
                                          step);
                            a.extra = extra;
                            emit_event(writer, a);
                            ts += a.dur;
                            ++rank_events;
                        }

                        // Emit forward wrapper (level=1)
                        {
                            auto a = make_event("forward", "APP", tid_main,
                                                fwd_start, ts - fwd_start, 1);
                            std::snprintf(extra, sizeof(extra),
                                          R"("epoch":%d,"step":%d)", epoch,
                                          step);
                            a.extra = extra;
                            emit_event(writer, a);
                            ++rank_events;
                        }

                        // -- Loss + backward (3 events, level=2) --
                        std::uint64_t back_start = ts;
                        const char* back_ops[] = {"dice_loss", "backward",
                                                  "allreduce"};
                        for (int b = 0; b < 3; ++b) {
                            auto a =
                                make_event(back_ops[b], "APP", tid_main, ts,
                                           jitter(rng, step_dur_us / 4), 2);
                            std::snprintf(extra, sizeof(extra),
                                          R"("epoch":%d,"step":%d)", epoch,
                                          step);
                            a.extra = extra;
                            emit_event(writer, a);
                            ts += a.dur;
                            ++rank_events;
                        }

                        // Emit backward wrapper (level=1)
                        {
                            auto a =
                                make_event("backward_pass", "APP", tid_main,
                                           back_start, ts - back_start, 1);
                            std::snprintf(extra, sizeof(extra),
                                          R"("epoch":%d,"step":%d)", epoch,
                                          step);
                            a.extra = extra;
                            emit_event(writer, a);
                            ++rank_events;
                        }

                        // -- Optimizer step (1 event, level=1) --
                        {
                            auto a =
                                make_event("optimizer_step", "APP", tid_main,
                                           ts, jitter(rng, step_dur_us / 8), 1);
                            std::snprintf(extra, sizeof(extra),
                                          R"("epoch":%d,"step":%d)", epoch,
                                          step);
                            a.extra = extra;
                            emit_event(writer, a);
                            ts += a.dur;
                            ++rank_events;
                        }

                        // Emit train_step wrapper (level=0, spans
                        // entire step)
                        {
                            auto a = make_event("train_step", "APP", tid_main,
                                                step_start, ts - step_start, 0);
                            std::snprintf(extra, sizeof(extra),
                                          R"("epoch":%d,"step":%d)", epoch,
                                          step);
                            a.extra = extra;
                            emit_event(writer, a);
                            ++rank_events;
                        }
                    }

                    // Validation (every validation_every epochs)
                    if (validation_every > 0 &&
                        (epoch + 1) % validation_every == 0) {
                        std::uint64_t val_phase_start = ts;
                        const int val_steps = 10;
                        for (int vs = 0; vs < val_steps; ++vs) {
                            char extra[256];
                            int vf_idx = vs % num_val_files;
                            const std::string& vf_hash =
                                (*val_file_hashes_ptr)[vf_idx];
                            std::uint64_t vio_size = jitter(rng, 4096);
                            std::uint64_t val_step_start = ts;

                            // open (level=2)
                            {
                                auto a = make_event("open", "POSIX", tid_io, ts,
                                                    jitter(rng, 5), 2);
                                a.fhash = vf_hash;
                                std::snprintf(extra, sizeof(extra),
                                              R"("ret":4)");
                                a.extra = extra;
                                emit_event(writer, a);
                                ts += a.dur;
                                ++rank_events;
                            }

                            // read calls (2-3, level=2)
                            int num_reads = 2 + static_cast<int>(rng() % 2);
                            const char* read_ops[] = {"pread", "read", "fread"};
                            for (int r = 0; r < num_reads; ++r) {
                                auto a =
                                    make_event(read_ops[r % 3], "POSIX", tid_io,
                                               ts, jitter(rng, 20), 2);
                                a.fhash = vf_hash;
                                std::snprintf(
                                    extra, sizeof(extra),
                                    R"("ret":%llu,"count":%llu,"offset":%llu)",
                                    static_cast<unsigned long long>(vio_size),
                                    static_cast<unsigned long long>(vio_size),
                                    static_cast<unsigned long long>(
                                        static_cast<std::uint64_t>(r) *
                                        vio_size));
                                a.extra = extra;
                                emit_event(writer, a);
                                ts += a.dur;
                                ++rank_events;
                            }

                            // close (level=2)
                            {
                                auto a = make_event("close", "POSIX", tid_io,
                                                    ts, jitter(rng, 3), 2);
                                a.fhash = vf_hash;
                                std::snprintf(extra, sizeof(extra),
                                              R"("ret":0)");
                                a.extra = extra;
                                emit_event(writer, a);
                                ts += a.dur;
                                ++rank_events;
                            }

                            // val_forward (level=2)
                            {
                                auto a = make_event(
                                    "val_forward", "APP", tid_main, ts,
                                    jitter(rng, step_dur_us / 3), 2);
                                std::snprintf(extra, sizeof(extra),
                                              R"("epoch":%d,"step":%d)", epoch,
                                              vs);
                                a.extra = extra;
                                emit_event(writer, a);
                                ts += a.dur;
                                ++rank_events;
                            }

                            // val_loss (level=2)
                            {
                                auto a =
                                    make_event("val_loss", "APP", tid_main, ts,
                                               jitter(rng, step_dur_us / 6), 2);
                                std::snprintf(extra, sizeof(extra),
                                              R"("epoch":%d,"step":%d)", epoch,
                                              vs);
                                a.extra = extra;
                                emit_event(writer, a);
                                ts += a.dur;
                                ++rank_events;
                            }

                            // val_step wrapper (level=1)
                            {
                                auto a = make_event("val_step", "APP", tid_main,
                                                    val_step_start,
                                                    ts - val_step_start, 1);
                                std::snprintf(extra, sizeof(extra),
                                              R"("epoch":%d,"step":%d)", epoch,
                                              vs);
                                a.extra = extra;
                                emit_event(writer, a);
                                ++rank_events;
                            }
                        }

                        // validation wrapper (level=0)
                        {
                            char extra[256];
                            auto a = make_event("validation", "APP", tid_main,
                                                val_phase_start,
                                                ts - val_phase_start, 0);
                            std::snprintf(extra, sizeof(extra), R"("epoch":%d)",
                                          epoch);
                            a.extra = extra;
                            emit_event(writer, a);
                            ++rank_events;
                        }
                    }

                    // Checkpoint (every checkpoint_every epochs)
                    if (checkpoint_every > 0 &&
                        (epoch + 1) % checkpoint_every == 0) {
                        char extra[256];
                        std::uint64_t ckpt_start = ts;

                        // open checkpoint file (level=1)
                        {
                            auto a = make_event("open", "POSIX", tid_io, ts,
                                                jitter(rng, 10), 1);
                            a.fhash = ckref;
                            std::snprintf(extra, sizeof(extra), R"("ret":5)");
                            a.extra = extra;
                            emit_event(writer, a);
                            ts += a.dur;
                            ++rank_events;
                        }

                        // pwrite calls (10-20, level=1)
                        int num_writes = 10 + static_cast<int>(rng() % 11);
                        for (int wr = 0; wr < num_writes; ++wr) {
                            auto a = make_event(
                                (wr % 2 == 0) ? "pwrite" : "write", "POSIX",
                                tid_io, ts, jitter(rng, 50), 1);
                            a.fhash = ckref;
                            std::uint64_t wr_size =
                                jitter(rng, 1048576);  // ~1 MB
                            std::snprintf(
                                extra, sizeof(extra),
                                R"("ret":%llu,"count":%llu,"offset":%llu)",
                                static_cast<unsigned long long>(wr_size),
                                static_cast<unsigned long long>(wr_size),
                                static_cast<unsigned long long>(
                                    static_cast<std::uint64_t>(wr) * wr_size));
                            a.extra = extra;
                            emit_event(writer, a);
                            ts += a.dur;
                            ++rank_events;
                        }

                        // fsync (level=1)
                        {
                            auto a = make_event("fsync", "POSIX", tid_io, ts,
                                                jitter(rng, 200), 1);
                            a.fhash = ckref;
                            std::snprintf(extra, sizeof(extra), R"("ret":0)");
                            a.extra = extra;
                            emit_event(writer, a);
                            ts += a.dur;
                            ++rank_events;
                        }

                        // close (level=1)
                        {
                            auto a = make_event("close", "POSIX", tid_io, ts,
                                                jitter(rng, 3), 1);
                            a.fhash = ckref;
                            std::snprintf(extra, sizeof(extra), R"("ret":0)");
                            a.extra = extra;
                            emit_event(writer, a);
                            ts += a.dur;
                            ++rank_events;
                        }

                        // checkpoint wrapper (level=0)
                        {
                            auto a = make_event("checkpoint", "IO", tid_io,
                                                ckpt_start, ts - ckpt_start, 0);
                            std::snprintf(extra, sizeof(extra), R"("epoch":%d)",
                                          epoch);
                            a.extra = extra;
                            emit_event(writer, a);
                            ++rank_events;
                        }
                    }
                }

                writer.close();
                (*rank_event_counts_ptr)[rank] = rank_events;
                co_return rank_events;
            },
            "Rank-" + std::to_string(rank));
        rank_tasks.push_back(task);
    }

    pipeline.set_source(rank_tasks);
    pipeline.execute();

    std::size_t total_events = 0;
    for (int rank = 0; rank < num_ranks; ++rank) {
        std::printf("  rank %d: %zu events -> %s\n", rank,
                    rank_event_counts[rank], generated_files[rank].c_str());
        total_events += rank_event_counts[rank];
    }

    // -----------------------------------------------------------------------
    // Summary banner
    // -----------------------------------------------------------------------
    std::printf("\n==========================================\n");
    std::printf("Generation complete\n");
    std::printf("==========================================\n");
    std::printf("  Total events: %zu\n", total_events);
    std::printf("  Total files:  %d\n", num_ranks);
    std::printf("\nInteresting queries for bloom filter testing:\n");
    std::printf(
        "  1. name=pwrite                   (checkpoint I/O, ~%d%% of "
        "epochs)\n",
        checkpoint_every > 0 ? 100 / checkpoint_every : 0);
    std::printf("  2. fhash=%s  (validation data, ~%d%% of epochs)\n",
                val_file_hashes[0].c_str(),
                validation_every > 0 ? 100 / validation_every : 0);
    if (!train_file_hashes.empty()) {
        std::printf("  3. fhash=%s  (rank-specific train shard)\n",
                    train_file_hashes[0].c_str());
    }
    std::printf("  4. hhash=%s  (host-specific, %s)\n", host_hashes[0].c_str(),
                host_names[0].c_str());
    std::printf("  5. name=allreduce                (every step, dense)\n");
    std::printf(
        "  6. name=fsync                    (checkpoint only, sparse)\n");
    std::printf("==========================================\n");

    // -----------------------------------------------------------------------
    // Verify mode: build bloom indices and run queries
    // -----------------------------------------------------------------------
    if (verify) {
        std::vector<QuerySpec> test_queries;

        // --- Single-dimension queries ---

        // name dimension
        test_queries.push_back(
            {"name=pwrite (sparse, ckpt only)", {{"name", {"pwrite"}}}});
        test_queries.push_back(
            {"name=allreduce (dense, every step)", {{"name", {"allreduce"}}}});
        test_queries.push_back(
            {"name=fsync (sparse, ckpt only)", {{"name", {"fsync"}}}});
        test_queries.push_back(
            {"name=val_forward (periodic)", {{"name", {"val_forward"}}}});

        // cat dimension
        test_queries.push_back(
            {"cat=POSIX (all I/O events)", {{"cat", {"POSIX"}}}});
        test_queries.push_back(
            {"cat=APP (all compute events)", {{"cat", {"APP"}}}});

        // pid dimension (rank-specific)
        std::string pid0 = std::to_string(1000);
        test_queries.push_back(
            {"pid=" + pid0 + " (rank 0 only)", {{"pid", {pid0}}}});

        // tid dimension (io thread vs main thread)
        std::string tid_io_0 = std::to_string(10001);
        test_queries.push_back(
            {"tid=" + tid_io_0 + " (rank 0 io thread)", {{"tid", {tid_io_0}}}});

        // fhash dimension (resolved file names)
        test_queries.push_back({"fhash=" + val_file_names[0] + " (resolved)",
                                {{"fhash", {val_file_hashes[0]}}}});
        if (!train_file_hashes.empty()) {
            test_queries.push_back(
                {"fhash=" + train_file_names[0] + " (resolved)",
                 {{"fhash", {train_file_hashes[0]}}}});
        }
        test_queries.push_back(
            {"fhash=ckpt (resolved)", {{"fhash", {ckpt_file_hash}}}});

        // hhash dimension (host-specific)
        test_queries.push_back({"hhash=" + host_names[0] + " (resolved)",
                                {{"hhash", {host_hashes[0]}}}});

        // sref dimension (script hash)
        test_queries.push_back(
            {"shash=train_unet3d (resolved)", {{"shash", {script_hash}}}});

        // --- Multi-dimension AND queries ---

        // name AND cat (checkpoint writes that are POSIX I/O)
        test_queries.push_back({"name=pwrite AND cat=POSIX",
                                {{"name", {"pwrite"}}, {"cat", {"POSIX"}}}});

        // name AND fhash (fsync on checkpoint file only)
        test_queries.push_back(
            {"name=fsync AND fhash=ckpt",
             {{"name", {"fsync"}}, {"fhash", {ckpt_file_hash}}}});

        // cat AND hhash (POSIX I/O on node-0)
        test_queries.push_back(
            {"cat=POSIX AND hhash=" + host_names[0],
             {{"cat", {"POSIX"}}, {"hhash", {host_hashes[0]}}}});

        // cat AND pid (APP events for rank 0)
        test_queries.push_back(
            {"cat=APP AND pid=" + pid0, {{"cat", {"APP"}}, {"pid", {pid0}}}});

        // name AND hhash AND fhash (read on node-0 for train shard 0)
        if (!train_file_hashes.empty()) {
            test_queries.push_back(
                {"name=read AND hhash=" + host_names[0] + " AND fhash=shard_0",
                 {{"name", {"read"}},
                  {"hhash", {host_hashes[0]}},
                  {"fhash", {train_file_hashes[0]}}}});
        }

        // --- OR-within dimension queries ---

        // name = pwrite OR write (all checkpoint write ops)
        test_queries.push_back({"name=pwrite|write (ckpt writes)",
                                {{"name", {"pwrite", "write"}}}});

        // name = open OR close (all open/close ops)
        test_queries.push_back({"name=open|close (all open/close)",
                                {{"name", {"open", "close"}}}});

        // fhash = any val file (all validation I/O)
        test_queries.push_back(
            {"fhash=any val file (OR)",
             {{"fhash", std::vector<std::string>(val_file_hashes.begin(),
                                                 val_file_hashes.end())}}});

        // --- Negative tests ---
        test_queries.push_back(
            {"name=NONEXISTENT (expect 0)", {{"name", {"NONEXISTENT"}}}});
        test_queries.push_back(
            {"cat=NONEXISTENT (expect 0)", {{"cat", {"NONEXISTENT"}}}});
        test_queries.push_back({"name=pwrite AND cat=APP (impossible)",
                                {{"name", {"pwrite"}}, {"cat", {"APP"}}}});

        return run_verify(generated_files, test_queries, checkpoint_size).get();
    }

    return 0;
}
