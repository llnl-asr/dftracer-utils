#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#include <dftracer/utils/utilities/common/arrow/partition_router.h>
#include <nanoarrow/nanoarrow.h>

#include <cstring>
#include <functional>
#include <iomanip>
#include <sstream>

namespace dftracer::utils::utilities::common::arrow {

namespace {

std::string extract_string(const ArrowArrayView* view, int64_t idx) {
    ArrowStringView sv = ArrowArrayViewGetStringUnsafe(view, idx);
    return std::string(sv.data, sv.size_bytes);
}

int64_t extract_int64(const ArrowArrayView* view, int64_t idx) {
    return ArrowArrayViewGetIntUnsafe(view, idx);
}

uint64_t extract_uint64(const ArrowArrayView* view, int64_t idx) {
    return static_cast<uint64_t>(ArrowArrayViewGetUIntUnsafe(view, idx));
}

double extract_double(const ArrowArrayView* view, int64_t idx) {
    return ArrowArrayViewGetDoubleUnsafe(view, idx);
}

bool is_null(const ArrowArrayView* view, int64_t idx) {
    return ArrowArrayViewIsNull(view, idx);
}

std::string value_to_string(const ArrowArrayView* view, int64_t idx) {
    if (is_null(view, idx)) {
        return "__null__";
    }

    switch (view->storage_type) {
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING:
            return extract_string(view, idx);
        case NANOARROW_TYPE_INT64:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT8:
            return std::to_string(extract_int64(view, idx));
        case NANOARROW_TYPE_UINT64:
        case NANOARROW_TYPE_UINT32:
        case NANOARROW_TYPE_UINT16:
        case NANOARROW_TYPE_UINT8:
            return std::to_string(extract_uint64(view, idx));
        case NANOARROW_TYPE_DOUBLE:
        case NANOARROW_TYPE_FLOAT:
            return std::to_string(extract_double(view, idx));
        case NANOARROW_TYPE_BOOL:
            return extract_int64(view, idx) ? "true" : "false";
        default:
            return "__unsupported__";
    }
}

ColumnType nanoarrow_to_column_type(ArrowType type) {
    switch (type) {
        case NANOARROW_TYPE_INT64:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT8:
            return ColumnType::INT64;
        case NANOARROW_TYPE_UINT64:
        case NANOARROW_TYPE_UINT32:
        case NANOARROW_TYPE_UINT16:
        case NANOARROW_TYPE_UINT8:
            return ColumnType::UINT64;
        case NANOARROW_TYPE_DOUBLE:
        case NANOARROW_TYPE_FLOAT:
            return ColumnType::DOUBLE;
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING:
            return ColumnType::STRING;
        case NANOARROW_TYPE_BOOL:
            return ColumnType::BOOL;
        default:
            return ColumnType::STRING;
    }
}

uint64_t fnv1a_hash(const std::string& s) {
    uint64_t hash = 14695981039346656037ULL;
    for (char c : s) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

}  // namespace

PartitionRouter::~PartitionRouter() {}

PartitionRouter::PartitionRouter(PartitionRouter&& other) noexcept
    : output_dir_(std::move(other.output_dir_)),
      config_(std::move(other.config_)),
      chunk_size_bytes_(other.chunk_size_bytes_),
      compression_(other.compression_),
      is_open_(other.is_open_),
      writers_(std::move(other.writers_)),
      predicates_(std::move(other.predicates_)) {
    other.is_open_ = false;
}

PartitionRouter& PartitionRouter::operator=(PartitionRouter&& other) noexcept {
    if (this != &other) {
        output_dir_ = std::move(other.output_dir_);
        config_ = std::move(other.config_);
        chunk_size_bytes_ = other.chunk_size_bytes_;
        compression_ = other.compression_;
        is_open_ = other.is_open_;
        writers_ = std::move(other.writers_);
        predicates_ = std::move(other.predicates_);
        other.is_open_ = false;
    }
    return *this;
}

int PartitionRouter::open(const std::string& output_dir,
                          const PartitionConfig& config,
                          int64_t chunk_size_bytes,
                          IpcCompression compression) {
    if (is_open_) return -1;

    std::error_code ec;
    fs::create_directories(output_dir, ec);
    if (ec) return -1;

    output_dir_ = output_dir;
    config_ = config;
    chunk_size_bytes_ = chunk_size_bytes;
    compression_ = compression;
    writers_.clear();
    predicates_.clear();

    is_open_ = true;
    return 0;
}

void PartitionRouter::register_predicate(const std::string& view_name,
                                         PredicateEvaluator evaluator) {
    predicates_[view_name] = std::move(evaluator);
}

std::string PartitionRouter::partition_path(
    const std::string& partition_key) const {
    if (partition_key.empty()) {
        return output_dir_;
    }
    return (fs::path(output_dir_) / partition_key).string();
}

coro::CoroTask<PartitionWriter*> PartitionRouter::get_or_create_writer(
    const std::string& partition_key) {
    auto it = writers_.find(partition_key);
    if (it != writers_.end()) {
        co_return it->second.get();
    }

    auto writer = std::make_unique<PartitionWriter>();
    std::string path = partition_path(partition_key);
    if (co_await writer->open(path, chunk_size_bytes_, compression_) != 0) {
        co_return nullptr;
    }

    PartitionWriter* ptr = writer.get();
    writers_[partition_key] = std::move(writer);
    co_return ptr;
}

int PartitionRouter::compute_bucket(
    const std::vector<std::string>& values) const {
    std::string combined;
    for (const auto& v : values) {
        combined += v;
        combined += '\0';
    }
    return static_cast<int>(fnv1a_hash(combined) %
                            static_cast<uint64_t>(config_.num_buckets));
}

coro::CoroTask<int> PartitionRouter::route_none(ArrowExportResult& batch) {
    PartitionWriter* writer = co_await get_or_create_writer("");
    if (!writer) co_return -1;
    co_return co_await writer->write_batch(batch);
}

coro::CoroTask<int> PartitionRouter::route_column(ArrowExportResult& batch) {
    ArrowSchema* schema = batch.get_schema();
    ArrowArray* array = batch.get_array();
    int64_t num_rows = batch.num_rows();

    if (num_rows == 0) co_return 0;

    ArrowArrayView view;
    ArrowError error;
    int rc = ArrowArrayViewInitFromSchema(&view, schema, &error);
    if (rc != NANOARROW_OK) {
        ArrowArrayViewReset(&view);
        co_return rc;
    }
    rc = ArrowArrayViewSetArray(&view, array, &error);
    if (rc != NANOARROW_OK) {
        ArrowArrayViewReset(&view);
        co_return rc;
    }

    std::vector<int64_t> partition_col_indices;
    for (const auto& col_name : config_.partition_columns) {
        for (int64_t i = 0; i < schema->n_children; i++) {
            if (schema->children[i]->name == col_name) {
                partition_col_indices.push_back(i);
                break;
            }
        }
    }

    if (partition_col_indices.size() != config_.partition_columns.size()) {
        ArrowArrayViewReset(&view);
        co_return -1;
    }

    std::unordered_map<std::string, std::vector<int64_t>> partition_rows;

    for (int64_t row = 0; row < num_rows; row++) {
        std::string partition_key;
        for (size_t i = 0; i < partition_col_indices.size(); i++) {
            int64_t col_idx = partition_col_indices[i];
            const ArrowArrayView* col_view = view.children[col_idx];
            std::string value = value_to_string(col_view, row);

            if (i > 0) partition_key += "/";
            partition_key += config_.partition_columns[i] + "=" + value;
        }
        partition_rows[partition_key].push_back(row);
    }

    std::vector<ColumnSpec> col_specs;
    col_specs.reserve(schema->n_children);
    for (int64_t i = 0; i < schema->n_children; i++) {
        ArrowType type = view.children[i]->storage_type;
        col_specs.push_back(
            {schema->children[i]->name, nanoarrow_to_column_type(type)});
    }

    for (auto& [partition_key, rows] : partition_rows) {
        RecordBatchBuilder builder;
        builder.declare_schema(col_specs);
        builder.reserve(rows.size());

        for (int64_t row : rows) {
            for (int64_t col = 0; col < schema->n_children; col++) {
                const ArrowArrayView* col_view = view.children[col];

                if (is_null(col_view, row)) {
                    builder.append_null(col);
                } else {
                    switch (col_view->storage_type) {
                        case NANOARROW_TYPE_INT64:
                        case NANOARROW_TYPE_INT32:
                        case NANOARROW_TYPE_INT16:
                        case NANOARROW_TYPE_INT8:
                            builder.append_int64(col,
                                                 extract_int64(col_view, row));
                            break;
                        case NANOARROW_TYPE_UINT64:
                        case NANOARROW_TYPE_UINT32:
                        case NANOARROW_TYPE_UINT16:
                        case NANOARROW_TYPE_UINT8:
                            builder.append_uint64(
                                col, extract_uint64(col_view, row));
                            break;
                        case NANOARROW_TYPE_DOUBLE:
                        case NANOARROW_TYPE_FLOAT:
                            builder.append_double(
                                col, extract_double(col_view, row));
                            break;
                        case NANOARROW_TYPE_STRING:
                        case NANOARROW_TYPE_LARGE_STRING:
                            builder.append_string(
                                col, extract_string(col_view, row));
                            break;
                        case NANOARROW_TYPE_BOOL:
                            builder.append_bool(
                                col, extract_int64(col_view, row) != 0);
                            break;
                        default:
                            builder.append_null(col);
                            break;
                    }
                }
            }
            builder.end_row();
        }

        auto sub_batch = builder.finish();
        PartitionWriter* writer = co_await get_or_create_writer(partition_key);
        if (!writer) {
            ArrowArrayViewReset(&view);
            co_return -1;
        }
        rc = co_await writer->write_batch(sub_batch);
        if (rc != 0) {
            ArrowArrayViewReset(&view);
            co_return rc;
        }
    }

    ArrowArrayViewReset(&view);
    co_return 0;
}

coro::CoroTask<int> PartitionRouter::route_bucketed(ArrowExportResult& batch) {
    ArrowSchema* schema = batch.get_schema();
    ArrowArray* array = batch.get_array();
    int64_t num_rows = batch.num_rows();

    if (num_rows == 0) co_return 0;

    ArrowArrayView view;
    ArrowError error;
    int rc = ArrowArrayViewInitFromSchema(&view, schema, &error);
    if (rc != NANOARROW_OK) {
        ArrowArrayViewReset(&view);
        co_return rc;
    }
    rc = ArrowArrayViewSetArray(&view, array, &error);
    if (rc != NANOARROW_OK) {
        ArrowArrayViewReset(&view);
        co_return rc;
    }

    std::vector<int64_t> partition_col_indices;
    for (const auto& col_name : config_.partition_columns) {
        for (int64_t i = 0; i < schema->n_children; i++) {
            if (schema->children[i]->name == col_name) {
                partition_col_indices.push_back(i);
                break;
            }
        }
    }

    if (partition_col_indices.size() != config_.partition_columns.size()) {
        ArrowArrayViewReset(&view);
        co_return -1;
    }

    std::unordered_map<int, std::vector<int64_t>> bucket_rows;

    for (int64_t row = 0; row < num_rows; row++) {
        std::vector<std::string> values;
        values.reserve(partition_col_indices.size());
        for (int64_t col_idx : partition_col_indices) {
            values.push_back(value_to_string(view.children[col_idx], row));
        }
        int bucket = compute_bucket(values);
        bucket_rows[bucket].push_back(row);
    }

    std::vector<ColumnSpec> col_specs;
    col_specs.reserve(schema->n_children);
    for (int64_t i = 0; i < schema->n_children; i++) {
        col_specs.push_back(
            {schema->children[i]->name,
             nanoarrow_to_column_type(view.children[i]->storage_type)});
    }

    auto bucket_key = [&](int bucket) {
        std::ostringstream ss;
        ss << config_.partition_columns[0] << "_bucket=" << std::setw(2)
           << std::setfill('0') << bucket;
        return ss.str();
    };

    for (auto& [bucket, rows] : bucket_rows) {
        RecordBatchBuilder builder;
        builder.declare_schema(col_specs);
        builder.reserve(rows.size());

        for (int64_t row : rows) {
            for (int64_t col = 0; col < schema->n_children; col++) {
                const ArrowArrayView* col_view = view.children[col];

                if (is_null(col_view, row)) {
                    builder.append_null(col);
                } else {
                    switch (col_view->storage_type) {
                        case NANOARROW_TYPE_INT64:
                        case NANOARROW_TYPE_INT32:
                        case NANOARROW_TYPE_INT16:
                        case NANOARROW_TYPE_INT8:
                            builder.append_int64(col,
                                                 extract_int64(col_view, row));
                            break;
                        case NANOARROW_TYPE_UINT64:
                        case NANOARROW_TYPE_UINT32:
                        case NANOARROW_TYPE_UINT16:
                        case NANOARROW_TYPE_UINT8:
                            builder.append_uint64(
                                col, extract_uint64(col_view, row));
                            break;
                        case NANOARROW_TYPE_DOUBLE:
                        case NANOARROW_TYPE_FLOAT:
                            builder.append_double(
                                col, extract_double(col_view, row));
                            break;
                        case NANOARROW_TYPE_STRING:
                        case NANOARROW_TYPE_LARGE_STRING:
                            builder.append_string(
                                col, extract_string(col_view, row));
                            break;
                        case NANOARROW_TYPE_BOOL:
                            builder.append_bool(
                                col, extract_int64(col_view, row) != 0);
                            break;
                        default:
                            builder.append_null(col);
                            break;
                    }
                }
            }
            builder.end_row();
        }

        auto sub_batch = builder.finish();
        PartitionWriter* writer =
            co_await get_or_create_writer(bucket_key(bucket));
        if (!writer) {
            ArrowArrayViewReset(&view);
            co_return -1;
        }
        rc = co_await writer->write_batch(sub_batch);
        if (rc != 0) {
            ArrowArrayViewReset(&view);
            co_return rc;
        }
    }

    ArrowArrayViewReset(&view);
    co_return 0;
}

coro::CoroTask<int> PartitionRouter::route_view(ArrowExportResult& batch) {
    ArrowSchema* schema = batch.get_schema();
    ArrowArray* array = batch.get_array();
    int64_t num_rows = batch.num_rows();

    if (num_rows == 0) co_return 0;

    ArrowArrayView view;
    ArrowError error;
    int rc = ArrowArrayViewInitFromSchema(&view, schema, &error);
    if (rc != NANOARROW_OK) {
        ArrowArrayViewReset(&view);
        co_return rc;
    }
    rc = ArrowArrayViewSetArray(&view, array, &error);
    if (rc != NANOARROW_OK) {
        ArrowArrayViewReset(&view);
        co_return rc;
    }

    std::unordered_map<std::string, int64_t> col_name_to_idx;
    for (int64_t i = 0; i < schema->n_children; i++) {
        col_name_to_idx[schema->children[i]->name] = i;
    }

    std::unordered_map<std::string, std::vector<int64_t>> view_rows;

    for (int64_t row = 0; row < num_rows; row++) {
        std::unordered_map<std::string, std::string> row_values;
        for (int64_t col = 0; col < schema->n_children; col++) {
            row_values[schema->children[col]->name] =
                value_to_string(view.children[col], row);
        }

        std::string matched_view;
        for (const auto& [view_name, predicate] : config_.views) {
            if (!predicate.has_value()) {
                if (matched_view.empty()) {
                    matched_view = view_name;
                }
                continue;
            }

            auto it = predicates_.find(view_name);
            if (it != predicates_.end() && it->second(row_values)) {
                matched_view = view_name;
                break;
            }
        }

        if (!matched_view.empty()) {
            view_rows[matched_view].push_back(row);
        }
    }

    std::vector<ColumnSpec> col_specs;
    col_specs.reserve(schema->n_children);
    for (int64_t i = 0; i < schema->n_children; i++) {
        col_specs.push_back(
            {schema->children[i]->name,
             nanoarrow_to_column_type(view.children[i]->storage_type)});
    }

    for (auto& [view_name, rows] : view_rows) {
        RecordBatchBuilder builder;
        builder.declare_schema(col_specs);
        builder.reserve(rows.size());

        for (int64_t row : rows) {
            for (int64_t col = 0; col < schema->n_children; col++) {
                const ArrowArrayView* col_view = view.children[col];

                if (is_null(col_view, row)) {
                    builder.append_null(col);
                } else {
                    switch (col_view->storage_type) {
                        case NANOARROW_TYPE_INT64:
                        case NANOARROW_TYPE_INT32:
                        case NANOARROW_TYPE_INT16:
                        case NANOARROW_TYPE_INT8:
                            builder.append_int64(col,
                                                 extract_int64(col_view, row));
                            break;
                        case NANOARROW_TYPE_UINT64:
                        case NANOARROW_TYPE_UINT32:
                        case NANOARROW_TYPE_UINT16:
                        case NANOARROW_TYPE_UINT8:
                            builder.append_uint64(
                                col, extract_uint64(col_view, row));
                            break;
                        case NANOARROW_TYPE_DOUBLE:
                        case NANOARROW_TYPE_FLOAT:
                            builder.append_double(
                                col, extract_double(col_view, row));
                            break;
                        case NANOARROW_TYPE_STRING:
                        case NANOARROW_TYPE_LARGE_STRING:
                            builder.append_string(
                                col, extract_string(col_view, row));
                            break;
                        case NANOARROW_TYPE_BOOL:
                            builder.append_bool(
                                col, extract_int64(col_view, row) != 0);
                            break;
                        default:
                            builder.append_null(col);
                            break;
                    }
                }
            }
            builder.end_row();
        }

        auto sub_batch = builder.finish();
        PartitionWriter* writer = co_await get_or_create_writer(view_name);
        if (!writer) {
            ArrowArrayViewReset(&view);
            co_return -1;
        }
        rc = co_await writer->write_batch(sub_batch);
        if (rc != 0) {
            ArrowArrayViewReset(&view);
            co_return rc;
        }
    }

    ArrowArrayViewReset(&view);
    co_return 0;
}

coro::CoroTask<int> PartitionRouter::write_batch(ArrowExportResult& batch) {
    if (!is_open_ || !batch.valid()) co_return -1;

    switch (config_.mode) {
        case PartitionConfig::Mode::NONE:
            co_return co_await route_none(batch);
        case PartitionConfig::Mode::COLUMN:
            co_return co_await route_column(batch);
        case PartitionConfig::Mode::BUCKETED:
            co_return co_await route_bucketed(batch);
        case PartitionConfig::Mode::VIEW:
            co_return co_await route_view(batch);
    }
    co_return -1;
}

coro::CoroTask<RouterWriteStats> PartitionRouter::close() {
    RouterWriteStats stats;

    if (!is_open_) co_return stats;

    for (auto& [partition_key, writer] : writers_) {
        auto partition_stats = co_await writer->close();
        stats.partitions[partition_key] = std::move(partition_stats);
        stats.total_rows += stats.partitions[partition_key].total_rows;
        stats.total_uncompressed_bytes +=
            stats.partitions[partition_key].total_uncompressed_bytes;
    }

    writers_.clear();
    predicates_.clear();
    is_open_ = false;

    co_return stats;
}

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
