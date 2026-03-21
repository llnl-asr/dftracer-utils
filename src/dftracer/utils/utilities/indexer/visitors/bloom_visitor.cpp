#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/visitors/bloom_visitor.h>
#include <yyjson.h>

#include <string>

using dftracer::utils::utilities::common::json::JsonValue;
using dftracer::utils::utilities::composites::dft::indexing::BloomFilter;
namespace queries =
    dftracer::utils::utilities::composites::dft::indexing::queries;

namespace dftracer::utils::utilities::indexer {

namespace {

static const std::string DIM_NAME = "name";
static const std::string DIM_CAT = "cat";
static const std::string DIM_PID = "pid";
static const std::string DIM_TID = "tid";
static const std::string DIM_PID_TID = "pid_tid";
static const std::string DIM_HHASH = "hhash";
static const std::string DIM_FHASH = "fhash";
static const std::string DIM_SHASH = "shash";

std::string json_value_to_string(const JsonValue& val) {
    if (val.is_string()) return val.get<std::string>();
    if (val.is_uint()) return std::to_string(val.get<std::uint64_t>());
    if (val.is_int()) return std::to_string(val.get<std::int64_t>());
    if (val.is_number()) return std::to_string(val.get<double>());
    if (val.is_bool()) return val.get<bool>() ? "true" : "false";
    return {};
}

}  // namespace

BloomVisitor::BloomVisitor(ChunkIndexerConfig config,
                           std::vector<std::string> dimensions)
    : config_(std::move(config)), dimensions_(std::move(dimensions)) {}

void BloomVisitor::begin(std::size_t /*num_checkpoints*/) { chunks_.clear(); }

void BloomVisitor::on_checkpoint(std::size_t /*checkpoint_idx*/) {}

void BloomVisitor::ensure_chunk(std::size_t checkpoint_idx) {
    if (checkpoint_idx < chunks_.size()) return;
    chunks_.resize(checkpoint_idx + 1);
    for (auto& chunk : chunks_) {
        if (chunk.bloom_filters.empty()) {
            for (const auto& dim : dimensions_) {
                chunk.bloom_filters.emplace(
                    dim, BloomFilter(config_.expected_entries_per_chunk,
                                     config_.false_positive_rate));
            }
        }
        if (chunk.dimension_stats.empty()) {
            for (const auto& dim : dimensions_) {
                auto& ds = chunk.dimension_stats[dim];
                ds.dimension = dim;
                if (dim == DIM_PID || dim == DIM_TID) {
                    ds.value_type = "uint";
                } else {
                    ds.value_type = "string";
                }
            }
            auto& pt = chunk.dimension_stats[DIM_PID_TID];
            pt.dimension = DIM_PID_TID;
            pt.value_type = "string";
        }
    }
}

void BloomVisitor::on_line(std::string_view line, std::size_t checkpoint_idx) {
    if (line.empty()) return;
    ensure_chunk(checkpoint_idx);

    ChunkState& chunk = chunks_[checkpoint_idx];

    yyjson_doc* doc =
        yyjson_read_opts(const_cast<char*>(line.data()), line.size(),
                         YYJSON_READ_NOFLAG, nullptr, nullptr);
    if (!doc) return;

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return;
    }

    JsonValue json(root);
    std::string_view ph = json["ph"].get<std::string_view>();

    if (ph == "M") {
        std::string_view name_sv = json["name"].get<std::string_view>();
        JsonValue args = json["args"];

        if (args.exists()) {
            std::string hash_val = args["value"].get<std::string>();
            std::string resolved = args["name"].get<std::string>();

            if (!hash_val.empty() && !resolved.empty()) {
                if (name_sv == "HH") {
                    chunk.hash_resolutions[DIM_HHASH][hash_val] = resolved;
                } else if (name_sv == "FH") {
                    chunk.hash_resolutions[DIM_FHASH][hash_val] = resolved;
                } else if (name_sv == "SH") {
                    chunk.hash_resolutions[DIM_SHASH][hash_val] = resolved;
                }
            }
        }
    } else {
        std::string_view name_sv = json["name"].get<std::string_view>();
        std::string_view cat_sv = json["cat"].get<std::string_view>();
        std::uint64_t pid = json["pid"].get<std::uint64_t>();
        std::uint64_t tid = json["tid"].get<std::uint64_t>();
        std::uint64_t ts = json["ts"].get<std::uint64_t>();
        std::uint64_t dur = json["dur"].get<std::uint64_t>();

        chunk.statistics.update_from_event(name_sv, cat_sv, pid, tid, ts, dur);

        // Helper: add to bloom filter and observe dimension stats
        auto observe = [&chunk](const std::string& dim, std::string_view val) {
            if (val.empty()) return;
            auto bf_it = chunk.bloom_filters.find(dim);
            if (bf_it != chunk.bloom_filters.end()) {
                bf_it->second.add(val);
            }
            auto ds_it = chunk.dimension_stats.find(dim);
            if (ds_it != chunk.dimension_stats.end()) {
                ds_it->second.observe(val);
            }
        };

        observe(DIM_NAME, name_sv);
        observe(DIM_CAT, cat_sv);

        auto pid_str = std::to_string(pid);
        auto tid_str = std::to_string(tid);
        observe(DIM_PID, pid_str);
        observe(DIM_TID, tid_str);

        auto pid_tid_str = pid_str + ":" + tid_str;
        observe(DIM_PID_TID, pid_tid_str);

        JsonValue args = json["args"];
        if (args.exists()) {
            std::string_view hhash = args["hhash"].get<std::string_view>();
            observe(DIM_HHASH, hhash);

            std::string_view fhash = args["fhash"].get<std::string_view>();
            observe(DIM_FHASH, fhash);

            std::string_view shash = args["cmd_hash"].get<std::string_view>();
            if (shash.empty()) {
                shash = args["exec_hash"].get<std::string_view>();
            }
            observe(DIM_SHASH, shash);

            for (const auto& dim : config_.extra_dimensions) {
                JsonValue val = args.at(dim.c_str());
                if (val.exists()) {
                    std::string str_val = json_value_to_string(val);
                    observe(dim, str_val);
                }
            }
        }

        chunk.events_processed++;
    }

    yyjson_doc_free(doc);
}

void BloomVisitor::finalize(IndexDatabase& db, int file_id) {
    auto& sql_db = db.sql_db();

    std::unordered_map<std::string, BloomFilter> file_blooms;
    for (const auto& dim : dimensions_) {
        file_blooms.emplace(dim, BloomFilter(config_.expected_entries_per_chunk,
                                             config_.false_positive_rate));
    }

    for (std::size_t i = 0; i < chunks_.size(); ++i) {
        ChunkState& chunk = chunks_[i];
        auto checkpoint_idx = static_cast<std::uint64_t>(i);

        for (const auto& dim : dimensions_) {
            auto it = chunk.bloom_filters.find(dim);
            if (it == chunk.bloom_filters.end()) continue;

            const BloomFilter& bf = it->second;
            auto blob = bf.serialize();
            queries::insert_chunk_bloom_filter(
                sql_db, file_id, checkpoint_idx, dim, blob.data(),
                static_cast<int>(blob.size()),
                static_cast<std::uint64_t>(bf.num_entries()));

            file_blooms.at(dim).merge_from(bf);
        }

        queries::insert_chunk_statistics(sql_db, file_id, checkpoint_idx,
                                         chunk.statistics);

        for (const auto& [dim, ds] : chunk.dimension_stats) {
            queries::insert_chunk_dimension_stats(
                sql_db, file_id, checkpoint_idx, ds, config_.value_counts_cap);
        }

        for (const auto& [dim, resolutions] : chunk.hash_resolutions) {
            for (const auto& [hash_val, resolved] : resolutions) {
                queries::insert_hash_resolution(sql_db, file_id, dim, hash_val,
                                                resolved);
            }
        }
    }

    for (const auto& dim : dimensions_) {
        const BloomFilter& bf = file_blooms.at(dim);
        auto blob = bf.serialize();
        queries::insert_file_bloom_filter(
            sql_db, file_id, dim, blob.data(), static_cast<int>(blob.size()),
            static_cast<std::uint64_t>(bf.num_entries()));
    }

    for (const auto& dim : dimensions_) {
        queries::insert_index_dimension(sql_db, file_id, dim);
    }
}

}  // namespace dftracer::utils::utilities::indexer
