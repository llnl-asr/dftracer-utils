#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_merge_operator.h>
#include <dftracer/utils/index/schemas/dft/agg/system_metrics_merge_operator.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/error.h>
#include <dftracer/utils/index/store/index_database_sst_writer_context.h>
#include <dftracer/utils/index/store/internal/db_error.h>
#include <rocksdb/sst_file_writer.h>

#include <algorithm>
#include <stdexcept>

namespace dftracer::utils::index::store {

namespace {

using Entry = IndexDatabaseSstWriterContext::Entry;

// Sorted, one entry per key: a run of same-key merge operands is combined
// with `merge_op` (PartialMerge is associative, so reads see the same result
// as separate operands); any other run keeps its last entry.
std::string emit_sst(
    const std::string& path, std::vector<Entry>& entries,
    const std::vector<std::pair<std::string, std::string>>& ranges,
    const ::rocksdb::MergeOperator* merge_op) {
    std::stable_sort(
        entries.begin(), entries.end(),
        [](const auto& a, const auto& b) { return a.key < b.key; });

    ::rocksdb::EnvOptions env_opts;
    ::rocksdb::Options writer_options(
        index::store::RocksDatabase::default_options(),
        index::store::RocksDatabase::default_column_family_options());
    ::rocksdb::SstFileWriter writer(env_opts, writer_options);

    auto status = writer.Open(path);
    if (!status.ok())
        internal::throw_db_error("Failed to open SST writer at '" + path + "'",
                                 status);
    for (const auto& [begin, end] : ranges) {
        status = writer.DeleteRange(begin, end);
        if (!status.ok())
            internal::throw_db_error(
                "Failed to add a range delete to SST '" + path + "'", status);
    }
    std::size_t i = 0;
    while (i < entries.size()) {
        std::size_t j = i + 1;
        while (j < entries.size() && entries[j].key == entries[i].key) ++j;
        if (entries[i].is_merge && j - i > 1 && merge_op) {
            std::string combined = entries[i].value;
            for (std::size_t k = i + 1; k < j; ++k) {
                std::string next;
                if (!merge_op->PartialMerge(entries[i].key, combined,
                                            entries[k].value, &next, nullptr))
                    internal::throw_db_error(
                        "PartialMerge failed combining SST operands for '" +
                            path + "'",
                        ::rocksdb::Status::Corruption("PartialMerge"));
                combined = std::move(next);
            }
            status = writer.Merge(entries[i].key, combined);
        } else {
            const auto& e = entries[j - 1];
            status = e.is_merge ? writer.Merge(e.key, e.value)
                                : writer.Put(e.key, e.value);
        }
        if (!status.ok())
            internal::throw_db_error("Failed to append to SST '" + path + "'",
                                     status);
        i = j;
    }
    status = writer.Finish();
    if (!status.ok())
        internal::throw_db_error("Failed to finalize SST '" + path + "'",
                                 status);
    return path;
}

/// Move one file from `src` to `dst`. Uses rename (O(1) same-FS) with a
/// copy+unlink fallback for cross-FS. `dst` parent directory must exist.
void move_file(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::rename(src, dst, ec);
    if (!ec) return;
    // Cross-FS or other rename failure -> fall back to copy.
    ec.clear();
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        throw IndexerError(IndexerError::Type::FILE_ERROR,
                           "Failed to move SST '" + src.string() + "' to '" +
                               dst.string() + "': " + ec.message());
    }
    fs::remove(src, ec);  // best-effort; staging cleanup handled by caller
}

}  // namespace

bool IndexDatabaseSstWriterContext::Artifacts::empty() const noexcept {
    for (const auto& p : sst)
        if (p) return false;
    return true;
}

IndexDatabaseSstWriterContext::Artifacts
IndexDatabaseSstWriterContext::Artifacts::move_to(
    std::string_view dest_dir) && {
    const fs::path dir(dest_dir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        throw IndexerError(IndexerError::Type::FILE_ERROR,
                           "Failed to create SST move destination '" +
                               std::string(dest_dir) + "': " + ec.message());
    }
    Artifacts out;
    for (std::size_t f = 0; f < sst.size(); ++f) {
        if (!sst[f]) continue;
        const fs::path src(*sst[f]);
        const fs::path dst = dir / src.filename();
        move_file(src, dst);
        out.sst[f] = dst.string();
        sst[f].reset();
    }
    return out;
}

IndexDatabaseSstWriterContext::IndexDatabaseSstWriterContext(
    std::string staging_dir, std::string batch_id)
    : staging_dir_(std::move(staging_dir)), batch_id_(std::move(batch_id)) {
    std::error_code ec;
    fs::create_directories(fs::path(staging_dir_) / batch_id_, ec);
    if (ec) {
        throw IndexerError(IndexerError::Type::FILE_ERROR,
                           "Failed to create SST staging dir '" + staging_dir_ +
                               "/" + batch_id_ + "': " + ec.message());
    }
}

IndexDatabaseSstWriterContext::IndexDatabaseSstWriterContext(
    IndexDatabaseSstWriterContext&&) noexcept = default;
IndexDatabaseSstWriterContext& IndexDatabaseSstWriterContext::operator=(
    IndexDatabaseSstWriterContext&&) noexcept = default;
IndexDatabaseSstWriterContext::~IndexDatabaseSstWriterContext() = default;

void IndexDatabaseSstWriterContext::put(layout::Family family,
                                        std::string_view key,
                                        std::string_view value) {
    buffers_[static_cast<std::size_t>(family)].entries.push_back(
        {std::string(key), std::string(value), false});
}

void IndexDatabaseSstWriterContext::merge(layout::Family family,
                                          std::string_view key,
                                          std::string_view operand) {
    buffers_[static_cast<std::size_t>(family)].entries.push_back(
        {std::string(key), std::string(operand), true});
}

void IndexDatabaseSstWriterContext::delete_range(layout::Family family,
                                                 std::string_view begin,
                                                 std::string_view end) {
    buffers_[static_cast<std::size_t>(family)].ranges.emplace_back(begin, end);
}

IndexDatabaseSstWriterContext::Artifacts
IndexDatabaseSstWriterContext::commit() {
    Artifacts out;
    if (committed_) return out;
    committed_ = true;

    const auto batch_dir = fs::path(staging_dir_) / batch_id_;
    index::schemas::dft::agg::AggregationMergeOperator agg_merge_op;
    index::schemas::dft::agg::SystemMetricsMergeOperator sys_merge_op;
    for (std::size_t f = 0; f < layout::FAMILY_COUNT; ++f) {
        auto& buf = buffers_[f];
        if (buf.entries.empty() && buf.ranges.empty()) continue;
        const auto family = static_cast<layout::Family>(f);
        const ::rocksdb::MergeOperator* merge_op = nullptr;
        if (family == layout::Family::AGGREGATION) merge_op = &agg_merge_op;
        if (family == layout::Family::SYSTEM_METRICS) merge_op = &sys_merge_op;
        out.sst[f] = emit_sst(
            (batch_dir / (std::string(layout::family_name(family)) + ".sst"))
                .string(),
            buf.entries, buf.ranges, merge_op);
        buf = {};
    }
    return out;
}

}  // namespace dftracer::utils::index::store
