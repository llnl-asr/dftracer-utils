#include <dftracer/utils/utilities/composites/dft/dfanalyzer/dfanalyzer_scan.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/core/common/hash_combine.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/rocksdb/db_manager.h>
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/system_metrics_serialization.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::dfanalyzer {

using namespace dftracer::utils::utilities::composites::dft::aggregators;
using common::arrow::ColumnSpec;
using common::arrow::ColumnType;
using common::arrow::RecordBatchBuilder;

std::unique_ptr<AggDbHandle> open_agg_db(const std::string& index_path,
                                         std::string& error_msg) {
    std::shared_ptr<dftracer::utils::rocksdb::RocksDatabase> db;
    try {
        db = EventAggregator::open_with_merge_operator(index_path);
    } catch (...) {
        auto& mgr = dftracer::utils::rocksdb::RocksDBManager::instance();
        mgr.reset(index_path);
        db = mgr.get_or_open(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        if (db && db->is_open()) {
            load_intern_dictionary(*db);
        }
    }
    if (!db || !db->is_open()) {
        error_msg = "Failed to open aggregation database";
        return nullptr;
    }
    std::string config_val;
    auto key = std::string_view(AGG_GLOBAL_CONFIG_KEY,
                                sizeof(AGG_GLOBAL_CONFIG_KEY) - 1);
    if (!db->get(key, &config_val, dftracer::utils::rocksdb::cf::AGGREGATION)
             .ok()) {
        error_msg = "No aggregation config found - was aggregation enabled?";
        return nullptr;
    }
    auto cfg = deserialize_agg_global_config(config_val);
    auto handle = std::make_unique<AggDbHandle>();
    handle->db = db;
    handle->agg = std::make_unique<EventAggregator>(db, cfg.config_hash);
    return handle;
}

AggScanOutput scan_aggregation_shard_range(AggScanInput input) {
    AggScanOutput output;

    static const std::vector<ColumnSpec> schema = {
        {"batch_type", ColumnType::INT64},  {"cat", ColumnType::DICT_STRING},
        {"name", ColumnType::DICT_STRING},  {"pid", ColumnType::UINT64},
        {"tid", ColumnType::UINT64},        {"hhash", ColumnType::DICT_STRING},
        {"fhash", ColumnType::DICT_STRING}, {"time_bucket", ColumnType::UINT64},
        {"count", ColumnType::UINT64},      {"dur_total", ColumnType::UINT64},
        {"dur_min", ColumnType::UINT64},    {"dur_max", ColumnType::UINT64},
        {"dur_mean", ColumnType::DOUBLE},   {"dur_std", ColumnType::DOUBLE},
        {"size_total", ColumnType::UINT64}, {"size_min", ColumnType::UINT64},
        {"size_max", ColumnType::UINT64},   {"size_mean", ColumnType::DOUBLE},
        {"size_std", ColumnType::DOUBLE},   {"ts", ColumnType::UINT64},
        {"te", ColumnType::UINT64},
    };

    RecordBatchBuilder builder;
    builder.declare_schema(schema);
    builder.reserve(static_cast<std::size_t>(input.batch_size));

    std::size_t row_count = 0;

    input.agg->scan_shard_range_raw(
        input.shard_begin, input.shard_end,
        [&](std::string_view key_bytes, std::string_view val_bytes) -> bool {
            AggKeyView kv;
            if (!parse_agg_key_view(key_bytes, kv)) return true;
            if (kv.map_type != input.target_type) return true;

            AggMetricsFullView mv;
            if (!parse_agg_value_full_view(val_bytes, mv)) return true;

            std::size_t ci = 0;
            builder.append_int64(ci++,
                                 static_cast<std::int64_t>(input.batch_type));
            builder.append_dict_string(ci++, kv.cat);
            builder.append_dict_string(ci++, kv.name);
            builder.append_uint64(ci++, kv.pid);
            builder.append_uint64(ci++, kv.tid);
            builder.append_dict_string(ci++, kv.hhash);
            builder.append_dict_string(ci++, kv.fhash);
            builder.append_uint64(ci++, kv.time_bucket);
            builder.append_uint64(ci++, mv.count);
            builder.append_uint64(ci++, mv.dur_total);
            builder.append_uint64(ci++, mv.count > 0 ? mv.dur_min : 0);
            builder.append_uint64(ci++, mv.dur_max);
            builder.append_double(ci++, mv.dur_mean);
            builder.append_double(ci++, mv.dur_stddev());
            builder.append_uint64(ci++, mv.size_total);
            builder.append_uint64(ci++, mv.count > 0 ? mv.size_min : 0);
            builder.append_uint64(ci++, mv.size_max);
            builder.append_double(ci++, mv.size_mean);
            builder.append_double(ci++, mv.size_stddev());
            builder.append_uint64(ci++, mv.ts);
            builder.append_uint64(ci++, mv.te);
            builder.end_row();

            row_count++;
            if (static_cast<std::int64_t>(row_count) >= input.batch_size) {
                auto arrow = builder.finish();
                if (arrow.valid()) {
                    output.results.push_back(std::move(arrow));
                }
                builder.reset(true);
                builder.reserve(static_cast<std::size_t>(input.batch_size));
                row_count = 0;
            }
            return true;
        });

    if (row_count > 0) {
        auto arrow = builder.finish();
        if (arrow.valid()) {
            output.results.push_back(std::move(arrow));
        }
    }

    return output;
}

std::optional<GroupByField> parse_group_by_name(std::string_view name) {
    if (name == "cat") return GB_CAT;
    if (name == "func_name") return GB_FUNC_NAME;
    if (name == "pid") return GB_PID;
    if (name == "tid") return GB_TID;
    if (name == "file_hash") return GB_FILE_HASH;
    if (name == "host_hash") return GB_HOST_HASH;
    if (name == "file_name") return GB_FILE_NAME;
    if (name == "host_name") return GB_HOST_NAME;
    if (name == "proc_name") return GB_PROC_NAME;
    if (name == "io_cat") return GB_IO_CAT;
    if (name == "acc_pat") return GB_ACC_PAT;
    if (name == "time_range") return GB_TIME_RANGE;
    return std::nullopt;
}

namespace {

enum class IOCategory : std::int8_t {
    READ = 1,
    WRITE = 2,
    METADATA = 3,
    PCTL = 4,
    IPC = 5,
    OTHER = 6,
    SYNC = 7,
};

inline IOCategory get_io_category(std::string_view func_name) {
    if (func_name == "read" || func_name == "pread" || func_name == "readv" ||
        func_name == "preadv" || func_name == "fread") {
        return IOCategory::READ;
    }
    if (func_name == "write" || func_name == "pwrite" ||
        func_name == "writev" || func_name == "pwritev" ||
        func_name == "fwrite") {
        return IOCategory::WRITE;
    }
    if (func_name == "fsync" || func_name == "fdatasync" ||
        func_name == "msync" || func_name == "sync") {
        return IOCategory::SYNC;
    }
    if (func_name == "open" || func_name == "open64" || func_name == "close" ||
        func_name == "fopen" || func_name == "fopen64" ||
        func_name == "fclose" || func_name == "stat" || func_name == "fstat" ||
        func_name == "lstat" || func_name == "fstatat" ||
        func_name == "__xstat" || func_name == "__xstat64" ||
        func_name == "__lxstat" || func_name == "__lxstat64" ||
        func_name == "__fxstat" || func_name == "__fxstat64" ||
        func_name == "access" || func_name == "lseek" ||
        func_name == "lseek64" || func_name == "fseek" ||
        func_name == "ftell" || func_name == "seek" || func_name == "fcntl" ||
        func_name == "ftruncate" || func_name == "mkdir" ||
        func_name == "rmdir" || func_name == "unlink" ||
        func_name == "remove" || func_name == "rename" || func_name == "link" ||
        func_name == "readlink" || func_name == "opendir" ||
        func_name == "closedir" || func_name == "readdir") {
        return IOCategory::METADATA;
    }
    return IOCategory::OTHER;
}

class HashResolver {
   public:
    HashResolver(
        const std::unordered_map<std::string, std::string>* file_hashes,
        const std::unordered_map<std::string, std::string>* host_hashes)
        : file_hashes_(file_hashes), host_hashes_(host_hashes) {
        if (file_hashes_) {
            for (const auto& [hash, name] : *file_hashes_) {
                auto hash_sv = intern_.intern(hash);
                auto name_sv = intern_.intern(name);
                file_map_[hash_sv] = name_sv;
            }
        }
        if (host_hashes_) {
            for (const auto& [hash, name] : *host_hashes_) {
                auto hash_sv = intern_.intern(hash);
                auto name_sv = intern_.intern(name);
                host_map_[hash_sv] = name_sv;
            }
        }
    }

    // Unresolved hashes resolve to empty (not the hash itself): the
    // dfanalyzer side treats empty file_name/host_name as missing (NA).
    std::string_view resolve_file(std::string_view hash) {
        if (hash.empty()) return hash;
        auto it = file_map_.find(intern_.intern(hash));
        return it != file_map_.end() ? it->second : std::string_view{};
    }

    std::string_view resolve_host(std::string_view hash) {
        if (hash.empty()) return hash;
        auto it = host_map_.find(intern_.intern(hash));
        return it != host_map_.end() ? it->second : std::string_view{};
    }

    std::string_view intern(std::string_view sv) { return intern_.intern(sv); }

   private:
    const std::unordered_map<std::string, std::string>* file_hashes_;
    const std::unordered_map<std::string, std::string>* host_hashes_;
    dftracer::utils::StringIntern intern_;
    std::unordered_map<std::string_view, std::string_view> file_map_;
    std::unordered_map<std::string_view, std::string_view> host_map_;
};

struct ProcKey {
    std::string_view hhash;
    std::uint64_t pid;
    std::uint64_t tid;
    bool operator==(const ProcKey& o) const {
        return hhash == o.hhash && pid == o.pid && tid == o.tid;
    }
};

struct ProcKeyHash {
    std::size_t operator()(const ProcKey& k) const {
        return std::hash<std::string_view>{}(k.hhash) ^
               (std::hash<std::uint64_t>{}(k.pid) << 1) ^
               (std::hash<std::uint64_t>{}(k.tid) << 2);
    }
};

const std::vector<ColumnSpec> DFANALYZER_SCHEMA = {
    {"cat", ColumnType::DICT_STRING},
    {"func_name", ColumnType::DICT_STRING},
    {"pid", ColumnType::INT64},
    {"tid", ColumnType::INT64},
    {"file_hash", ColumnType::DICT_STRING},
    {"host_hash", ColumnType::DICT_STRING},
    {"file_name", ColumnType::DICT_STRING},
    {"host_name", ColumnType::DICT_STRING},
    {"proc_name", ColumnType::DICT_STRING},
    {"io_cat", ColumnType::INT64},
    {"acc_pat", ColumnType::INT64},
    {"count", ColumnType::INT64},
    {"time", ColumnType::DOUBLE},
    {"size", ColumnType::INT64},
    {"time_min", ColumnType::DOUBLE},
    {"time_max", ColumnType::DOUBLE},
    {"size_min", ColumnType::INT64},
    {"size_max", ColumnType::INT64},
    {"offset_min", ColumnType::INT64},
    {"offset_max", ColumnType::INT64},
    {"time_range", ColumnType::INT64},
    {"time_start", ColumnType::INT64},
    {"time_end", ColumnType::INT64},
};

struct CoarseKey {
    std::string_view cat;
    std::string_view func_name;
    std::uint64_t pid = 0;
    std::uint64_t tid = 0;
    std::string_view file_hash;
    std::string_view host_hash;
    std::string_view file_name;
    std::string_view host_name;
    std::string_view proc_name;
    std::int64_t io_cat = 0;
    std::int64_t acc_pat = 0;
    std::int64_t time_range = 0;

    bool operator==(const CoarseKey& o) const {
        return cat == o.cat && func_name == o.func_name && pid == o.pid &&
               tid == o.tid && file_hash == o.file_hash &&
               host_hash == o.host_hash && file_name == o.file_name &&
               host_name == o.host_name && proc_name == o.proc_name &&
               io_cat == o.io_cat && acc_pat == o.acc_pat &&
               time_range == o.time_range;
    }
};

struct CoarseKeyHash {
    std::size_t operator()(const CoarseKey& k) const {
        auto combine = [](std::size_t h, std::size_t v) {
            dftracer::utils::hash_combine(h, v);
            return h;
        };
        std::size_t h = std::hash<std::string_view>{}(k.cat);
        h = combine(h, std::hash<std::string_view>{}(k.func_name));
        h = combine(h, std::hash<std::uint64_t>{}(k.pid));
        h = combine(h, std::hash<std::uint64_t>{}(k.tid));
        h = combine(h, std::hash<std::string_view>{}(k.file_hash));
        h = combine(h, std::hash<std::string_view>{}(k.host_hash));
        h = combine(h, std::hash<std::string_view>{}(k.file_name));
        h = combine(h, std::hash<std::string_view>{}(k.host_name));
        h = combine(h, std::hash<std::string_view>{}(k.proc_name));
        h = combine(h, std::hash<std::int64_t>{}(k.io_cat));
        h = combine(h, std::hash<std::int64_t>{}(k.acc_pat));
        h = combine(h, std::hash<std::int64_t>{}(k.time_range));
        return h;
    }
};

struct CoarseMetrics {
    std::uint64_t count = 0;
    double time_sum = 0.0;
    double time_sq_sum = 0.0;
    double time_min_val = std::numeric_limits<double>::infinity();
    double time_max_val = -std::numeric_limits<double>::infinity();
    double time_call_min_val = std::numeric_limits<double>::infinity();
    double time_call_max_val = -std::numeric_limits<double>::infinity();
    std::uint64_t size_sum = 0;
    double size_sq_sum = 0.0;
    std::uint64_t size_min_val = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t size_max_val = 0;
    std::uint64_t size_call_min_val = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t size_call_max_val = 0;
    bool has_size = false;
    std::uint64_t time_start_val = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t time_end_val = 0;
    bool has_time_bounds = false;
};

inline std::vector<ColumnSpec> make_coarse_schema(const GroupByConfig& cfg) {
    std::vector<ColumnSpec> specs;
    specs.reserve(cfg.order.size() + 16);
    for (std::size_t i = 0; i < cfg.order.size(); ++i) {
        GroupByField f = cfg.order[i];
        const std::string& name = cfg.names[i];
        switch (f) {
            case GB_CAT:
            case GB_FUNC_NAME:
            case GB_FILE_HASH:
            case GB_HOST_HASH:
            case GB_FILE_NAME:
            case GB_HOST_NAME:
            case GB_PROC_NAME:
                specs.push_back({name, ColumnType::DICT_STRING});
                break;
            case GB_PID:
            case GB_TID:
            case GB_IO_CAT:
            case GB_ACC_PAT:
            case GB_TIME_RANGE:
                specs.push_back({name, ColumnType::INT64});
                break;
        }
    }
    specs.push_back({"count", ColumnType::INT64});
    specs.push_back({"time", ColumnType::DOUBLE});
    specs.push_back({"size", ColumnType::INT64});
    specs.push_back({"time_sq", ColumnType::DOUBLE});
    specs.push_back({"size_sq", ColumnType::DOUBLE});
    specs.push_back({"time_min", ColumnType::DOUBLE});
    specs.push_back({"time_max", ColumnType::DOUBLE});
    specs.push_back({"size_min", ColumnType::INT64});
    specs.push_back({"size_max", ColumnType::INT64});
    specs.push_back({"time_call_min", ColumnType::DOUBLE});
    specs.push_back({"time_call_max", ColumnType::DOUBLE});
    specs.push_back({"size_call_min", ColumnType::INT64});
    specs.push_back({"size_call_max", ColumnType::INT64});
    specs.push_back({"time_start", ColumnType::INT64});
    specs.push_back({"time_end", ColumnType::INT64});
    return specs;
}

// Immutable per-scan context shared by the row emitters.
struct DfaEmitCtx {
    const DfanalyzerContext* ctx;
    std::uint64_t bucket_width_us;
    std::int64_t batch_size;
};

// Finish the current batch into `results` and start a fresh one.
void flush_dfa_builder(RecordBatchBuilder& builder, std::size_t& count,
                       std::vector<ArrowExportResult>& results,
                       std::int64_t batch_size) {
    if (count > 0) {
        auto arrow = builder.finish();
        if (arrow.valid()) {
            results.push_back(std::move(arrow));
        }
        builder.reset(true);
        builder.reserve(static_cast<std::size_t>(batch_size));
        count = 0;
    }
}

// Emit one full-granularity row from a single aggregated key/value.
void append_fine_row(RecordBatchBuilder& builder, std::size_t& count,
                     std::vector<ArrowExportResult>& results,
                     const AggKeyView& kv, const AggMetricsView& mv,
                     std::string_view file_name, std::string_view host_name,
                     std::string_view proc_name, IOCategory io_cat,
                     const DfaEmitCtx& ec) {
    std::size_t ci = 0;
    builder.append_dict_string(ci++, kv.cat);
    builder.append_dict_string(ci++, kv.name);
    builder.append_int64(ci++, static_cast<std::int64_t>(kv.pid));
    builder.append_int64(ci++, static_cast<std::int64_t>(kv.tid));
    builder.append_dict_string(ci++, kv.fhash);
    builder.append_dict_string(ci++, kv.hhash);
    builder.append_dict_string(ci++, file_name);
    builder.append_dict_string(ci++, host_name);
    builder.append_dict_string(ci++, proc_name);
    builder.append_int64(ci++, static_cast<std::int64_t>(io_cat));
    builder.append_int64(ci++, 0);

    builder.append_int64(ci++, static_cast<std::int64_t>(mv.count));
    builder.append_double(
        ci++, static_cast<double>(mv.dur_total) / ec.ctx->time_resolution);

    if (mv.size_total > 0) {
        builder.append_int64(ci++, static_cast<std::int64_t>(mv.size_total));
    } else {
        builder.append_null(ci++);
    }

    builder.append_double(ci++, mv.count > 0 ? static_cast<double>(mv.dur_min) /
                                                   ec.ctx->time_resolution
                                             : 0.0);
    builder.append_double(ci++, mv.count > 0 ? static_cast<double>(mv.dur_max) /
                                                   ec.ctx->time_resolution
                                             : 0.0);

    if (mv.size_total > 0 && mv.count > 0) {
        builder.append_int64(ci++, static_cast<std::int64_t>(mv.size_min));
        builder.append_int64(ci++, static_cast<std::int64_t>(mv.size_max));
    } else {
        builder.append_null(ci++);
        builder.append_null(ci++);
    }

    // offset_min > offset_max only when no offset was ever recorded
    // (MetricStats default min=UINT64_MAX, max=0); 0 is a valid offset.
    if (mv.offset_min <= mv.offset_max) {
        builder.append_int64(ci++, static_cast<std::int64_t>(mv.offset_min));
        builder.append_int64(ci++, static_cast<std::int64_t>(mv.offset_max));
    } else {
        builder.append_null(ci++);
        builder.append_null(ci++);
    }

    auto time_range =
        ec.bucket_width_us > 0
            ? static_cast<std::int64_t>((kv.time_bucket - ec.ctx->time_origin) /
                                        ec.bucket_width_us)
            : 0;
    builder.append_int64(ci++, time_range);
    // Counter (profile) rows align to the bucket grid: time_start is the
    // bucket start, time_end one bucket later. Plain events keep the
    // precise min/max event timestamps.
    if (kv.map_type == AggMapType::PROFILE) {
        auto bucket_start =
            static_cast<std::int64_t>(kv.time_bucket - ec.ctx->time_origin);
        builder.append_int64(ci++, bucket_start);
        builder.append_int64(
            ci++, bucket_start + static_cast<std::int64_t>(ec.bucket_width_us));
    } else {
        builder.append_int64(
            ci++, static_cast<std::int64_t>(mv.ts - ec.ctx->time_origin));
        builder.append_int64(
            ci++, static_cast<std::int64_t>(mv.te - ec.ctx->time_origin));
    }
    builder.end_row();

    count++;
    if (static_cast<std::int64_t>(count) >= ec.batch_size) {
        flush_dfa_builder(builder, count, results, ec.batch_size);
    }
}

// Emit one coarse (grouped) row from an accumulated key/metrics pair.
void append_coarse_row(RecordBatchBuilder& builder, const CoarseKey& key,
                       const CoarseMetrics& m, const GroupByConfig& cfg) {
    std::size_t ci = 0;
    for (std::size_t i = 0; i < cfg.order.size(); ++i) {
        switch (cfg.order[i]) {
            case GB_CAT:
                builder.append_dict_string(ci++, key.cat);
                break;
            case GB_FUNC_NAME:
                builder.append_dict_string(ci++, key.func_name);
                break;
            case GB_PID:
                builder.append_int64(ci++, static_cast<std::int64_t>(key.pid));
                break;
            case GB_TID:
                builder.append_int64(ci++, static_cast<std::int64_t>(key.tid));
                break;
            case GB_FILE_HASH:
                builder.append_dict_string(ci++, key.file_hash);
                break;
            case GB_HOST_HASH:
                builder.append_dict_string(ci++, key.host_hash);
                break;
            case GB_FILE_NAME:
                builder.append_dict_string(ci++, key.file_name);
                break;
            case GB_HOST_NAME:
                builder.append_dict_string(ci++, key.host_name);
                break;
            case GB_PROC_NAME:
                builder.append_dict_string(ci++, key.proc_name);
                break;
            case GB_IO_CAT:
                builder.append_int64(ci++, key.io_cat);
                break;
            case GB_ACC_PAT:
                builder.append_int64(ci++, key.acc_pat);
                break;
            case GB_TIME_RANGE:
                builder.append_int64(ci++, key.time_range);
                break;
        }
    }
    builder.append_int64(ci++, static_cast<std::int64_t>(m.count));
    builder.append_double(ci++, m.time_sum);
    if (m.has_size) {
        builder.append_int64(ci++, static_cast<std::int64_t>(m.size_sum));
    } else {
        builder.append_null(ci++);
    }
    builder.append_double(ci++, m.time_sq_sum);
    if (m.has_size) {
        builder.append_double(ci++, m.size_sq_sum);
    } else {
        builder.append_null(ci++);
    }
    builder.append_double(ci++, m.count > 0 ? m.time_min_val : 0.0);
    builder.append_double(ci++, m.count > 0 ? m.time_max_val : 0.0);
    if (m.has_size) {
        builder.append_int64(ci++, static_cast<std::int64_t>(m.size_min_val));
        builder.append_int64(ci++, static_cast<std::int64_t>(m.size_max_val));
    } else {
        builder.append_null(ci++);
        builder.append_null(ci++);
    }
    builder.append_double(ci++, m.count > 0 ? m.time_call_min_val : 0.0);
    builder.append_double(ci++, m.count > 0 ? m.time_call_max_val : 0.0);
    if (m.has_size) {
        builder.append_int64(ci++,
                             static_cast<std::int64_t>(m.size_call_min_val));
        builder.append_int64(ci++,
                             static_cast<std::int64_t>(m.size_call_max_val));
    } else {
        builder.append_null(ci++);
        builder.append_null(ci++);
    }
    builder.append_int64(ci++, m.has_time_bounds
                                   ? static_cast<std::int64_t>(m.time_start_val)
                                   : 0);
    builder.append_int64(ci++, m.has_time_bounds
                                   ? static_cast<std::int64_t>(m.time_end_val)
                                   : 0);
    builder.end_row();
}

}  // namespace

DfanalyzerScanOutput scan_dfanalyzer_shards(DfanalyzerScanInput input) {
    DfanalyzerScanOutput output;

    const bool coarse = input.group_by != nullptr;
    const std::vector<ColumnSpec> coarse_schema =
        coarse ? make_coarse_schema(*input.group_by)
               : std::vector<ColumnSpec>{};

    auto make_builder = [&]() {
        RecordBatchBuilder b;
        if (coarse) {
            b.declare_schema(coarse_schema);
        } else {
            b.declare_schema(DFANALYZER_SCHEMA);
        }
        b.reserve(static_cast<std::size_t>(input.batch_size));
        return b;
    };

    RecordBatchBuilder event_builder, profile_builder, system_builder;
    bool use_events =
        !input.type_filter || *input.type_filter == AggMapType::EVENT;
    bool use_profiles =
        !input.type_filter || *input.type_filter == AggMapType::PROFILE;
    bool use_system =
        !input.type_filter || *input.type_filter == AggMapType::SYSTEM;

    if (use_events) event_builder = make_builder();
    if (use_profiles) profile_builder = make_builder();
    if (use_system) system_builder = make_builder();

    auto bucket_width_us = static_cast<std::uint64_t>(
        input.ctx->time_granularity * input.ctx->time_resolution);
    const DfaEmitCtx emit_ctx{input.ctx, bucket_width_us, input.batch_size};
    std::size_t event_count = 0, profile_count = 0, system_count = 0;

    HashResolver resolver(input.ctx->file_hashes, input.ctx->host_hashes);
    std::unordered_map<ProcKey, std::string, ProcKeyHash> proc_name_cache;
    std::unordered_map<std::string_view, IOCategory> io_cat_cache;

    std::unordered_map<CoarseKey, CoarseMetrics, CoarseKeyHash> event_coarse,
        profile_coarse, system_coarse;

    auto flush_builder = [&](RecordBatchBuilder& builder, std::size_t& count,
                             std::vector<ArrowExportResult>& results) {
        flush_dfa_builder(builder, count, results, input.batch_size);
    };

    auto append_row = [&](RecordBatchBuilder& builder, std::size_t& count,
                          std::vector<ArrowExportResult>& results,
                          const AggKeyView& kv, const AggMetricsView& mv,
                          std::string_view file_name,
                          std::string_view host_name,
                          std::string_view proc_name, IOCategory io_cat) {
        append_fine_row(builder, count, results, kv, mv, file_name, host_name,
                        proc_name, io_cat, emit_ctx);
    };

    auto accumulate_coarse =
        [&](std::unordered_map<CoarseKey, CoarseMetrics, CoarseKeyHash>& map,
            const AggKeyView& kv, const AggMetricsView& mv,
            std::string_view file_name, std::string_view host_name,
            std::string_view proc_name, IOCategory io_cat) {
            const auto& cfg = *input.group_by;
            // Probe with non-interned views; hash/equality compare by content,
            // so string_view lifetime doesn't matter for lookup. We only copy
            // (intern) on first insert.
            CoarseKey probe;
            if (cfg.mask & GB_CAT) probe.cat = kv.cat;
            if (cfg.mask & GB_FUNC_NAME) probe.func_name = kv.name;
            if (cfg.mask & GB_PID) probe.pid = kv.pid;
            if (cfg.mask & GB_TID) probe.tid = kv.tid;
            if (cfg.mask & GB_FILE_HASH) probe.file_hash = kv.fhash;
            if (cfg.mask & GB_HOST_HASH) probe.host_hash = kv.hhash;
            if (cfg.mask & GB_FILE_NAME) probe.file_name = file_name;
            if (cfg.mask & GB_HOST_NAME) probe.host_name = host_name;
            if (cfg.mask & GB_PROC_NAME) probe.proc_name = proc_name;
            if (cfg.mask & GB_IO_CAT)
                probe.io_cat = static_cast<std::int64_t>(io_cat);
            if (cfg.mask & GB_TIME_RANGE) {
                probe.time_range =
                    bucket_width_us > 0
                        ? static_cast<std::int64_t>(
                              (kv.time_bucket - input.ctx->time_origin) /
                              bucket_width_us)
                        : 0;
            }
            // acc_pat is always 0 today; included for completeness.

            auto it = map.find(probe);
            if (it == map.end()) {
                // First sighting: promote views referencing unstable DB buffers
                // to interned copies. file_name/host_name come from the
                // resolver's intern pool, and proc_name from proc_name_cache;
                // both already stable across iterations, no copy needed.
                CoarseKey stable = probe;
                if (cfg.mask & GB_CAT) stable.cat = resolver.intern(kv.cat);
                if (cfg.mask & GB_FUNC_NAME)
                    stable.func_name = resolver.intern(kv.name);
                if (cfg.mask & GB_FILE_HASH)
                    stable.file_hash = resolver.intern(kv.fhash);
                if (cfg.mask & GB_HOST_HASH)
                    stable.host_hash = resolver.intern(kv.hhash);
                auto [nit, _] = map.emplace(std::move(stable), CoarseMetrics{});
                it = nit;
            }
            CoarseMetrics& m = it->second;
            m.count += mv.count;
            double time_val =
                static_cast<double>(mv.dur_total) / input.ctx->time_resolution;
            m.time_sum += time_val;
            m.time_sq_sum += time_val * time_val;
            if (time_val < m.time_call_min_val) m.time_call_min_val = time_val;
            if (time_val > m.time_call_max_val) m.time_call_max_val = time_val;
            if (mv.count > 0) {
                double dur_min_v = static_cast<double>(mv.dur_min) /
                                   input.ctx->time_resolution;
                double dur_max_v = static_cast<double>(mv.dur_max) /
                                   input.ctx->time_resolution;
                if (dur_min_v < m.time_min_val) m.time_min_val = dur_min_v;
                if (dur_max_v > m.time_max_val) m.time_max_val = dur_max_v;
            }
            if (mv.size_total > 0) {
                m.has_size = true;
                m.size_sum += mv.size_total;
                double sz = static_cast<double>(mv.size_total);
                m.size_sq_sum += sz * sz;
                if (mv.size_total < m.size_call_min_val)
                    m.size_call_min_val = mv.size_total;
                if (mv.size_total > m.size_call_max_val)
                    m.size_call_max_val = mv.size_total;
                if (mv.count > 0) {
                    if (mv.size_min < m.size_min_val)
                        m.size_min_val = mv.size_min;
                    if (mv.size_max > m.size_max_val)
                        m.size_max_val = mv.size_max;
                }
            }
            if (mv.ts >= input.ctx->time_origin) {
                m.has_time_bounds = true;
                auto ts_off = mv.ts - input.ctx->time_origin;
                auto te_off = mv.te - input.ctx->time_origin;
                if (ts_off < m.time_start_val) m.time_start_val = ts_off;
                if (te_off > m.time_end_val) m.time_end_val = te_off;
            }
        };

    input.agg->scan_shard_range_raw(
        input.shard_begin, input.shard_end,
        [&](std::string_view key_bytes, std::string_view val_bytes) -> bool {
            AggKeyView kv;
            if (!parse_agg_key_view(key_bytes, kv)) return true;

            if (input.type_filter && kv.map_type != *input.type_filter)
                return true;

            if (input.ctx->query_filter) {
                auto& q = *input.ctx->query_filter;
                dftracer::utils::utilities::common::query::ValueMap fields;
                if (q.references("cat")) fields["cat"] = std::string(kv.cat);
                if (q.references("name")) fields["name"] = std::string(kv.name);
                if (q.references("pid")) fields["pid"] = kv.pid;
                if (q.references("tid")) fields["tid"] = kv.tid;
                if (q.references("hhash"))
                    fields["hhash"] = std::string(kv.hhash);
                if (q.references("fhash"))
                    fields["fhash"] = std::string(kv.fhash);
                if (q.references("time_bucket"))
                    fields["time_bucket"] = kv.time_bucket;
                if (!q.evaluate(fields)) return true;
            }

            AggMetricsView mv;
            if (!parse_agg_value_view(val_bytes, mv)) return true;

            auto file_name = resolver.resolve_file(kv.fhash);
            auto host_name = resolver.resolve_host(kv.hhash);

            ProcKey pk{kv.hhash, kv.pid, kv.tid};
            auto proc_it = proc_name_cache.find(pk);
            std::string_view proc_name;
            if (proc_it != proc_name_cache.end()) {
                proc_name = proc_it->second;
            } else {
                std::string pn = "app#";
                if (!host_name.empty()) {
                    pn.append(host_name);
                } else if (!kv.hhash.empty()) {
                    pn.append(kv.hhash);
                } else {
                    pn.append("unknown");
                }
                pn.push_back('#');
                pn.append(std::to_string(kv.pid));
                pn.push_back('#');
                pn.append(std::to_string(kv.tid));
                ProcKey stable_pk{resolver.intern(kv.hhash), kv.pid, kv.tid};
                auto [it, _] =
                    proc_name_cache.emplace(stable_pk, std::move(pn));
                proc_name = it->second;
            }

            auto io_it = io_cat_cache.find(kv.name);
            IOCategory io_cat;
            if (io_it != io_cat_cache.end()) {
                io_cat = io_it->second;
            } else {
                io_cat = get_io_category(kv.name);
                io_cat_cache[resolver.intern(kv.name)] = io_cat;
            }

            if (coarse) {
                switch (kv.map_type) {
                    case AggMapType::EVENT:
                        if (use_events)
                            accumulate_coarse(event_coarse, kv, mv, file_name,
                                              host_name, proc_name, io_cat);
                        break;
                    case AggMapType::PROFILE:
                        if (use_profiles)
                            accumulate_coarse(profile_coarse, kv, mv, file_name,
                                              host_name, proc_name, io_cat);
                        break;
                    case AggMapType::SYSTEM:
                        if (use_system)
                            accumulate_coarse(system_coarse, kv, mv, file_name,
                                              host_name, proc_name, io_cat);
                        break;
                }
            } else {
                switch (kv.map_type) {
                    case AggMapType::EVENT:
                        append_row(event_builder, event_count, output.events,
                                   kv, mv, file_name, host_name, proc_name,
                                   io_cat);
                        break;
                    case AggMapType::PROFILE:
                        append_row(profile_builder, profile_count,
                                   output.profiles, kv, mv, file_name,
                                   host_name, proc_name, io_cat);
                        break;
                    case AggMapType::SYSTEM:
                        append_row(system_builder, system_count, output.system,
                                   kv, mv, file_name, host_name, proc_name,
                                   io_cat);
                        break;
                }
            }
            return true;
        });

    if (coarse) {
        const auto& cfg = *input.group_by;
        auto flush_coarse = [&](std::unordered_map<CoarseKey, CoarseMetrics,
                                                   CoarseKeyHash>& map,
                                RecordBatchBuilder& builder, std::size_t& count,
                                std::vector<ArrowExportResult>& results) {
            for (auto& [key, m] : map) {
                append_coarse_row(builder, key, m, cfg);
                ++count;
                if (static_cast<std::int64_t>(count) >= input.batch_size) {
                    flush_builder(builder, count, results);
                }
            }
            flush_builder(builder, count, results);
        };
        if (use_events)
            flush_coarse(event_coarse, event_builder, event_count,
                         output.events);
        if (use_profiles)
            flush_coarse(profile_coarse, profile_builder, profile_count,
                         output.profiles);
        if (use_system)
            flush_coarse(system_coarse, system_builder, system_count,
                         output.system);
    } else {
        if (use_events)
            flush_builder(event_builder, event_count, output.events);
        if (use_profiles)
            flush_builder(profile_builder, profile_count, output.profiles);
        if (use_system)
            flush_builder(system_builder, system_count, output.system);
    }

    return output;
}

std::vector<ArrowExportResult> scan_system_metrics_buffer(
    const EventAggregator* agg, const DfanalyzerContext* ctx,
    std::int64_t batch_size) {
    std::vector<ArrowExportResult> results;
    if (!agg) return results;

    std::vector<std::string> metric_names_ordered;
    std::unordered_set<std::string> metric_name_seen;
    agg->scan_system_metrics_raw(
        [&](std::string_view, std::string_view val_bytes) -> bool {
            auto m = deserialize_system_value(val_bytes);
            if (m.metrics) {
                for (const auto& [name, _] : *m.metrics) {
                    if (metric_name_seen.insert(name).second) {
                        metric_names_ordered.push_back(name);
                    }
                }
            }
            return true;
        });

    if (metric_names_ordered.empty()) return results;

    // SystemAggregationMetrics::metrics is an unordered_map; sort the
    // discovered column names so the emitted Arrow schema is deterministic
    // across runs and builds.
    std::sort(metric_names_ordered.begin(), metric_names_ordered.end());

    std::vector<ColumnSpec> schema;
    schema.reserve(6 + metric_names_ordered.size());
    schema.push_back({"host_hash", ColumnType::DICT_STRING});
    schema.push_back({"name", ColumnType::DICT_STRING});
    schema.push_back({"time_bucket", ColumnType::INT64});
    schema.push_back({"ts", ColumnType::INT64});
    schema.push_back({"te", ColumnType::INT64});
    schema.push_back({"count", ColumnType::INT64});
    for (const auto& mn : metric_names_ordered) {
        schema.push_back({mn, ColumnType::DOUBLE});
    }

    RecordBatchBuilder builder;
    builder.declare_schema(schema);
    builder.reserve(static_cast<std::size_t>(batch_size));

    auto flush = [&](std::size_t& row_count) {
        if (row_count == 0) return;
        auto arrow = builder.finish();
        if (arrow.valid()) results.push_back(std::move(arrow));
        builder.reset(true);
        builder.reserve(static_cast<std::size_t>(batch_size));
        row_count = 0;
    };

    std::size_t row_count = 0;
    const std::size_t n_metric_cols = metric_names_ordered.size();

    agg->scan_system_metrics_raw(
        [&](std::string_view key_bytes, std::string_view val_bytes) -> bool {
            auto k = deserialize_system_key(key_bytes);
            auto m = deserialize_system_value(val_bytes);

            std::size_t ci = 0;
            builder.append_dict_string(ci++, k.key.hhash);
            builder.append_dict_string(ci++, k.key.name);
            builder.append_int64(ci++,
                                 static_cast<std::int64_t>(k.key.time_bucket));
            builder.append_int64(ci++, static_cast<std::int64_t>(m.ts));
            builder.append_int64(ci++, static_cast<std::int64_t>(m.te));
            builder.append_int64(ci++, static_cast<std::int64_t>(m.count));

            for (std::size_t i = 0; i < n_metric_cols; ++i) {
                const auto& mn = metric_names_ordered[i];
                bool present = false;
                if (m.metrics) {
                    auto it = m.metrics->find(mn);
                    if (it != m.metrics->end()) {
                        builder.append_double(ci++, it->second.mean);
                        present = true;
                    }
                }
                if (!present) builder.append_null(ci++);
            }
            builder.end_row();
            row_count++;
            if (static_cast<std::int64_t>(row_count) >= batch_size) {
                flush(row_count);
            }
            return true;
        });
    flush(row_count);

    (void)ctx;
    return results;
}

}  // namespace dftracer::utils::utilities::composites::dft::dfanalyzer

#endif  // DFTRACER_UTILS_ENABLE_ARROW
