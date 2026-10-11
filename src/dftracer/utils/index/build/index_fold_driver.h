#ifndef DFTRACER_UTILS_INDEX_BUILD_INDEX_FOLD_DRIVER_H
#define DFTRACER_UTILS_INDEX_BUILD_INDEX_FOLD_DRIVER_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/coro/async_semaphore.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/build/index_visitor.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/extensions/plugin_extension.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/trace/parse_inflated.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <simdjson.h>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::index::build {

/// Drives index folds (BloomFold, DictFold, AggregationFold) from the gzip
/// index parse: it is an IndexVisitor, so it slots into Indexer::VisitorList;
/// its on_chunk parses each member's plaintext into owned FoldEvents and steps
/// the folds. Reuses parse_buffer + strip_array_delimiters so array-wrapped
/// traces and lines that straddle chunk boundaries are handled identically to
/// the dispatcher. The caller owns the folds and calls seal() once the file's
/// members are done, then writes each fold via its write method.
class IndexFoldDriver : public index::build::IndexVisitor {
   public:
    IndexFoldDriver(dftracer::utils::StringIntern& intern,
                    std::span<trace::views::detail::Fold* const> folds,
                    std::string file_path, std::string index_path,
                    std::vector<std::string> extra_fields = {})
        : intern_(&intern),
          folds_(folds.begin(), folds.end()),
          file_path_(std::move(file_path)),
          index_path_(std::move(index_path)),
          extra_fields_(std::move(extra_fields)) {
        for (auto* f : folds_) {
            if (auto* b = dynamic_cast<index::extensions::BloomFold*>(f))
                bloom_ = b;
            needs_args_ |= f->needs_args();
            capture_schema_ |= f->wants_schema();
            skip_never_indexed_ &= !f->reads_never_indexed();
            for (std::string& name : f->extra_captures())
                if (std::find(extra_fields_.begin(), extra_fields_.end(),
                              name) == extra_fields_.end())
                    extra_fields_.push_back(std::move(name));
        }
    }

    /// Decode records as `schema` says: by path with its declared fields, or
    /// as dftracer events. `schema` is registered, so it outlives the driver.
    void set_record_schema(const index::RecordSchema& schema) {
        decoder_ = schema.decoder;
        record_schema_ = schema.fields.empty() ? nullptr : &schema;
    }

    /// Also hand each chunk's record lines to `builders`, which must outlive
    /// the parse.
    void set_plugin_builders(index::extensions::PluginBuilders* builders) {
        builders_ = builders;
    }

    /// Parse up to `n` pieces ahead of the folds, in parallel on the scope the
    /// indexer binds; 0 or 1 parses each chunk in on_chunk. A caller that
    /// indexes several files at once divides the cores between them: each
    /// piece in flight holds its plaintext and its events.
    void set_parse_ahead(std::size_t n) { parse_ahead_ = n; }

    /// Chunks are parsed ahead in pieces of about this many bytes of whole
    /// lines, so the parse ahead holds `n` pieces, not `n` chunks.
    static constexpr std::size_t PARSE_PIECE_BYTES = 4u << 20;

    void begin(std::size_t /*num_checkpoints*/) override {}
    void bind_scope(CoroScope* scope) override { scope_ = scope; }
    coro::CoroTask<void> on_checkpoint(std::size_t /*cp*/) override {
        co_return;
    }

    coro::CoroTask<void> on_chunk(const char* data, std::size_t len,
                                  std::size_t checkpoint_idx) override {
        // A line straddling this chunk's end is carried into the next chunk, so
        // it is parsed once and attributed to the chunk where it completes.
        const std::size_t cut = std::string_view(data, len).rfind('\n');
        if (cut == std::string_view::npos) {
            partial_.append(data, len);
            co_return;
        }
        const std::size_t whole = cut + 1;
        const bool ahead = scope_ && parse_ahead_ > 1;
        std::size_t off = 0;
        while (off < whole) {
            std::size_t end = whole;
            if (ahead && whole - off > PARSE_PIECE_BYTES)
                end = std::string_view(data, whole)
                          .find('\n', off + PARSE_PIECE_BYTES - 1) +
                      1;
            auto piece = std::make_unique<Parsed>();
            piece->checkpoint_idx = checkpoint_idx;
            piece->buf = std::make_shared<std::string>();
            piece->buf->reserve(partial_.size() + (end - off) +
                                simdjson::SIMDJSON_PADDING);
            piece->buf->append(partial_);
            piece->buf->append(data + off, end - off);
            partial_.clear();
            off = end;
            if (!ahead) {
                if (!inline_parser_)
                    inline_parser_ = std::make_unique<json::RecordParser>();
                parse_into(*config(), *inline_parser_, *piece);
                step(*piece);
                continue;
            }
            if (in_flight_.size() >= parse_ahead_) co_await step_oldest();
            std::unique_ptr<json::RecordParser> parser;
            if (!parsers_.empty()) {
                parser = std::move(parsers_.back());
                parsers_.pop_back();
            } else {
                parser = std::make_unique<json::RecordParser>();
            }
            in_flight_.push_back(
                scope_->spawn([cfg = config(), parser = std::move(parser),
                               piece = std::move(piece)](CoroScope&) mutable {
                    return parse_task(std::move(cfg), std::move(parser),
                                      std::move(piece));
                }));
        }
        partial_.assign(data + whole, len - whole);
    }

    /// Steps the folds with every chunk still being parsed, in chunk order.
    coro::CoroTask<void> flush() override {
        while (!in_flight_.empty()) co_await step_oldest();
        while (!bloom_steps_.empty()) co_await await_bloom_front();
        co_await await_tails();
    }

    /// Publish accumulated fold state once the file's members are all parsed.
    /// The dictionary is file-global, so one seal attributes every entry to the
    /// file; BloomFold buckets by checkpoint and does not need a per-member
    /// seal.
    void seal() {
        trace::views::detail::ScanUnit unit;
        unit.file_path = file_path_;
        unit.index_path = index_path_;
        for (auto* f : folds_) f->seal_unit(unit);
        if (builders_) builders_->finish();
    }

    void finalize(index::store::IndexDatabaseWriterContext& /*writer*/,
                  int /*file_id*/) override {}

   private:
    // What a parse needs from the driver, shared with the parse tasks so a
    // task never reads the driver itself.
    struct ParseConfig {
        dftracer::utils::StringIntern* intern;
        index::Decoder decoder;
        const index::RecordSchema* record_schema;
        bool needs_args;
        bool capture_schema;
        bool skip_never_indexed;
        bool keep_lines;
        std::vector<std::string> extra_fields;
    };

    // One chunk of whole lines, its events and the lines plugin builders read.
    // The events and lines point into `buf`.
    struct Parsed {
        std::shared_ptr<std::string> buf;
        std::size_t checkpoint_idx = 0;
        std::vector<trace::views::detail::FoldEvent> batch;
        std::vector<std::string_view> lines;
        std::unique_ptr<json::RecordParser> parser;
    };

    std::shared_ptr<const ParseConfig> config() {
        if (!config_)
            config_ = std::make_shared<const ParseConfig>(ParseConfig{
                intern_, decoder_, record_schema_, needs_args_, capture_schema_,
                skip_never_indexed_, builders_ != nullptr, extra_fields_});
        return config_;
    }

    static void parse_into(const ParseConfig& cfg, json::RecordParser& parser,
                           Parsed& c) {
        std::size_t total = c.buf->size();
        trace::strip_array_delimiters(c.buf->data(), total);
        c.buf->resize(total + simdjson::SIMDJSON_PADDING, '\0');
        std::size_t line_number = 0;
        if (cfg.decoder == index::Decoder::PATH) {
            trace::parse_lines(
                parser, c.buf->data(), total,
                [&](simdjson::dom::element root, std::string_view line) {
                    if (!root.is_object()) return;
                    if (cfg.keep_lines) c.lines.push_back(line);
                    c.batch.push_back(trace::views::detail::decode_record(
                        root, *cfg.intern, true, nullptr, cfg.record_schema,
                        nullptr, trace::views::detail::INDEX_MAX_CHILDREN,
                        nullptr, cfg.skip_never_indexed));
                });
        } else {
            trace::parse_buffer(
                parser, c.buf, total, c.checkpoint_idx, line_number,
                cfg.needs_args, [&](const trace::EventRecord& r) {
                    if (cfg.keep_lines) c.lines.push_back(r.line);
                    c.batch.push_back(trace::views::detail::build_fold_event(
                        r.ev, r.args_dom, r.has_args, *cfg.intern,
                        cfg.needs_args));
                    for (const auto& name : cfg.extra_fields)
                        trace::views::detail::capture_extra_field(
                            c.batch.back(), r.json.element(), *cfg.intern,
                            name);
                    if (cfg.capture_schema)
                        trace::views::detail::capture_schema_leaves(
                            c.batch.back(), r.json.element(), *cfg.intern);
                });
        }
    }

    // A named coroutine with by-value parameters, not a capturing lambda
    // (CP.51): the task owns its configuration, parser and chunk.
    static coro::CoroTask<std::unique_ptr<Parsed>> parse_task(
        std::shared_ptr<const ParseConfig> cfg,
        std::unique_ptr<json::RecordParser> parser,
        std::unique_ptr<Parsed> chunk) {
        parse_into(*cfg, *parser, *chunk);
        chunk->parser = std::move(parser);
        co_return chunk;
    }

    trace::views::detail::ScanUnit unit_of(const Parsed& c) const {
        trace::views::detail::ScanUnit unit;
        unit.file_path = file_path_;
        unit.index_path = index_path_;
        unit.checkpoint_idx = c.checkpoint_idx;
        return unit;
    }

    bool ahead() const { return scope_ && parse_ahead_ > 1; }

    void step(Parsed& c) {
        const auto unit = unit_of(c);
        trace::views::detail::FoldBatch fb{
            std::span<const trace::views::detail::FoldEvent>(c.batch), unit};
        for (auto* f : folds_) f->step(fb);
        if (builders_) builders_->step(c.checkpoint_idx, c.lines);
    }

    // Returns a piece's permit once its last step lets go of it.
    struct PieceRelease {
        coro::CoroSemaphore* permits;
        void operator()(Parsed* p) const {
            delete p;
            permits->release(1);
        }
    };

    // Each fold steps on its own chain of tasks, so it sees every batch in
    // order while the folds and this thread run at once. At most
    // `parse_ahead_` pieces are held by the chains.
    coro::CoroTask<void> step_ahead(std::unique_ptr<Parsed> done) {
        parsers_.push_back(std::move(done->parser));
        if (builders_) builders_->step(done->checkpoint_idx, done->lines);
        if (!permits_) {
            permits_ = std::make_unique<coro::CoroSemaphore>(parse_ahead_);
            tails_.resize(folds_.size());
        }
        co_await permits_->acquire(1);
        std::shared_ptr<const Parsed> piece(done.release(),
                                            PieceRelease{permits_.get()});
        for (std::size_t i = 0; i < folds_.size(); ++i) {
            if (folds_[i] == bloom_) continue;
            std::optional<coro::SpawnFuture<void>> prev = std::move(tails_[i]);
            tails_[i].reset();
            tails_[i].emplace(
                scope_->spawn([fold = folds_[i], piece, unit = unit_of(*piece),
                               prev = std::move(prev)](CoroScope&) mutable {
                    return fold_task(fold, std::move(piece), std::move(unit),
                                     std::move(prev));
                }));
        }
        if (bloom_) co_await step_bloom(std::move(piece));
    }

    // Spawned frames are destroyed after their future completes, so a task
    // drops its piece itself: the last owner returns the permit to the driver.
    static coro::CoroTask<void> fold_task(
        trace::views::detail::Fold* fold, std::shared_ptr<const Parsed> c,
        trace::views::detail::ScanUnit unit,
        std::optional<coro::SpawnFuture<void>> prev) {
        std::exception_ptr error;
        try {
            if (prev) co_await std::move(*prev);
            fold->step(
                {std::span<const trace::views::detail::FoldEvent>(c->batch),
                 unit});
        } catch (...) {
            error = std::current_exception();
        }
        c.reset();
        if (error) std::rethrow_exception(error);
    }

    // The bloom fold's state is per checkpoint, so batches of different
    // checkpoints are observed at once; a batch waits for the batch of its
    // checkpoint before it. With spilling, the chunks a spill writes are
    // observed first.
    coro::CoroTask<void> step_bloom(std::shared_ptr<const Parsed> c) {
        const std::size_t cp = c->checkpoint_idx;
        while (!bloom_steps_.empty() &&
               (bloom_steps_.size() >= parse_ahead_ ||
                (bloom_->spills() &&
                 bloom_steps_.front().checkpoint_idx +
                         index::extensions::BloomFold::SPILL_LAG <
                     cp)))
            co_await await_bloom_front();
        const auto unit = unit_of(*c);
        const auto lane = bloom_->open(
            {std::span<const trace::views::detail::FoldEvent>(c->batch), unit});
        std::optional<coro::SpawnFuture<void>> prev;
        if (!bloom_steps_.empty() && bloom_steps_.back().checkpoint_idx == cp) {
            prev.emplace(std::move(bloom_steps_.back().done));
            bloom_steps_.pop_back();
        }
        bloom_steps_.push_back(
            {cp, scope_->spawn([bloom = bloom_, lane, c = std::move(c),
                                prev = std::move(prev)](CoroScope&) mutable {
                 return observe_task(bloom, lane, std::move(c),
                                     std::move(prev));
             })});
    }

    static coro::CoroTask<void> observe_task(
        index::extensions::BloomFold* bloom,
        index::extensions::BloomFold::Lane lane,
        std::shared_ptr<const Parsed> c,
        std::optional<coro::SpawnFuture<void>> prev) {
        std::exception_ptr error;
        try {
            if (prev) co_await std::move(*prev);
            trace::views::detail::ScanUnit unit;
            unit.checkpoint_idx = c->checkpoint_idx;
            bloom->observe(
                lane,
                {std::span<const trace::views::detail::FoldEvent>(c->batch),
                 unit});
        } catch (...) {
            error = std::current_exception();
        }
        c.reset();
        if (error) std::rethrow_exception(error);
    }

    // A failed step rethrows here, after every task in flight is awaited, so
    // no task outlives the driver.
    coro::CoroTask<void> await_bloom_front() {
        auto done = std::move(bloom_steps_.front().done);
        bloom_steps_.pop_front();
        std::exception_ptr error;
        try {
            co_await std::move(done);
        } catch (...) {
            error = std::current_exception();
        }
        if (error) {
            co_await drain();
            std::rethrow_exception(error);
        }
    }

    coro::CoroTask<void> await_tails() {
        std::exception_ptr error;
        for (auto& tail : tails_) {
            if (!tail) continue;
            auto done = std::move(*tail);
            tail.reset();
            try {
                co_await std::move(done);
            } catch (...) {
                if (!error) error = std::current_exception();
            }
        }
        if (error) {
            co_await drain();
            std::rethrow_exception(error);
        }
    }

    coro::CoroTask<void> drain() {
        while (!in_flight_.empty()) {
            auto rest = std::move(in_flight_.front());
            in_flight_.pop_front();
            try {
                co_await std::move(rest);
            } catch (...) {
            }
        }
        while (!bloom_steps_.empty()) {
            auto rest = std::move(bloom_steps_.front().done);
            bloom_steps_.pop_front();
            try {
                co_await std::move(rest);
            } catch (...) {
            }
        }
        for (auto& tail : tails_) {
            if (!tail) continue;
            auto rest = std::move(*tail);
            tail.reset();
            try {
                co_await std::move(rest);
            } catch (...) {
            }
        }
    }

    // Steps the folds with the oldest chunk in flight. A parse or a step that
    // threw rethrows here, after every task in flight is awaited, so no task
    // outlives the driver.
    coro::CoroTask<void> step_oldest() {
        auto future = std::move(in_flight_.front());
        in_flight_.pop_front();
        std::unique_ptr<Parsed> done;
        std::exception_ptr error;
        try {
            done = co_await std::move(future);
        } catch (...) {
            error = std::current_exception();
        }
        if (!error) {
            try {
                if (ahead()) {
                    co_await step_ahead(std::move(done));
                } else {
                    step(*done);
                    parsers_.push_back(std::move(done->parser));
                }
            } catch (...) {
                error = std::current_exception();
            }
        }
        if (error) {
            co_await drain();
            std::rethrow_exception(error);
        }
    }

    std::shared_ptr<const ParseConfig> config_;
    CoroScope* scope_ = nullptr;
    std::size_t parse_ahead_ = 0;
    std::deque<coro::SpawnFuture<std::unique_ptr<Parsed>>> in_flight_;
    struct BloomStep {
        std::size_t checkpoint_idx;
        coro::SpawnFuture<void> done;
    };
    std::deque<BloomStep> bloom_steps_;
    index::extensions::BloomFold* bloom_ = nullptr;
    std::unique_ptr<coro::CoroSemaphore> permits_;
    std::vector<std::optional<coro::SpawnFuture<void>>> tails_;
    std::vector<std::unique_ptr<json::RecordParser>> parsers_;
    std::unique_ptr<json::RecordParser> inline_parser_;
    dftracer::utils::StringIntern* intern_;
    std::vector<trace::views::detail::Fold*> folds_;
    index::Decoder decoder_ = index::Decoder::DFTRACER;
    const index::RecordSchema* record_schema_ = nullptr;
    bool needs_args_ = false;
    bool capture_schema_ = false;
    bool skip_never_indexed_ = true;
    std::string partial_;
    std::string file_path_;
    std::string index_path_;
    std::vector<std::string> extra_fields_;
    index::extensions::PluginBuilders* builders_ = nullptr;
};

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_INDEX_FOLD_DRIVER_H
