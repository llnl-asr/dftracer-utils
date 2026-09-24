#ifndef DFTRACER_UTILS_TRACE_VIEWS_INDEX_FOLD_DRIVER_H
#define DFTRACER_UTILS_TRACE_VIEWS_INDEX_FOLD_DRIVER_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/trace/parse_inflated.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/utilities/indexer/index_visitor.h>
#include <simdjson.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace dftracer::utils::trace::views::detail {

/// Drives index folds (BloomFold, DictFold, AggregationFold) from the gzip
/// index parse: it is an IndexVisitor, so it slots into Indexer::VisitorList;
/// its on_chunk parses each member's plaintext into owned FoldEvents and steps
/// the folds. Reuses parse_buffer + strip_array_delimiters so array-wrapped
/// traces and lines that straddle chunk boundaries are handled identically to
/// the dispatcher. The caller owns the folds and calls seal() once the file's
/// members are done, then writes each fold via write_to_sink.
class IndexFoldDriver : public utilities::indexer::IndexVisitor {
   public:
    IndexFoldDriver(dftracer::utils::StringIntern& intern,
                    std::span<Fold* const> folds, std::string file_path,
                    std::string index_path,
                    std::vector<std::string> extra_fields = {})
        : intern_(&intern),
          folds_(folds.begin(), folds.end()),
          file_path_(std::move(file_path)),
          index_path_(std::move(index_path)),
          extra_fields_(std::move(extra_fields)) {
        for (auto* f : folds_) {
            needs_args_ |= f->needs_args();
            capture_schema_ |= f->wants_schema();
            for (std::string& name : f->extra_captures())
                if (std::find(extra_fields_.begin(), extra_fields_.end(),
                              name) == extra_fields_.end())
                    extra_fields_.push_back(std::move(name));
        }
    }

    void begin(std::size_t /*num_checkpoints*/) override {}
    coro::CoroTask<void> on_checkpoint(std::size_t /*cp*/) override {
        co_return;
    }

    coro::CoroTask<void> on_chunk(const char* data, std::size_t len,
                                  std::size_t checkpoint_idx) override {
        if (len == 0 && partial_.empty()) co_return;
        auto buf = std::make_shared<std::string>();
        buf->reserve(partial_.size() + len + simdjson::SIMDJSON_PADDING);
        buf->append(partial_);
        buf->append(data, len);
        partial_.clear();

        std::size_t total = buf->size();
        strip_array_delimiters(buf->data(), total);
        buf->resize(total + simdjson::SIMDJSON_PADDING, '\0');

        batch_.clear();
        std::size_t truncated = parse_buffer(
            parser_, buf, total, checkpoint_idx, line_number_, needs_args_,
            [&](const EventRecord& r) {
                batch_.push_back(build_fold_event(r.ev, r.args_dom, r.has_args,
                                                  *intern_, needs_args_));
                for (const auto& name : extra_fields_)
                    capture_extra_field(batch_.back(), r.json.element(),
                                        *intern_, name);
                if (capture_schema_)
                    capture_schema_leaves(batch_.back(), r.json.element(),
                                          *intern_);
            });
        // A line straddling this chunk's end is carried into the next chunk, so
        // it is parsed once and attributed to the chunk where it completes.
        if (truncated > 0 && truncated <= total)
            partial_.assign(buf->data() + total - truncated,
                            buf->data() + total);

        ScanUnit unit;
        unit.file_path = file_path_;
        unit.index_path = index_path_;
        unit.checkpoint_idx = checkpoint_idx;
        FoldBatch fb{std::span<const FoldEvent>(batch_), unit};
        for (auto* f : folds_) f->step(fb);
        co_return;
    }

    /// Publish accumulated fold state once the file's members are all parsed.
    /// The dictionary is file-global, so one seal attributes every entry to the
    /// file; BloomFold buckets by checkpoint and does not need a per-member
    /// seal.
    void seal() {
        ScanUnit unit;
        unit.file_path = file_path_;
        unit.index_path = index_path_;
        for (auto* f : folds_) f->seal_unit(unit);
    }

    void finalize(utilities::indexer::IndexDatabaseWriterContext& /*writer*/,
                  int /*file_id*/) override {}

   private:
    dftracer::utils::StringIntern* intern_;
    std::vector<Fold*> folds_;
    bool needs_args_ = false;
    bool capture_schema_ = false;
    simdjson::dom::parser parser_;
    std::string partial_;
    std::size_t line_number_ = 0;
    std::string file_path_;
    std::string index_path_;
    std::vector<std::string> extra_fields_;
    std::vector<FoldEvent> batch_;
};

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_INDEX_FOLD_DRIVER_H
