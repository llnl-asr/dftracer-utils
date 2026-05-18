#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_ORGANIZE_VISITOR_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_ORGANIZE_VISITOR_H

#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/dft_event_visitor.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reorganization_planner.h>
#include <dftracer/utils/utilities/indexer/index_visitor.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::reorganize {

struct LineRecord {
    std::uint32_t offset;
    std::uint32_t length;
    std::size_t source_file_idx;
    std::size_t checkpoint_idx;
    std::size_t source_line_number;
};

struct LineBatch {
    std::string bytes;
    std::vector<LineRecord> lines;

    void reserve(std::size_t n) {
        lines.reserve(n);
        bytes.reserve(n * 256);
    }
    std::size_t size() const { return lines.size(); }
    bool empty() const { return lines.empty(); }
    void clear() {
        lines.clear();
        bytes.clear();
    }

    std::string_view line_view(std::size_t i) const {
        const auto& r = lines[i];
        return std::string_view(bytes.data() + r.offset, r.length);
    }

    void append_line(std::string_view line, std::size_t source_file_idx,
                     std::size_t checkpoint_idx,
                     std::size_t source_line_number) {
        auto offset = static_cast<std::uint32_t>(bytes.size());
        bytes.append(line.data(), line.size());
        lines.push_back(LineRecord{
            .offset = offset,
            .length = static_cast<std::uint32_t>(line.size()),
            .source_file_idx = source_file_idx,
            .checkpoint_idx = checkpoint_idx,
            .source_line_number = source_line_number,
        });
    }
};

struct OrganizeVisitorConfig {
    std::vector<PredicateGroup> groups;
    std::vector<std::shared_ptr<coro::Channel<std::shared_ptr<LineBatch>>>>
        group_channels;
    std::size_t source_file_idx = 0;
    std::size_t batch_size = 1024;
};

class OrganizeVisitor : public DftEventVisitor {
   public:
    explicit OrganizeVisitor(OrganizeVisitorConfig config);

    void begin(std::size_t num_checkpoints) override;
    void on_checkpoint(std::size_t checkpoint_idx) override;
    void on_event(const EventRecord& record) override;
    bool wants_drain() const noexcept override;
    coro::CoroTask<void> drain_pending() override;
    coro::CoroTask<void> on_file_complete() override;

    std::unique_ptr<DftEventVisitor> create_parallel_slice() const override;
    void merge_parallel_slice(DftEventVisitor& slice) override;

    std::size_t events_routed() const { return events_routed_; }
    std::size_t events_unmatched() const { return events_unmatched_; }

   private:
    std::size_t evaluate_event(const DFTracerEvent& ev,
                               const common::json::JsonValue& json);

    OrganizeVisitorConfig config_;
    std::vector<std::optional<common::query::Query>> parsed_queries_;
    std::vector<LineBatch> pending_batches_;
    /// Full LineBatches queued by `merge_parallel_slice` (move-only, no
    /// byte copy). Drained alongside `pending_batches_` on the next
    /// `drain_pending` / `on_file_complete` call.
    std::vector<std::vector<std::shared_ptr<LineBatch>>> drain_queue_;
    std::size_t current_checkpoint_ = 0;
    std::size_t events_routed_ = 0;
    std::size_t events_unmatched_ = 0;
};

}  // namespace dftracer::utils::utilities::composites::dft::reorganize

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_ORGANIZE_VISITOR_H
