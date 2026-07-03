#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_DFT_EVENT_DISPATCHER_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_DFT_EVENT_DISPATCHER_H

#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/utilities/common/json/json.h>
#include <dftracer/utils/utilities/composites/dft/dft_event_visitor.h>
#include <dftracer/utils/utilities/composites/dft/event.h>
#include <dftracer/utils/utilities/composites/dft/parse_inflated.h>
#include <dftracer/utils/utilities/indexer/index_visitor.h>
#include <simdjson.h>

#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::composites::dft {

class DftEventDispatcher : public indexer::IndexVisitor {
   public:
    using VisitorList = std::vector<std::reference_wrapper<DftEventVisitor>>;

    static constexpr std::size_t FLUSH_THRESHOLD = 4 * 1024 * 1024;  // 4MB

    explicit DftEventDispatcher(VisitorList visitors, bool force_serial = false)
        : visitors_(std::move(visitors)), force_serial_(force_serial) {
        for (auto& v : visitors_) {
            if (v.get().needs_args_map()) {
                needs_args_map_ = true;
                break;
            }
        }
    }

    void begin(std::size_t num_checkpoints) override {
        for (auto& v : visitors_) {
            v.get().begin(num_checkpoints);
        }
    }

    coro::CoroTask<void> on_checkpoint(std::size_t checkpoint_idx) override {
        co_await flush_batch(pending_checkpoint_idx_);
        line_number_ = 0;
        for (auto& v : visitors_) {
            v.get().on_checkpoint(checkpoint_idx);
        }
    }

    coro::CoroTask<void> on_chunk(const char* data, std::size_t len,
                                  std::size_t checkpoint_idx) override {
        if (len == 0) co_return;
        ensure_accum();
        accum_->append(data, len);
        pending_checkpoint_idx_ = checkpoint_idx;
        if (accum_->size() >= FLUSH_THRESHOLD) {
            co_await flush_batch(checkpoint_idx);
        }
    }

    coro::CoroTask<void> flush() override {
        co_await flush_batch(pending_checkpoint_idx_);
    }

    bool wants_drain() const noexcept override {
        for (const auto& v : visitors_) {
            if (v.get().wants_drain()) return true;
        }
        return false;
    }

    coro::CoroTask<void> drain_pending() override {
        for (auto& v : visitors_) {
            if (v.get().wants_drain()) {
                co_await v.get().drain_pending();
            }
        }
    }

    void on_line(std::string_view line, indexer::SharedLineBuffer buffer,
                 std::size_t checkpoint_idx) override {
        std::size_t ln = line_number_++;

        if (line.empty()) return;

        auto result = parser_.parse(line.data(), line.size());
        if (result.error()) return;

        auto root = result.value_unsafe();
        if (!root.is_object()) return;

        common::json::JsonValue json(root);
        DFTracerEvent ev;
        simdjson::dom::element args_dom{};
        bool has_args = false;
        bool ok = false;
        if (needs_args_map_) {
            ok = DFTracerEvent::parse(json, ev, args_dom, has_args);
        } else {
            ok = DFTracerEvent::parse_scalars(root, ev, args_dom, has_args);
        }
        if (ok) {
            EventRecord record{ev, json,     line,    buffer, checkpoint_idx,
                               ln, args_dom, has_args};
            for (auto& v : visitors_) {
                v.get().on_event(record);
            }
        }
    }

    void finalize(indexer::IndexDatabaseWriterContext& writer,
                  int file_id) override {
        (void)writer;
        (void)file_id;
    }

   private:
    void ensure_accum() {
        if (!accum_) {
            accum_ = std::make_shared<std::string>();
            accum_->reserve(FLUSH_THRESHOLD + FLUSH_THRESHOLD / 4);
            if (!partial_doc_.empty()) {
                accum_->append(partial_doc_.data(), partial_doc_.size());
                partial_doc_.clear();
            }
        }
    }

    coro::CoroTask<void> flush_batch(std::size_t checkpoint_idx) {
        if (!accum_ || accum_->empty()) co_return;

        std::size_t total = accum_->size();
        strip_array_delimiters(accum_->data(), total);
        accum_->resize(total + simdjson::SIMDJSON_PADDING, '\0');
        auto chunk_buffer = std::move(accum_);
        accum_ = nullptr;

        std::size_t partial = 0;
        Executor* exec = Executor::current();
        std::size_t num_slices = preferred_slice_count(exec, total);

        if (!force_serial_ && num_slices >= 2 &&
            all_visitors_parallelizable()) {
            partial = co_await parallel_flush(*exec, chunk_buffer, total,
                                              checkpoint_idx, num_slices);
        } else {
            partial = serial_flush(chunk_buffer, total, checkpoint_idx);
        }

        if (partial > 0 && partial <= total) {
            partial_doc_.assign(chunk_buffer->data() + total - partial,
                                chunk_buffer->data() + total);
        }
    }

    bool all_visitors_parallelizable() {
        if (visitor_parallel_clones_cached_) return parallelizable_cached_;
        visitor_parallel_clones_cached_ = true;
        for (auto& v : visitors_) {
            if (!v.get().create_parallel_slice()) {
                parallelizable_cached_ = false;
                return false;
            }
        }
        parallelizable_cached_ = true;
        return true;
    }

    std::size_t preferred_slice_count(Executor* exec, std::size_t total) const {
        if (!exec) return 1;
#ifdef DFTRACER_UTILS_VALGRIND_MODE
        const std::size_t MIN_SLICE_BYTES = 2 * 1024;
#else
        const std::size_t MIN_SLICE_BYTES = 256 * 1024;
#endif
        std::size_t cap = exec->get_num_threads();
        if (cap < 2) return 1;
        if (cap > 8) cap = 8;
        std::size_t by_size = total / MIN_SLICE_BYTES;
        if (by_size < 2) return 1;
        return std::min(by_size, cap);
    }

    std::size_t serial_flush(std::shared_ptr<std::string> chunk_buffer,
                             std::size_t total, std::size_t checkpoint_idx) {
        return parse_buffer(parser_, chunk_buffer, total, checkpoint_idx,
                            line_number_, needs_args_map_,
                            [this](const EventRecord& record) {
                                for (auto& v : visitors_)
                                    v.get().on_event(record);
                            });
    }

    coro::CoroTask<std::size_t> parallel_flush(
        Executor& /*exec*/, std::shared_ptr<std::string> chunk_buffer,
        std::size_t total, std::size_t checkpoint_idx, std::size_t num_slices) {
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        ranges.reserve(num_slices);
        const char* data = chunk_buffer->data();
        std::size_t cursor = 0;
        std::size_t partial_tail = 0;
        for (std::size_t i = 0; i < num_slices; ++i) {
            std::size_t target =
                (i + 1 == num_slices) ? total : (i + 1) * (total / num_slices);
            std::size_t end = target;
            if (i + 1 == num_slices) {
                end = total;
            } else {
                while (end < total && data[end] != '\n') ++end;
                if (end < total) ++end;
            }
            if (end <= cursor) continue;
            ranges.emplace_back(cursor, end);
            cursor = end;
        }
        if (cursor < total) {
            partial_tail = total - cursor;
        }

        std::vector<std::vector<std::unique_ptr<DftEventVisitor>>> slice_vis(
            ranges.size());
        for (std::size_t s = 0; s < ranges.size(); ++s) {
            slice_vis[s].reserve(visitors_.size());
            for (auto& v : visitors_) {
                slice_vis[s].push_back(v.get().create_parallel_slice());
            }
        }

        std::vector<coro::CoroTask<std::size_t>> slice_tasks;
        slice_tasks.reserve(ranges.size());
        for (std::size_t s = 0; s < ranges.size(); ++s) {
            slice_tasks.push_back(
                make_slice_task(chunk_buffer, ranges[s].first, ranges[s].second,
                                checkpoint_idx, slice_vis[s]));
        }
        auto slice_truncs = co_await coro::when_all(std::move(slice_tasks));

        if (!slice_truncs.empty()) {
            std::size_t last_trunc = slice_truncs.back();
            std::size_t last_len = ranges.back().second - ranges.back().first;
            if (last_trunc > 0 && last_trunc <= last_len) {
                partial_tail = last_trunc;
            }
        }

        std::vector<std::size_t> running_offsets(visitors_.size(), 0);
        for (std::size_t s = 0; s < ranges.size(); ++s) {
            for (std::size_t i = 0; i < visitors_.size(); ++i) {
                if (!slice_vis[s][i]) continue;
                slice_vis[s][i]->set_line_offset(running_offsets[i]);
                running_offsets[i] += slice_vis[s][i]->parallel_event_count();
                visitors_[i].get().merge_parallel_slice(*slice_vis[s][i]);
            }
        }

        co_return partial_tail;
    }

    static coro::CoroTask<std::size_t> make_slice_task(
        std::shared_ptr<std::string> chunk_buffer, std::size_t start,
        std::size_t end, std::size_t checkpoint_idx,
        std::vector<std::unique_ptr<DftEventVisitor>>& slice_vis) {
        std::size_t truncated = 0;
        try {
            simdjson::dom::parser local_parser;
            simdjson::dom::document_stream stream;
            auto err = local_parser
                           .parse_many(chunk_buffer->data() + start,
                                       end - start, end - start)
                           .get(stream);
            if (!err) {
                std::size_t slice_ln = 0;
                for (auto it = stream.begin(); it != stream.end(); ++it) {
                    if ((*it).error()) continue;
                    auto root = (*it).value_unsafe();
                    if (!root.is_object()) continue;
                    common::json::JsonValue json(root);
                    DFTracerEvent ev;
                    simdjson::dom::element args_dom{};
                    bool has_args = false;
                    if (DFTracerEvent::parse_scalars(root, ev, args_dom,
                                                     has_args)) {
                        std::string_view src = it.source();
                        EventRecord record{
                            ev,           json,           src,
                            chunk_buffer, checkpoint_idx, slice_ln,
                            args_dom,     has_args};
                        ++slice_ln;
                        for (auto& v : slice_vis) {
                            if (v) v->on_event(record);
                        }
                    }
                }
                truncated = stream.truncated_bytes();
            }
        } catch (...) {
        }
        co_return truncated;
    }

    static void strip_array_delimiters(char* buf, std::size_t len) {
        ::dftracer::utils::utilities::composites::dft::strip_array_delimiters(
            buf, len);
    }

    VisitorList visitors_;
    bool needs_args_map_ = false;
    bool force_serial_ = false;
    simdjson::dom::parser parser_;
    std::size_t line_number_ = 0;
    std::shared_ptr<std::string> accum_;
    std::vector<char> partial_doc_;
    std::size_t pending_checkpoint_idx_ = 0;
    bool visitor_parallel_clones_cached_ = false;
    bool parallelizable_cached_ = false;
};

}  // namespace dftracer::utils::utilities::composites::dft

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_DFT_EVENT_DISPATCHER_H
