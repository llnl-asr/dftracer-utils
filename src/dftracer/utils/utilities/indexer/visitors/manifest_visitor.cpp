#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/visitors/manifest_visitor.h>
#include <yyjson.h>

using dftracer::utils::utilities::common::json::JsonValue;
namespace queries =
    dftracer::utils::utilities::composites::dft::indexing::queries;

namespace dftracer::utils::utilities::indexer {

void ManifestVisitor::begin(std::size_t /*num_checkpoints*/) {
    event_lines_.clear();
    metadata_lines_.clear();
    chunk_line_ = 0;
}

void ManifestVisitor::on_checkpoint(std::size_t /*checkpoint_idx*/) {
    chunk_line_ = 0;
}

void ManifestVisitor::ensure_chunk(std::size_t checkpoint_idx) {
    if (checkpoint_idx < event_lines_.size()) return;
    event_lines_.resize(checkpoint_idx + 1);
    metadata_lines_.resize(checkpoint_idx + 1);
}

void ManifestVisitor::on_line(std::string_view line,
                              std::size_t checkpoint_idx) {
    std::uint32_t ln = chunk_line_++;

    if (line.empty()) return;
    ensure_chunk(checkpoint_idx);

    yyjson_doc* doc = yyjson_read(line.data(), line.size(), YYJSON_READ_NOFLAG);
    if (!doc) return;

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (root && yyjson_is_obj(root)) {
        JsonValue json(root);
        std::string_view ph = json["ph"].get<std::string_view>();

        if (ph == "M") {
            std::string name = json["name"].get<std::string>();
            if (!name.empty()) {
                metadata_lines_[checkpoint_idx][name].push_back(ln);
            }
        } else {
            std::string cat = json["cat"].get<std::string>();
            std::string name = json["name"].get<std::string>();
            event_lines_[checkpoint_idx][{cat, name}].push_back(ln);
        }
    }

    yyjson_doc_free(doc);
}

void ManifestVisitor::finalize(IndexDatabase& db, int file_id) {
    for (std::size_t ci = 0; ci < event_lines_.size(); ++ci) {
        for (auto& [key, lines] : event_lines_[ci]) {
            db.insert_event_range(file_id, static_cast<std::uint64_t>(ci),
                                  key.first, key.second, lines);
        }

        for (auto& [meta_type, lines] : metadata_lines_[ci]) {
            db.insert_metadata_lines(file_id, static_cast<std::uint64_t>(ci),
                                     meta_type, lines);
        }
    }
}

}  // namespace dftracer::utils::utilities::indexer
