#include <dftracer/utils/utilities/composites/dft/reorganize/organize_visitor.h>

namespace dftracer::utils::utilities::composites::dft::reorganize {

OrganizeVisitor::OrganizeVisitor(OrganizeVisitorConfig config)
    : config_(std::move(config)) {
    parsed_queries_.reserve(config_.groups.size());
    for (const auto& group : config_.groups) {
        if (group.query.empty()) {
            parsed_queries_.push_back(std::nullopt);
        } else {
            auto result = common::query::Query::from_string(group.query);
            if (result) {
                parsed_queries_.push_back(std::move(*result));
            } else {
                parsed_queries_.push_back(std::nullopt);
            }
        }
    }

    pending_batches_.resize(config_.groups.size());
    for (auto& batch : pending_batches_) {
        batch.reserve(config_.batch_size);
    }
    drain_queue_.resize(config_.groups.size());
}

void OrganizeVisitor::begin(std::size_t /*num_checkpoints*/) {
    for (auto& batch : pending_batches_) {
        batch.clear();
    }
    for (auto& q : drain_queue_) {
        q.clear();
    }
    events_routed_ = 0;
    events_unmatched_ = 0;
}

void OrganizeVisitor::on_checkpoint(std::size_t checkpoint_idx) {
    current_checkpoint_ = checkpoint_idx;
}

std::size_t OrganizeVisitor::evaluate_event(
    const DFTracerEvent& /*ev*/, const common::json::JsonValue& json) {
    for (std::size_t i = 0; i < parsed_queries_.size(); ++i) {
        const auto& query_opt = parsed_queries_[i];
        if (!query_opt) {
            return i;
        }
        if (query_opt->evaluate(json)) {
            return i;
        }
    }
    return SIZE_MAX;
}

void OrganizeVisitor::on_event(const EventRecord& record) {
    if (record.ev.is_metadata()) {
        return;
    }

    std::size_t group_idx = evaluate_event(record.ev, record.json);
    if (group_idx == SIZE_MAX) {
        events_unmatched_++;
        return;
    }

    auto& batch = pending_batches_[group_idx];
    batch.append_line(record.line, config_.source_file_idx,
                      record.checkpoint_idx, record.line_number);

    events_routed_++;
}

bool OrganizeVisitor::wants_drain() const noexcept {
    for (std::size_t i = 0; i < pending_batches_.size(); ++i) {
        if (!drain_queue_[i].empty()) return true;
        if (pending_batches_[i].size() >= config_.batch_size) return true;
    }
    return false;
}

coro::CoroTask<void> OrganizeVisitor::drain_pending() {
    for (std::size_t i = 0; i < pending_batches_.size(); ++i) {
        auto& channel = config_.group_channels[i];
        // Send queued slice batches first.
        for (auto& shared_batch : drain_queue_[i]) {
            if (channel) {
                co_await channel->send(std::move(shared_batch));
            }
        }
        drain_queue_[i].clear();
        // Then drain the threshold-triggered current batch.
        auto& batch = pending_batches_[i];
        if (batch.size() < config_.batch_size) continue;
        if (channel) {
            co_await channel->send(
                std::make_shared<LineBatch>(std::move(batch)));
        }
        batch.clear();
        batch.reserve(config_.batch_size);
    }
}

coro::CoroTask<void> OrganizeVisitor::on_file_complete() {
    for (std::size_t i = 0; i < pending_batches_.size(); ++i) {
        auto& channel = config_.group_channels[i];
        for (auto& shared_batch : drain_queue_[i]) {
            if (channel) {
                co_await channel->send(std::move(shared_batch));
            }
        }
        drain_queue_[i].clear();
        auto& batch = pending_batches_[i];
        if (batch.empty()) continue;
        if (channel) {
            co_await channel->send(
                std::make_shared<LineBatch>(std::move(batch)));
        }
        batch.clear();
        batch.reserve(config_.batch_size);
    }
}

std::unique_ptr<DftEventVisitor> OrganizeVisitor::create_parallel_slice()
    const {
    return std::make_unique<OrganizeVisitor>(config_);
}

void OrganizeVisitor::merge_parallel_slice(DftEventVisitor& slice_base) {
    auto* slice = dynamic_cast<OrganizeVisitor*>(&slice_base);
    if (!slice) return;
    for (std::size_t i = 0;
         i < drain_queue_.size() && i < slice->pending_batches_.size(); ++i) {
        auto& src = slice->pending_batches_[i];
        if (src.empty()) continue;
        drain_queue_[i].push_back(std::make_shared<LineBatch>(std::move(src)));
        src.clear();
        src.reserve(config_.batch_size);
    }
    events_routed_ += slice->events_routed_;
    events_unmatched_ += slice->events_unmatched_;
}

}  // namespace dftracer::utils::utilities::composites::dft::reorganize
