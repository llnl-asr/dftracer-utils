#include <dftracer/utils/utilities/composites/dft/event.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/composites/dft/visitors/manifest_visitor.h>
#include <dftracer/utils/utilities/indexer/index_batch_sink.h>

namespace dftracer::utils::utilities::composites::dft::visitors {

void ManifestVisitor::begin(std::size_t /*num_checkpoints*/) {
    event_lines_.clear();
    metadata_lines_.clear();
    observed_pids_.clear();
    event_count_ = 0;
    line_offset_ = 0;
    base_idx_ = 0;
}

void ManifestVisitor::on_checkpoint(std::size_t /*checkpoint_idx*/) {}

void ManifestVisitor::ensure_chunk(std::size_t checkpoint_idx) {
    if (checkpoint_idx < base_idx_) return;
    const std::size_t local = checkpoint_idx - base_idx_;
    if (local < event_lines_.size()) return;
    event_lines_.resize(local + 1);
    metadata_lines_.resize(local + 1);
}

void ManifestVisitor::on_event(const EventRecord& record) {
    if (record.checkpoint_idx < base_idx_) return;
    auto ln = static_cast<std::uint32_t>(record.line_number);
    ensure_chunk(record.checkpoint_idx);
    ++event_count_;

    const auto local = record.checkpoint_idx - base_idx_;
    const auto& ev = record.ev;
    if (ev.is_metadata()) {
        std::string name(ev.name);
        if (!name.empty()) {
            metadata_lines_[local][name].push_back(ln);
        }
    } else {
        std::string cat(ev.cat);
        std::string name(ev.name);
        event_lines_[local][{cat, name}].push_back(ln);
        observed_pids_.insert(ev.pid);
    }
}

std::unique_ptr<DftEventVisitor> ManifestVisitor::create_parallel_slice()
    const {
    return std::make_unique<ManifestVisitor>();
}

void ManifestVisitor::merge_parallel_slice(DftEventVisitor& slice_base) {
    auto* slice = dynamic_cast<ManifestVisitor*>(&slice_base);
    if (!slice) return;
    const auto offset = static_cast<std::uint32_t>(slice->line_offset_);

    auto map_ci = [this](std::size_t slice_ci) -> std::size_t {
        return slice_ci - base_idx_;
    };
    for (std::size_t slice_ci = base_idx_;
         slice_ci < slice->event_lines_.size(); ++slice_ci) {
        if (slice->event_lines_[slice_ci].empty()) continue;
        const std::size_t parent_local = map_ci(slice_ci);
        if (parent_local >= event_lines_.size()) {
            event_lines_.resize(parent_local + 1);
        }
        for (auto& [key, lines] : slice->event_lines_[slice_ci]) {
            auto& dst = event_lines_[parent_local][key];
            dst.reserve(dst.size() + lines.size());
            for (auto ln : lines) dst.push_back(ln + offset);
        }
    }
    for (std::size_t slice_ci = base_idx_;
         slice_ci < slice->metadata_lines_.size(); ++slice_ci) {
        if (slice->metadata_lines_[slice_ci].empty()) continue;
        const std::size_t parent_local = map_ci(slice_ci);
        if (parent_local >= metadata_lines_.size()) {
            metadata_lines_.resize(parent_local + 1);
        }
        for (auto& [meta_type, lines] : slice->metadata_lines_[slice_ci]) {
            auto& dst = metadata_lines_[parent_local][meta_type];
            dst.reserve(dst.size() + lines.size());
            for (auto ln : lines) dst.push_back(ln + offset);
        }
    }
    for (auto pid : slice->observed_pids_) observed_pids_.insert(pid);
    event_count_ += slice->event_count_;
}

void ManifestVisitor::finalize(indexer::IndexBatchSink& db, int file_id) {
    flush_per_checkpoint_to_sink(db, file_id);
    finalize_file_to_sink(db, file_id);
}

void ManifestVisitor::flush_per_checkpoint_to_sink(
    indexer::IndexBatchSink& sink, int file_id) {
    const std::size_t n = std::max(event_lines_.size(), metadata_lines_.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto ci = static_cast<std::uint64_t>(base_idx_ + i);
        if (i < event_lines_.size()) {
            for (auto& [key, lines] : event_lines_[i]) {
                if (lines.empty()) continue;
                sink.insert_event_range(file_id, ci, key.first, key.second,
                                        lines);
            }
        }
        if (i < metadata_lines_.size()) {
            for (auto& [meta_type, lines] : metadata_lines_[i]) {
                if (lines.empty()) continue;
                sink.insert_metadata_lines(file_id, ci, meta_type, lines);
            }
        }
    }
    base_idx_ += n;
    event_lines_.clear();
    metadata_lines_.clear();
}

void ManifestVisitor::finalize_file_to_sink(indexer::IndexBatchSink& sink,
                                            int file_id) {
    flush_per_checkpoint_to_sink(sink, file_id);
    if (!observed_pids_.empty()) {
        sink.insert_file_pids(file_id, observed_pids_);
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::visitors
