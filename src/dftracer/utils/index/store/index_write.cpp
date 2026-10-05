#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/internal/index_encoding.h>
#include <dftracer/utils/index/store/internal/statistics_codec.h>

namespace dftracer::utils::index::store::records {

namespace enc = internal::encoding;
using layout::Ext;
namespace sk = layout::stats_kind;
using layout::Family;

namespace {

void put_value(IndexWrite& w, Ext ext, std::uint8_t kind, std::string_view key,
               std::string_view body) {
    w.put(layout::family_of(ext, kind), key,
          layout::with_header(ext, kind, body));
}

std::uint32_t fid(int file_id) { return static_cast<std::uint32_t>(file_id); }

std::uint32_t granule(std::uint64_t checkpoint_idx) {
    return static_cast<std::uint32_t>(checkpoint_idx);
}

// Removes exactly `key`: the range [key, key + "\0") holds no other key.
void delete_key(IndexWrite& w, layout::Family family, std::string key) {
    std::string end = key;
    end.push_back('\0');
    w.delete_range(family, key, end);
}

}  // namespace

void clear_file(IndexWrite& w, Ext ext, int file_id) {
    const auto manifest = layout::manifest_key(fid(file_id), ext);
    w.delete_range(layout::Family::REGISTRY, manifest,
                   layout::prefix_end(manifest));
    for (const auto& k : layout::kinds()) {
        if (k.ext != ext) continue;
        const auto begin = layout::file_prefix(ext, k.kind, fid(file_id));
        w.delete_range(k.family, begin, layout::prefix_end(begin));
    }
}

void clear_path(IndexWrite& w, Ext ext, int file_id, std::string_view path) {
    for (const auto& k : layout::kinds()) {
        if (k.ext != ext || k.kind == layout::path_kind::PATHS) continue;
        if (k.kind == layout::path_kind::FILE && ext != Ext::POSTINGS) {
            auto key = layout::file_prefix(ext, k.kind, fid(file_id));
            key.append(path);
            delete_key(w, k.family, std::move(key));
            continue;
        }
        const auto begin = layout::path_prefix(ext, k.kind, fid(file_id), path);
        w.delete_range(k.family, begin, layout::prefix_end(begin));
    }
}

void put_manifest(IndexWrite& w, int file_id, Ext ext,
                  std::uint64_t params_hash, layout::ExtStatus status) {
    w.put(layout::Family::REGISTRY, layout::manifest_key(fid(file_id), ext),
          layout::encode_manifest(
              {layout::ext_version(ext), params_hash, status}));
}

void put_plugin_manifest(IndexWrite& w, int file_id, std::string_view name,
                         std::uint32_t version, std::uint64_t params_hash,
                         layout::ExtStatus status) {
    w.put(layout::Family::REGISTRY,
          layout::plugin_manifest_key(fid(file_id), name),
          layout::encode_manifest({version, params_hash, status}));
}

void clear_plugin(IndexWrite& w, int file_id, std::string_view name) {
    const auto manifest = layout::plugin_manifest_key(fid(file_id), name);
    w.delete_range(layout::Family::REGISTRY, manifest,
                   layout::prefix_end(manifest));
    clear_path(w, Ext::PLUGIN, file_id, name);
    auto listed = layout::file_prefix(Ext::PLUGIN, layout::path_kind::PATHS,
                                      fid(file_id));
    listed.append(name);
    delete_key(w, layout::family_of(Ext::PLUGIN, layout::path_kind::PATHS),
               std::move(listed));
}

void put_file_record(IndexWrite& w, std::string_view logical_path,
                     const layout::FileRecord& record) {
    w.put(layout::Family::REGISTRY, layout::file_by_path_key(logical_path),
          layout::encode_file_record(record));
    w.put(
        layout::Family::REGISTRY, layout::file_by_id_key(record.file_id),
        layout::with_header(Ext::HOST, layout::host::FILE_BY_ID, logical_path));
}

void put_file_metadata(IndexWrite& w, int file_id,
                       std::uint64_t checkpoint_size, std::uint64_t total_lines,
                       std::uint64_t total_uc_size, bool truncated) {
    put_value(w, Ext::MEMBERS, layout::members::METADATA,
              layout::file_prefix(Ext::MEMBERS, layout::members::METADATA,
                                  fid(file_id)),
              enc::encode_metadata_record(
                  checkpoint_size, total_lines, total_uc_size,
                  truncated ? enc::METADATA_FLAG_TRUNCATED : 0));
}

void put_gzip_member(IndexWrite& w, int file_id,
                     const index::gzip::GzipMemberRecord& member) {
    put_value(w, Ext::MEMBERS, layout::members::MEMBER,
              layout::granule_key(Ext::MEMBERS, layout::members::MEMBER,
                                  fid(file_id), granule(member.member_idx)),
              enc::encode_gzip_member_value(member));
}

void put_restart_window(IndexWrite& w, int file_id, std::uint64_t member_idx,
                        std::string_view window) {
    put_value(w, Ext::MEMBERS, layout::members::RESTART,
              layout::granule_key(Ext::MEMBERS, layout::members::RESTART,
                                  fid(file_id), granule(member_idx)),
              window);
}

void put_path(IndexWrite& w, Ext ext, int file_id, std::string_view path) {
    auto key = layout::file_prefix(ext, layout::path_kind::PATHS, fid(file_id));
    key.append(path);
    w.put(layout::family_of(ext, layout::path_kind::PATHS), key, {});
}

void put_path_granule(IndexWrite& w, Ext ext, int file_id,
                      std::string_view path, std::uint64_t granule_idx,
                      std::string_view payload) {
    auto key =
        layout::path_prefix(ext, layout::path_kind::DATA, fid(file_id), path);
    layout::append_u32(key, granule(granule_idx));
    put_value(w, ext, layout::path_kind::DATA, key, payload);
}

void put_path_file(IndexWrite& w, Ext ext, int file_id, std::string_view path,
                   std::string_view payload) {
    auto key = layout::file_prefix(ext, layout::path_kind::FILE, fid(file_id));
    key.append(path);
    put_value(w, ext, layout::path_kind::FILE, key, payload);
}

void put_posting(IndexWrite& w, int file_id, std::string_view path,
                 std::uint64_t value_hash) {
    auto key = layout::path_prefix(Ext::POSTINGS, layout::path_kind::IN_FILE,
                                   fid(file_id), path);
    layout::append_u64(key, value_hash);
    w.put(Family::POSTINGS, key, {});
}

void put_posting_granule(IndexWrite& w, int file_id, std::string_view path,
                         std::uint64_t value_hash, std::uint64_t granule_idx) {
    auto key = layout::path_prefix(Ext::POSTINGS, layout::path_kind::IN_GRANULE,
                                   fid(file_id), path);
    layout::append_u64(key, value_hash);
    layout::append_u32(key, granule(granule_idx));
    w.put(Family::POSTINGS, key, {});
}

void put_chunk_statistics(IndexWrite& w, int file_id,
                          std::uint64_t checkpoint_idx,
                          const index::schemas::dft::ChunkStatistics& stats) {
    put_value(w, Ext::STATS, sk::CHUNK_STATS,
              layout::granule_key(Ext::STATS, sk::CHUNK_STATS, fid(file_id),
                                  granule(checkpoint_idx)),
              enc::encode_chunk_statistics_value(stats));
}

void put_chunk_metadata(IndexWrite& w, int file_id,
                        std::uint64_t checkpoint_idx,
                        const ChunkMetadata& metadata) {
    put_value(w, Ext::METADATA, layout::metadata_kind::CHUNK,
              layout::granule_key(Ext::METADATA, layout::metadata_kind::CHUNK,
                                  fid(file_id), granule(checkpoint_idx)),
              layout::encode_chunk_metadata(metadata));
}

void put_file_scalar_stats(IndexWrite& w, int file_id,
                           const index::schemas::dft::ChunkStatistics& stats,
                           std::uint64_t num_chunks) {
    put_value(
        w, Ext::STATS, sk::FILE_SCALAR_STATS,
        layout::file_prefix(Ext::STATS, sk::FILE_SCALAR_STATS, fid(file_id)),
        internal::encode_file_scalar_stats_value(stats, num_chunks));
}

void put_file_pid_tid_counts(IndexWrite& w, int file_id,
                             const StringViewMap<std::uint64_t>& counts) {
    put_value(
        w, Ext::STATS, sk::FILE_PID_TID_COUNTS,
        layout::file_prefix(Ext::STATS, sk::FILE_PID_TID_COUNTS, fid(file_id)),
        enc::encode_count_map_value(counts));
}

void put_profile(IndexWrite& w, int file_id, std::string_view profile_id,
                 std::uint64_t params_hash) {
    put_value(w, Ext::PROFILE, layout::profile_kind::ID,
              layout::file_prefix(Ext::PROFILE, layout::profile_kind::ID,
                                  fid(file_id)),
              profile_id);
    put_manifest(w, file_id, Ext::PROFILE, params_hash);
}

void put_catalog_path(IndexWrite& w, int file_id, std::string_view path,
                      const PathStat& stat) {
    auto key = layout::file_prefix(Ext::CATALOG, layout::catalog_kind::PATH,
                                   fid(file_id));
    key.append(path);
    put_value(w, Ext::CATALOG, layout::catalog_kind::PATH, key,
              layout::encode_path_stat(stat));
}

void put_rowset(IndexWrite& w, int file_id, std::string_view name,
                std::string_view frame) {
    put_value(w, Ext::ROWSET, layout::rowset_kind::FRAME,
              layout::path_prefix(Ext::ROWSET, layout::rowset_kind::FRAME,
                                  fid(file_id), name),
              frame);
}

}  // namespace dftracer::utils::index::store::records
