#ifndef DFTRACER_UTILS_INDEX_BUILD_INDEX_FOLD_DRIVER_H
#define DFTRACER_UTILS_INDEX_BUILD_INDEX_FOLD_DRIVER_H

#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/index_visitor.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/extensions/plugin_extension.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/memory/allocator.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/coro/async_semaphore.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/parse_inflated.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <simdjson.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::index::build {

/// Idle fold event buffers freed so far because a reservation over the budget
/// asked for bytes. For tests.
inline std::atomic<std::uint64_t> g_fold_buffers_freed{0};

/// RecordParsers created by index builds so far, and the most capacity bytes
/// the live ones held at once. For tests.
inline std::atomic<std::uint64_t> g_parsers_created{0};
inline std::atomic<std::uint64_t> g_parser_peak_capacity{0};
inline std::atomic<std::uint64_t> g_parser_live_capacity{0};

/// While set, every parsed piece reads the memory manager's live bytes and
/// keeps the largest in g_parse_peak_live. For tests.
inline std::atomic<bool> g_probe_parse{false};
inline std::atomic<std::uint64_t> g_parse_peak_live{0};

/// Event buffers parsed chunks decode into. A buffer goes back once the last
/// step of its chunk lets go, on whichever thread that is, and keeps its
/// slots' capacity, so a steady-state parse allocates no event vectors. At
/// most the chunks in flight hold one each.
///
/// The idle buffers are what the build can give back: the pool reports them
/// on a ReclaimSlot of its own "fold buffers" pool, below the pool current at
/// construction, and frees idle buffers to cover a posted request at the next
/// take() or give(). A new buffer is made under that pool's scope; what it
/// grows to while a chunk decodes is charged to the decoding thread's scope,
/// since the decode also interns strings that outlive the buffers.
class FoldBufferPool final : public memory::Reclaimer {
   public:
    using EventBuffer = trace::views::detail::FoldEventBuffer;

    FoldBufferPool()
        : pool_(memory::current_pool().child("fold buffers")),
          free_(memory::PoolAllocator<Idle>(pool_ ? *pool_
                                                  : memory::current_pool())) {
        if (pool_) slot_ = pool_->attach(*this);
    }
    FoldBufferPool(const FoldBufferPool&) = delete;
    FoldBufferPool& operator=(const FoldBufferPool&) = delete;

    std::unique_ptr<EventBuffer> take() {
        std::lock_guard<std::mutex> lock(mu_);
        answer_request();
        if (free_.empty()) {
            if (!pool_) return std::make_unique<EventBuffer>();
            memory::Scope scope{*pool_};
            return std::make_unique<EventBuffer>();
        }
        Idle idle = std::move(free_.back());
        free_.pop_back();
        idle_bytes_ -= idle.bytes;
        slot_.set_reclaimable(idle_bytes_);
        return std::move(idle.buffer);
    }

    void give(std::unique_ptr<EventBuffer> b) {
        if (!b) return;
        // The slots this use filled, a lower bound on what the buffer holds:
        // the slots' own vectors are not counted.
        const std::uint64_t bytes =
            b->size() * sizeof(trace::views::detail::FoldEvent);
        b->clear();
        std::lock_guard<std::mutex> lock(mu_);
        free_.push_back(Idle{std::move(b), bytes});
        idle_bytes_ += bytes;
        answer_request();
    }

    std::size_t idle() const {
        std::lock_guard<std::mutex> lock(mu_);
        return free_.size();
    }

    // The slot carries the idle bytes; the pool frees on its own thread.
    std::size_t reclaimable() const override { return 0; }
    std::size_t reclaim(std::size_t) override { return 0; }

   private:
    struct Idle {
        std::unique_ptr<EventBuffer> buffer;
        std::uint64_t bytes = 0;
    };

    // Holds mu_.
    void answer_request() {
        std::uint64_t want = slot_.take_request();
        while (want > 0 && !free_.empty()) {
            const std::uint64_t bytes = free_.back().bytes;
            free_.pop_back();
            idle_bytes_ -= bytes;
            want -= std::min(want, std::max<std::uint64_t>(bytes, 1));
            g_fold_buffers_freed.fetch_add(1, std::memory_order_relaxed);
        }
        slot_.set_reclaimable(idle_bytes_);
    }

    // Declared first: the pool closes after the slot detaches and the idle
    // buffers free.
    memory::PoolHandle pool_;
    dstd::PoolVector<Idle> free_;
    memory::ReclaimSlot slot_;
    mutable std::mutex mu_;
    std::uint64_t idle_bytes_ = 0;
};

/// Drives index folds (BloomFold, DictFold, AggregationFold) from the gzip
/// index parse: it is an IndexVisitor, so it slots into Indexer::VisitorList;
/// its on_chunk parses each member's plaintext into owned FoldEvents and steps
/// the folds. Reuses parse_buffer + strip_array_delimiters so array-wrapped
/// traces and lines that straddle chunk boundaries are handled identically to
/// the dispatcher. The caller owns the folds and calls seal() once the file's
/// members are done, then writes each fold via its write method.
class IndexFoldDriver : public index::build::IndexVisitor {
   public:
    IndexFoldDriver(dftracer::utils::dstd::StringIntern& intern,
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

    /// Take event buffers from `pool`, shared with other drivers, instead of
    /// a pool of this driver's own. `pool` must outlive the driver and every
    /// batch it hands out; call before the first chunk.
    void share_buffers(FoldBufferPool& pool) {
        buffers_ = &pool;
        own_buffers_.reset();
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

    /// A chunk is parsed in pieces of whole lines, each up to this many bytes
    /// and up to PARSE_PIECE_LINES lines, so a worker holds one piece and its
    /// events, not a chunk, and the parse ahead holds `n` pieces.
    static constexpr std::size_t PARSE_PIECE_BYTES = 4u << 20;
    static constexpr std::size_t PARSE_PIECE_LINES = 8192;

    /// A worker's parser keeps its buffers between pieces unless a long line
    /// grew them past this many bytes of document capacity.
    static constexpr std::size_t PARSER_RETAIN_BYTES =
        2 * trace::PARSE_BATCH_BYTES;

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
            const std::size_t end = piece_end(data, off, whole);
            auto piece = std::make_unique<Parsed>();
            piece->checkpoint_idx = checkpoint_idx;
            piece->batch = buffers_->take();
            piece->buf = std::make_shared<std::string>();
            piece->buf->reserve(partial_.size() + (end - off) +
                                simdjson::SIMDJSON_PADDING);
            piece->buf->append(partial_);
            piece->buf->append(data + off, end - off);
            partial_.clear();
            off = end;
            if (!ahead) {
                parse_into(*config(), *piece);
                step(*piece);
                buffers_->give(std::move(piece->batch));
                continue;
            }
            if (in_flight_.size() >= parse_ahead_) co_await step_oldest();
            in_flight_.push_back(scope_->spawn(
                [cfg = config(), piece = std::move(piece)](CoroScope&) mutable {
                    return parse_task(std::move(cfg), std::move(piece));
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
        dftracer::utils::dstd::StringIntern* intern;
        index::Decoder decoder;
        const index::RecordSchema* record_schema;
        bool needs_args;
        bool capture_schema;
        bool skip_never_indexed;
        bool keep_lines;
        std::vector<std::string> extra_fields;
    };

    using EventBuffer = FoldBufferPool::EventBuffer;

    // One chunk of whole lines, its events and the lines plugin builders read.
    // The events and lines point into `buf`.
    struct Parsed {
        std::shared_ptr<std::string> buf;
        std::size_t checkpoint_idx = 0;
        std::unique_ptr<EventBuffer> batch;
        std::vector<std::string_view> lines;
    };

    // The end of the piece that starts at `off`: whole lines up to
    // PARSE_PIECE_BYTES or PARSE_PIECE_LINES, and at least one line.
    static std::size_t piece_end(const char* data, std::size_t off,
                                 std::size_t whole) {
        const std::size_t limit = std::min(whole, off + PARSE_PIECE_BYTES);
        std::size_t end = off;
        std::size_t lines = 0;
        while (end < limit && lines < PARSE_PIECE_LINES) {
            const void* nl = std::memchr(data + end, '\n', whole - end);
            end = static_cast<const char*>(nl) - data + 1;
            ++lines;
        }
        return end;
    }

    std::shared_ptr<const ParseConfig> config() {
        if (!config_)
            config_ = std::make_shared<const ParseConfig>(ParseConfig{
                intern_, decoder_, record_schema_, needs_args_, capture_schema_,
                skip_never_indexed_, builders_ != nullptr, extra_fields_});
        return config_;
    }

    // One parser per thread, shared by every file the thread parses. A parse
    // never suspends, so no two tasks use a thread's parser at once.
    struct LocalParser {
        json::RecordParser parser;
        std::uint64_t held = 0;

        LocalParser() { g_parsers_created.fetch_add(1); }
        ~LocalParser() { g_parser_live_capacity.fetch_sub(held); }
        LocalParser(const LocalParser&) = delete;
        LocalParser& operator=(const LocalParser&) = delete;

        // Sizes the parser once for a piece of `bytes`, up to one parse
        // batch, rounded up to a power of two so pieces of similar sizes do not
        // each regrow it.
        void reserve(std::size_t bytes) {
            bytes = std::min(bytes, trace::PARSE_BATCH_BYTES);
            if (parser.capacity() >= bytes) return;
            std::size_t want = 1u << 16;
            while (want < bytes) want <<= 1;
            if (const auto err = parser.allocate(want))
                throw simdjson::simdjson_error(err);
            record();
        }

        void release() {
            record();
            if (parser.capacity() > PARSER_RETAIN_BYTES)
                parser = json::RecordParser();
            record();
        }

        void record() {
            const std::uint64_t now = parser.capacity();
            const std::uint64_t live =
                g_parser_live_capacity.fetch_add(now - held) + now - held;
            held = now;
            std::uint64_t peak = g_parser_peak_capacity.load();
            while (live > peak &&
                   !g_parser_peak_capacity.compare_exchange_weak(peak, live)) {
            }
        }
    };

    static void parse_into(const ParseConfig& cfg, Parsed& c) {
        thread_local LocalParser local;
        local.reserve(c.buf->size());
        parse_with(cfg, local.parser, c);
        if (g_probe_parse.load(std::memory_order_relaxed)) probe_live();
        local.release();
    }

    static void probe_live() {
        const std::uint64_t live = memory::Manager::get().stats().live;
        std::uint64_t peak = g_parse_peak_live.load();
        while (live > peak &&
               !g_parse_peak_live.compare_exchange_weak(peak, live)) {
        }
    }

    static void parse_with(const ParseConfig& cfg, json::RecordParser& parser,
                           Parsed& c) {
        std::size_t total = c.buf->size();
        trace::strip_array_delimiters(c.buf->data(), total);
        c.buf->resize(total + simdjson::SIMDJSON_PADDING, '\0');
        std::size_t line_number = 0;
        if (cfg.decoder == index::Decoder::Path) {
            trace::parse_lines(
                parser, c.buf->data(), total,
                [&](simdjson::dom::element root, std::string_view line) {
                    if (!root.is_object()) return;
                    if (cfg.keep_lines) c.lines.push_back(line);
                    trace::views::detail::decode_record_into(
                        c.batch->append(), root, *cfg.intern, true, nullptr,
                        cfg.record_schema, nullptr,
                        trace::views::detail::INDEX_MAX_CHILDREN, nullptr,
                        cfg.skip_never_indexed);
                });
        } else {
            trace::parse_buffer(
                parser, c.buf, total, c.checkpoint_idx, line_number,
                [&](const trace::EventRecord& r) {
                    if (cfg.keep_lines) c.lines.push_back(r.line);
                    trace::views::detail::FoldEvent& ev = c.batch->append();
                    trace::views::detail::build_fold_event_into(
                        ev, r.ev, r.args_dom, r.has_args, *cfg.intern,
                        cfg.needs_args);
                    for (const auto& name : cfg.extra_fields)
                        trace::views::detail::capture_extra_field(
                            ev, r.json.element(), *cfg.intern, name);
                    if (cfg.capture_schema)
                        trace::views::detail::capture_schema_leaves(
                            ev, r.json.element(), *cfg.intern);
                });
        }
    }

    // A named coroutine with by-value parameters, not a capturing lambda
    // (CP.51): the task owns its configuration, parser and chunk.
    static coro::CoroTask<std::unique_ptr<Parsed>> parse_task(
        std::shared_ptr<const ParseConfig> cfg, std::unique_ptr<Parsed> chunk) {
        parse_into(*cfg, *chunk);
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
        trace::views::detail::FoldBatch fb{c.batch->events(), unit};
        for (auto* f : folds_) f->step(fb);
        if (builders_) builders_->step(c.checkpoint_idx, c.lines);
    }

    // Returns a piece's permit once its last step lets go of it.
    struct PieceRelease {
        coro::CoroSemaphore* permits;
        FoldBufferPool* buffers;
        void operator()(Parsed* p) const {
            buffers->give(std::move(p->batch));
            delete p;
            permits->release(1);
        }
    };

    // Each fold steps on its own chain of tasks, so it sees every batch in
    // order while the folds and this thread run at once. At most
    // `parse_ahead_` pieces are held by the chains.
    coro::CoroTask<void> step_ahead(std::unique_ptr<Parsed> done) {
        if (builders_) builders_->step(done->checkpoint_idx, done->lines);
        if (!permits_) {
            permits_ = std::make_unique<coro::CoroSemaphore>(parse_ahead_);
            tails_.resize(folds_.size());
        }
        co_await permits_->acquire(1);
        std::shared_ptr<const Parsed> piece(
            done.release(), PieceRelease{permits_.get(), buffers_});
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
            fold->step({c->batch->events(), unit});
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
        const auto lane = bloom_->open({c->batch->events(), unit});
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
            bloom->observe(lane, {c->batch->events(), unit});
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
                    buffers_->give(std::move(done->batch));
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
    // Event buffers come from `buffers_`: a pool of the driver's own, or one
    // shared by every file of a build so a slot keeps its vectors' capacity
    // from file to file.
    std::optional<FoldBufferPool> own_buffers_{std::in_place};
    FoldBufferPool* buffers_ = &*own_buffers_;
    dftracer::utils::dstd::StringIntern* intern_;
    std::vector<trace::views::detail::Fold*> folds_;
    index::Decoder decoder_ = index::Decoder::Dftracer;
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
