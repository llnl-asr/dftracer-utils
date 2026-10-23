#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/index/schemas/dft/agg/system_metrics_merge_operator.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/error.h>
#include <dftracer/utils/index/store/index_database_sst_writer_context.h>
#include <dftracer/utils/index/store/internal/db_error.h>
#include <rocksdb/sst_file_writer.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace dftracer::utils::index::store {

namespace {

using Buffer = IndexDatabaseSstWriterContext::Buffer;
using Op = IndexDatabaseSstWriterContext::Op;

constexpr std::size_t GROUP_PREFIX_BYTES = 3;

std::string_view key_of(const Buffer& buf,
                        const IndexDatabaseSstWriterContext::Entry& e) {
    return {buf.bytes.data() + e.offset, e.key_size};
}

std::string_view value_of(const Buffer& buf,
                          const IndexDatabaseSstWriterContext::Entry& e) {
    return {buf.bytes.data() + e.offset + e.key_size, e.value_size};
}

std::string_view range_begin(const Buffer& buf,
                             const IndexDatabaseSstWriterContext::Range& r) {
    return {buf.bytes.data() + r.offset, r.begin_size};
}

std::string_view range_end(const Buffer& buf,
                           const IndexDatabaseSstWriterContext::Range& r) {
    return {buf.bytes.data() + r.offset + r.begin_size, r.end_size};
}

::rocksdb::Slice slice(std::string_view v) { return {v.data(), v.size()}; }

// Entries [lo, hi) of a sorted buffer, one entry per key: a run of same-key
// merge operands is combined with `merge_op` (PartialMerge is associative, so
// reads see the same result as separate operands); any other run keeps its
// last entry. `ranges` are the range deletes (indices into buf.ranges).
std::string emit_sst(const std::string& path, const Buffer& buf, std::size_t lo,
                     std::size_t hi, const dstd::Vector<std::size_t>& ranges,
                     const ::rocksdb::MergeOperator* merge_op) {
    ::rocksdb::EnvOptions env_opts;
    ::rocksdb::Options writer_options(
        index::store::RocksDatabase::default_options(),
        index::store::RocksDatabase::default_column_family_options());
    // SstFileWriter takes the bottommost codec when one is set. An ingested
    // SST is compacted again, so it gets the fast upper-level codec.
    writer_options.bottommost_compression =
        ::rocksdb::kDisableCompressionOption;
    ::rocksdb::SstFileWriter writer(env_opts, writer_options);

    auto status = writer.Open(path);
    if (!status.ok())
        internal::throw_db_error("Failed to open SST writer at '" + path + "'",
                                 status);
    for (const std::size_t r : ranges) {
        status = writer.DeleteRange(slice(range_begin(buf, buf.ranges[r])),
                                    slice(range_end(buf, buf.ranges[r])));
        if (!status.ok())
            internal::throw_db_error(
                "Failed to add a range delete to SST '" + path + "'", status);
    }
    std::size_t i = lo;
    while (i < hi) {
        const std::string_view key = key_of(buf, buf.entries[i]);
        std::size_t j = i + 1;
        while (j < hi && key_of(buf, buf.entries[j]) == key) ++j;
        if (buf.entries[i].op == Op::Merge && j - i > 1 && merge_op) {
            std::string combined(value_of(buf, buf.entries[i]));
            for (std::size_t k = i + 1; k < j; ++k) {
                std::string next;
                if (!merge_op->PartialMerge(
                        slice(key), slice(combined),
                        slice(value_of(buf, buf.entries[k])), &next, nullptr))
                    internal::throw_db_error(
                        "PartialMerge failed combining SST operands for '" +
                            path + "'",
                        ::rocksdb::Status::Corruption("PartialMerge"));
                combined = std::move(next);
            }
            status = writer.Merge(slice(key), slice(combined));
        } else {
            const auto& e = buf.entries[j - 1];
            const auto value = slice(value_of(buf, e));
            status = e.op == Op::Merge    ? writer.Merge(slice(key), value)
                     : e.op == Op::Delete ? writer.Delete(slice(key))
                                          : writer.Put(slice(key), value);
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

// The [ext u16][kind u8] a key starts with. SSTs of different batches are
// disjoint in key range only per prefix: one SST holding several prefixes
// spans all of them and overlaps every other batch's.
std::string_view group_of(std::string_view key) {
    return key.substr(0, std::min(key.size(), GROUP_PREFIX_BYTES));
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
        throw IndexerError(IndexerError::Type::FileError,
                           "Failed to move SST '" + src.string() + "' to '" +
                               dst.string() + "': " + ec.message());
    }
    fs::remove(src, ec);  // best-effort; staging cleanup handled by caller
}

}  // namespace

bool IndexDatabaseSstWriterContext::Artifacts::empty() const noexcept {
    for (const auto& paths : sst)
        if (!paths.empty()) return false;
    return true;
}

IndexDatabaseSstWriterContext::Artifacts
IndexDatabaseSstWriterContext::Artifacts::move_to(
    std::string_view dest_dir) && {
    const fs::path dir(dest_dir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        throw IndexerError(IndexerError::Type::FileError,
                           "Failed to create SST move destination '" +
                               std::string(dest_dir) + "': " + ec.message());
    }
    Artifacts out;
    for (std::size_t f = 0; f < sst.size(); ++f) {
        for (const auto& path : sst[f]) {
            const fs::path src(path);
            const fs::path dst = dir / src.filename();
            move_file(src, dst);
            out.sst[f].push_back(dst.string());
        }
        sst[f].clear();
    }
    return out;
}

IndexDatabaseSstWriterContext::IndexDatabaseSstWriterContext(
    std::string staging_dir, std::string batch_id)
    : staging_dir_(std::move(staging_dir)), batch_id_(std::move(batch_id)) {
    std::error_code ec;
    fs::create_directories(fs::path(staging_dir_) / batch_id_, ec);
    if (ec) {
        throw IndexerError(IndexerError::Type::FileError,
                           "Failed to create SST staging dir '" + staging_dir_ +
                               "/" + batch_id_ + "': " + ec.message());
    }
}

IndexDatabaseSstWriterContext::IndexDatabaseSstWriterContext(
    IndexDatabaseSstWriterContext&&) noexcept = default;
IndexDatabaseSstWriterContext& IndexDatabaseSstWriterContext::operator=(
    IndexDatabaseSstWriterContext&&) noexcept = default;
IndexDatabaseSstWriterContext::~IndexDatabaseSstWriterContext() = default;

void IndexDatabaseSstWriterContext::stage(layout::Family family,
                                          std::string_view first,
                                          std::string_view second, Op op) {
    constexpr std::size_t MAX_PART = std::numeric_limits<std::uint32_t>::max();
    if (first.size() > MAX_PART || second.size() > MAX_PART)
        throw IndexerError(IndexerError::Type::FileError,
                           "An SST entry is larger than 4 GiB");
    auto& buf = buffers_[static_cast<std::size_t>(family)];
    const std::size_t before = buf.footprint();
    const std::uint64_t offset = buf.bytes.size();
    buf.bytes.insert(buf.bytes.end(), first.begin(), first.end());
    buf.bytes.insert(buf.bytes.end(), second.begin(), second.end());
    if (op == Op::Range)
        buf.ranges.push_back({offset, static_cast<std::uint32_t>(first.size()),
                              static_cast<std::uint32_t>(second.size())});
    else
        buf.entries.push_back({offset, static_cast<std::uint32_t>(first.size()),
                               static_cast<std::uint32_t>(second.size()), op});
    held_ += buf.footprint() - before;
    set_buffered_bytes(held_);
}

void IndexDatabaseSstWriterContext::put(layout::Family family,
                                        std::string_view key,
                                        std::string_view value) {
    stage(family, key, value, Op::Put);
}

void IndexDatabaseSstWriterContext::merge(layout::Family family,
                                          std::string_view key,
                                          std::string_view operand) {
    stage(family, key, operand, Op::Merge);
}

void IndexDatabaseSstWriterContext::delete_range(layout::Family family,
                                                 std::string_view begin,
                                                 std::string_view end) {
    stage(family, begin, end, Op::Range);
}

void IndexDatabaseSstWriterContext::del(layout::Family family,
                                        std::string_view key) {
    stage(family, key, {}, Op::Delete);
}

IndexDatabaseSstWriterContext::Artifacts
IndexDatabaseSstWriterContext::commit() {
    Artifacts out;
    if (committed_) return out;
    committed_ = true;

    const auto batch_dir = fs::path(staging_dir_) / batch_id_;
    index::schemas::dft::agg::SystemMetricsMergeOperator sys_merge_op;
    for (std::size_t f = 0; f < layout::FAMILY_COUNT; ++f) {
        auto& buf = buffers_[f];
        if (buf.entries.empty() && buf.ranges.empty()) continue;
        const auto family = static_cast<layout::Family>(f);
        const ::rocksdb::MergeOperator* merge_op = nullptr;
        if (family == layout::Family::SystemMetrics) merge_op = &sys_merge_op;
        std::stable_sort(buf.entries.begin(), buf.entries.end(),
                         [&buf](const auto& a, const auto& b) {
                             return key_of(buf, a) < key_of(buf, b);
                         });
        std::stable_sort(buf.ranges.begin(), buf.ranges.end(),
                         [&buf](const auto& a, const auto& b) {
                             const auto ab = range_begin(buf, a);
                             const auto bb = range_begin(buf, b);
                             return ab != bb
                                        ? ab < bb
                                        : range_end(buf, a) < range_end(buf, b);
                         });

        // One SST per key prefix, in key order; a range delete goes with the
        // prefix of its start.
        dstd::Vector<std::string_view> prefixes;
        for (const auto& e : buf.entries)
            if (prefixes.empty() || group_of(key_of(buf, e)) != prefixes.back())
                prefixes.push_back(group_of(key_of(buf, e)));
        for (const auto& r : buf.ranges) {
            const auto begin = group_of(range_begin(buf, r));
            const auto it =
                std::lower_bound(prefixes.begin(), prefixes.end(), begin);
            if (it == prefixes.end() || *it != begin)
                prefixes.insert(it, begin);
        }
        const std::string family_name(layout::family_name(family));
        std::size_t lo = 0;
        for (std::size_t g = 0; g < prefixes.size(); ++g) {
            std::size_t hi = lo;
            while (hi < buf.entries.size() &&
                   group_of(key_of(buf, buf.entries[hi])) == prefixes[g])
                ++hi;
            dstd::Vector<std::size_t> ranges;
            for (std::size_t r = 0; r < buf.ranges.size(); ++r)
                if (group_of(range_begin(buf, buf.ranges[r])) == prefixes[g])
                    ranges.push_back(r);
            out.sst[f].push_back(emit_sst(
                (batch_dir / (family_name + "_" + std::to_string(g) + ".sst"))
                    .string(),
                buf, lo, hi, ranges, merge_op));
            lo = hi;
        }
        held_ -= buf.footprint();
        buf = {};
    }
    set_buffered_bytes(held_);
    return out;
}

}  // namespace dftracer::utils::index::store
