// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_BUILD_INDEX_FOLD_DRIVER_H
#define DFTRACER_UTILS_INDEX_BUILD_INDEX_FOLD_DRIVER_H

#include <dftracer/utils/common/test_probes.h>
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/intern_cache.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/index_visitor.h>
#include <dftracer/utils/index/build/parse_gate.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/extensions/plugin_extension.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/memory/allocator.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/trace/parse_inflated.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <simdjson.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::index::build {

#if DFTRACER_UTILS_TEST_PROBES
/// Idle fold event buffers freed to cover a reservation. For tests.
inline std::atomic<std::uint64_t> g_fold_buffers_freed{0};

/// RecordParsers created so far and the peak capacity bytes they held. For
/// tests.
inline std::atomic<std::uint64_t> g_parsers_created{0};
inline std::atomic<std::uint64_t> g_parser_peak_capacity{0};
inline std::atomic<std::uint64_t> g_parser_live_capacity{0};

/// While set, every parsed piece records the peak of live bytes. For tests.
inline std::atomic<bool> g_probe_parse{false};
inline std::atomic<std::uint64_t> g_parse_peak_live{0};

/// The most pieces of one file parsed at once. For tests.
inline std::atomic<std::uint64_t> g_max_parse_in_flight{0};

/// While set, seal() xors a digest of the file's intern into
/// g_intern_order_digest. For tests.
inline std::atomic<bool> g_probe_intern_order{false};
inline std::atomic<std::uint64_t> g_intern_order_digest{0};
#endif

/// Event buffers parsed chunks decode into. A buffer goes back from whichever
/// thread ends its chunk and keeps its capacity. Idle buffers are reclaimable
/// and are freed at the next take() or give() to cover a posted request. Growth
/// while a chunk decodes is charged to the decoding thread's scope.
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

    dstd::UniquePtr<EventBuffer> take() {
        std::lock_guard<std::mutex> lock(mu_);
        answer_request();
        if (free_.empty()) {
            if (!pool_) return dstd::make_unique<EventBuffer>();
            memory::Scope scope{*pool_};
            return dstd::make_unique<EventBuffer>();
        }
        Idle idle = std::move(free_.back());
        free_.pop_back();
        idle_bytes_ -= idle.bytes;
        slot_.set_reclaimable(idle_bytes_);
        return std::move(idle.buffer);
    }

    void give(dstd::UniquePtr<EventBuffer> b) {
        if (!b) return;
        b->clear();
        const std::uint64_t bytes = b->bytes();
        std::lock_guard<std::mutex> lock(mu_);
        free_.push_back(Idle{std::move(b), bytes});
        idle_bytes_ += bytes;
        answer_request();
    }

    std::size_t idle() const {
        std::lock_guard<std::mutex> lock(mu_);
        return free_.size();
    }

    std::size_t reclaimable() const override { return 0; }
    std::size_t reclaim(std::size_t) override { return 0; }

   private:
    struct Idle {
        dstd::UniquePtr<EventBuffer> buffer;
        std::uint64_t bytes = 0;
    };

    void answer_request() {
        std::uint64_t want = slot_.take_request();
        while (want > 0 && !free_.empty()) {
            const std::uint64_t bytes = free_.back().bytes;
            free_.pop_back();
            idle_bytes_ -= bytes;
            want -= std::min(want, std::max<std::uint64_t>(bytes, 1));
#if DFTRACER_UTILS_TEST_PROBES
            g_fold_buffers_freed.fetch_add(1, std::memory_order_relaxed);
#endif
        }
        slot_.set_reclaimable(idle_bytes_);
    }

    // Declared first, so the pool closes after the slot detaches and the idle
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
                    dstd::Span<trace::views::detail::Fold* const> folds,
                    dstd::String file_path, dstd::String index_path,
                    dstd::Vector<dstd::String> extra_fields = {})
        : intern_(&intern),
          folds_(folds.begin(), folds.end()),
          file_path_(std::move(file_path)),
          index_path_(std::move(index_path)),
          extra_fields_(std::move(extra_fields)) {
        for (auto* f : folds_) {
            if (auto* b = dynamic_cast<index::extensions::BloomFold*>(f))
                bloom_ = b;
            if (auto* a =
                    dynamic_cast<index::schemas::dft::agg::AggregationFold*>(f))
                agg_ = a;
            needs_args_ |= f->needs_args();
            capture_schema_ |= f->wants_schema();
            skip_never_indexed_ &= !f->reads_never_indexed();
            for (dstd::String& name : f->extra_captures())
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

    /// Shares `pool` with other drivers instead of using one of its own. `pool`
    /// must outlive the driver and every batch it hands out. Call before the
    /// first chunk.
    void share_buffers(FoldBufferPool& pool) {
        buffers_ = &pool;
        own_buffers_.reset();
    }

    /// Also hand each chunk's record lines to `builders`, which must outlive
    /// the parse.
    void set_plugin_builders(index::extensions::PluginBuilders* builders) {
        builders_ = builders;
    }

    /// Parses pieces ahead of the folds in parallel, as many as `gate` has
    /// workers and memory for. Without a gate each chunk is parsed in
    /// on_chunk. `gate` must outlive the driver and every batch it hands out.
    void set_parse_gate(ParseGate& gate) { gate_ = &gate; }

    /// A chunk is parsed in pieces of whole lines, so a worker holds a piece
    /// and its events, not a chunk.
    static constexpr std::size_t PARSE_PIECE_BYTES = 4u << 20;
    static constexpr std::size_t PARSE_PIECE_LINES = 8192;

    /// A parser grown past this by a long line drops its buffers.
    static constexpr std::size_t PARSER_RETAIN_BYTES =
        2 * trace::PARSE_BATCH_BYTES;

    void begin(std::size_t /*num_checkpoints*/) override {}
    void bind_scope(coro::Scope* scope) override { scope_ = scope; }
    coro::CoroTask<void> on_checkpoint(std::size_t /*cp*/) override {
        co_return;
    }

    coro::CoroTask<void> on_chunk(const char* data, std::size_t len,
                                  std::size_t checkpoint_idx) override {
        const std::size_t chunk_begin = plain_offset_;
        plain_offset_ += len;
        // A line straddling this chunk's end is carried into the next chunk, so
        // it is parsed once and attributed to the chunk where it completes.
        const std::size_t cut = dstd::StringView(data, len).rfind('\n');
        if (cut == dstd::StringView::npos) {
            partial_.append(data, len);
            co_return;
        }
        const std::size_t whole = cut + 1;
        const bool dynamic = scope_ && gate_;
        std::size_t off = 0;
        while (off < whole) {
            const std::size_t end = piece_end(data, off, whole);
            Admit admit;
            if (dynamic)
                admit = co_await admit_piece(end - off + partial_.size());
            auto piece = dstd::make_unique<Parsed>();
            piece->checkpoint_idx = checkpoint_idx;
            piece->chunk_begin = chunk_begin;
            piece->chunk_end = plain_offset_;
            piece->batch = buffers_->take();
            piece->buf = dstd::make_shared<dstd::String>();
            piece->buf->reserve(partial_.size() + (end - off) +
                                simdjson::SIMDJSON_PADDING);
            piece->buf->append(partial_);
            piece->buf->append(data + off, end - off);
            piece->held = std::move(admit.mem);
            if (admit.extra) piece->extra = gate_;
            piece->alive = &alive_;
            alive_.fetch_add(1, std::memory_order_relaxed);
            partial_.clear();
            off = end;
            if (!dynamic || admit.inline_) {
                SlotGuard slot{dynamic ? gate_ : nullptr};
                parse_into(*config(), *piece);
                slot.release();
                if (chained_) {
                    co_await step_ahead(std::move(piece));
                } else {
                    step(*piece);
                    buffers_->give(std::move(piece->batch));
                    co_await stage_agg();
                }
                continue;
            }
            piece->local =
                dstd::make_unique<dftracer::utils::dstd::StringIntern>();
            in_flight_.push_back(
                scope_->spawn_future([cfg = config(), piece = std::move(piece),
                                      gate = gate_]() mutable {
                    return parse_task(std::move(cfg), std::move(piece), gate);
                }));
        }
        partial_.assign(data + whole, len - whole);
    }

    /// Steps the folds with every chunk still being parsed, in chunk order.
    coro::CoroTask<void> flush() override {
        while (!in_flight_.empty()) co_await step_oldest();
        while (!bloom_steps_.empty()) co_await await_bloom_front();
        co_await await_tails();
        if (bloom_ && bloom_->spills()) co_await bloom_->write_run();
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
#if DFTRACER_UTILS_TEST_PROBES
        if (g_probe_intern_order.load(std::memory_order_relaxed))
            probe_intern_order();
#endif
    }

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
        dstd::Vector<dstd::String> extra_fields;
#if DFTRACER_UTILS_TEST_PROBES
        mutable std::atomic<std::uint64_t> parsing{0};
#endif
    };

    using EventBuffer = FoldBufferPool::EventBuffer;

    // One chunk of whole lines, its events and the lines plugin builders read.
    // The events and lines point into `buf`.
    struct Parsed {
        Parsed() = default;
        Parsed(const Parsed&) = delete;
        Parsed& operator=(const Parsed&) = delete;
        ~Parsed() {
            if (extra) extra->drop_piece();
            if (alive) alive->fetch_sub(1, std::memory_order_release);
        }

        dstd::SharedPtr<dstd::String> buf;
        std::size_t checkpoint_idx = 0;
        std::size_t chunk_begin = 0;
        std::size_t chunk_end = 0;
        dstd::UniquePtr<EventBuffer> batch;
        dstd::Vector<dstd::StringView> lines;
        // The strings of a piece parsed ahead, interned here with ids of their
        // own, and given the file's ids in piece order when the folds see it.
        dstd::UniquePtr<dftracer::utils::dstd::StringIntern> local;
        memory::Reservation held;
        ParseGate* extra = nullptr;
        std::atomic<std::size_t>* alive = nullptr;
    };

    // `inline_` parses on the thread that cut the piece, since no worker is
    // free.
    struct Admit {
        memory::Reservation mem;
        bool extra = false;
        bool inline_ = true;
    };

    // A throwing parse gives the slot back.
    struct SlotGuard {
        ParseGate* gate;
        void release() noexcept {
            if (gate) gate->give_slot();
            gate = nullptr;
        }
        ~SlotGuard() { release(); }
    };

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

    dstd::SharedPtr<const ParseConfig> config() {
        if (!config_)
            config_ = dstd::make_shared<const ParseConfig>(
                intern_, decoder_, record_schema_, needs_args_, capture_schema_,
                skip_never_indexed_, builders_ != nullptr, extra_fields_);
        return config_;
    }

    // One parser per thread, shared by every file the thread parses. A parse
    // never suspends, so no two tasks use a thread's parser at once.
    struct LocalParser {
        json::RecordParser parser;
#if DFTRACER_UTILS_TEST_PROBES
        std::uint64_t held = 0;

        LocalParser() { g_parsers_created.fetch_add(1); }
        ~LocalParser() { g_parser_live_capacity.fetch_sub(held); }
#else
        LocalParser() = default;
#endif
        LocalParser(const LocalParser&) = delete;
        LocalParser& operator=(const LocalParser&) = delete;

        // Rounded up to a power of two so pieces of similar sizes do not each
        // regrow it.
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
#if DFTRACER_UTILS_TEST_PROBES
            const std::uint64_t now = parser.capacity();
            const std::uint64_t live =
                g_parser_live_capacity.fetch_add(now - held) + now - held;
            held = now;
            std::uint64_t peak = g_parser_peak_capacity.load();
            while (live > peak &&
                   !g_parser_peak_capacity.compare_exchange_weak(peak, live)) {
            }
#endif
        }
    };

    static void parse_into(const ParseConfig& cfg, Parsed& c) {
        thread_local LocalParser local;
        local.reserve(c.buf->size());
#if DFTRACER_UTILS_TEST_PROBES
        if (g_probe_parse.load(std::memory_order_relaxed)) {
            ProbedParse probed(cfg);
            parse_with(cfg, local.parser, c);
            probe_live();
        } else {
            parse_with(cfg, local.parser, c);
        }
#else
        parse_with(cfg, local.parser, c);
#endif
        local.release();
    }

#if DFTRACER_UTILS_TEST_PROBES
    struct ProbedParse {
        explicit ProbedParse(const ParseConfig& cfg) : cfg_(cfg) {
            const std::uint64_t now =
                cfg_.parsing.fetch_add(1, std::memory_order_relaxed) + 1;
            std::uint64_t seen = g_max_parse_in_flight.load();
            while (now > seen &&
                   !g_max_parse_in_flight.compare_exchange_weak(seen, now)) {
            }
        }
        ~ProbedParse() { cfg_.parsing.fetch_sub(1, std::memory_order_relaxed); }
        ProbedParse(const ProbedParse&) = delete;
        ProbedParse& operator=(const ProbedParse&) = delete;

       private:
        const ParseConfig& cfg_;
    };

    void probe_intern_order() const {
        std::uint64_t digest = 0;
        for (std::size_t n = 0; n < intern_->entry_count(); ++n) {
            const std::uint32_t id = intern_->entry_id(n);
            digest = digest * dstd::hash::FNV1A_PRIME + id;
            digest = digest * dstd::hash::FNV1A_PRIME +
                     std::hash<dstd::StringView>{}(intern_->resolve(id));
        }
        g_intern_order_digest.fetch_xor(digest);
    }

    static void probe_live() {
        const std::uint64_t live = memory::Manager::get().stats().live;
        std::uint64_t peak = g_parse_peak_live.load();
        while (live > peak &&
               !g_parse_peak_live.compare_exchange_weak(peak, live)) {
        }
    }
#endif

    static void parse_with(const ParseConfig& cfg, json::RecordParser& parser,
                           Parsed& c) {
        dftracer::utils::dstd::StringIntern& intern =
            c.local ? *c.local : *cfg.intern;
        std::size_t total = c.buf->size();
        trace::strip_array_delimiters(c.buf->data(), total);
        c.buf->resize(total + simdjson::SIMDJSON_PADDING, '\0');
        if (cfg.decoder == index::Decoder::Path) {
            trace::parse_lines(
                parser, c.buf->data(), total,
                [&](simdjson::dom::element root, dstd::StringView line) {
                    if (!root.is_object()) return;
                    if (cfg.keep_lines) c.lines.push_back(line);
                    trace::views::detail::decode_record_into(
                        c.batch->append(), root, intern, true, nullptr,
                        cfg.record_schema, nullptr,
                        trace::views::detail::INDEX_MAX_CHILDREN, nullptr,
                        cfg.skip_never_indexed);
                });
        } else {
            trace::views::detail::DecodeHints hints;
            dftracer::utils::dstd::InternCache cache(intern);
            trace::parse_lines(
                parser, c.buf->data(), total,
                [&](simdjson::dom::element root, dstd::StringView line) {
                    if (!trace::views::detail::append_dftracer_record(
                            *c.batch, root, intern, cfg.needs_args,
                            cfg.capture_schema, hints, &cache))
                        return;
                    if (cfg.keep_lines) c.lines.push_back(line);
                    for (const auto& name : cfg.extra_fields)
                        trace::views::detail::capture_extra_field(
                            c.batch->events_mut().back(), root, intern, name);
                });
        }
    }

    // A named coroutine with by-value parameters, not a capturing lambda
    // (CP.51): the task owns its configuration, parser and chunk.
    static coro::CoroTask<dstd::UniquePtr<Parsed>> parse_task(
        dstd::SharedPtr<const ParseConfig> cfg, dstd::UniquePtr<Parsed> chunk,
        ParseGate* gate) {
        SlotGuard slot{gate};
        parse_into(*cfg, *chunk);
        slot.release();
        co_return chunk;
    }

    trace::views::detail::ScanUnit unit_of(const Parsed& c) const {
        trace::views::detail::ScanUnit unit;
        unit.file_path = file_path_;
        unit.index_path = index_path_;
        unit.checkpoint_idx = c.checkpoint_idx;
        unit.start_byte = c.chunk_begin;
        unit.end_byte = c.chunk_end;
        return unit;
    }

    bool ahead() const { return scope_ && gate_; }

    static constexpr std::size_t MAX_PIECES_HELD = 64;

    void step(Parsed& c) {
        const auto unit = unit_of(c);
        trace::views::detail::FoldBatch fb{c.batch->events(), unit};
        for (auto* f : folds_) f->step(fb);
        if (builders_) builders_->step(c.checkpoint_idx, c.lines);
    }

    // The rows and runs the folds put into the build after an in-order step(),
    // so no worker blocks on the store.
    coro::CoroTask<void> stage_agg() {
        if (agg_ && agg_->drain_due()) co_await agg_->drain();
        if (bloom_ && bloom_->spill_pending()) co_await bloom_->write_run();
    }

    struct PieceRelease {
        coro::CoroSemaphore* permits;
        FoldBufferPool* buffers;
        void operator()(Parsed* p) const {
            buffers->give(std::move(p->batch));
            dstd::UniquePtr<Parsed> owner(p);
            owner.reset();
            permits->release(1);
        }
    };

    // Each fold steps on its own chain of tasks, so it sees every batch in
    // order while the folds and this thread run at once.
    coro::CoroTask<void> step_ahead(dstd::UniquePtr<Parsed> done) {
        if (builders_) builders_->step(done->checkpoint_idx, done->lines);
        if (!permits_) {
            permits_ = dstd::make_unique<coro::CoroSemaphore>(MAX_PIECES_HELD);
            tails_.resize(folds_.size());
        }
        co_await permits_->acquire(1);
        dstd::SharedPtr<const Parsed> piece = dstd::adopt_shared_ptr(
            done.release(), PieceRelease{permits_.get(), buffers_});
        for (std::size_t i = 0; i < folds_.size(); ++i) {
            if (folds_[i] == bloom_) continue;
            dstd::Optional<coro::SpawnFuture<void>> prev = std::move(tails_[i]);
            tails_[i].reset();
            tails_[i].emplace(scope_->spawn_future(
                [fold = folds_[i], agg = folds_[i] == agg_ ? agg_ : nullptr,
                 piece, unit = unit_of(*piece),
                 prev = std::move(prev)]() mutable {
                    return fold_task(fold, agg, std::move(piece),
                                     std::move(unit), std::move(prev));
                }));
        }
        if (bloom_) co_await step_bloom(std::move(piece));
        if (bloom_ && bloom_->spill_pending()) co_await bloom_->write_run();
    }

    // Spawned frames are destroyed after their future completes, so a task
    // drops its piece itself: the last owner returns the permit to the driver.
    // `agg` is the fold when it is the aggregation fold, whose rows are
    // staged here so no worker blocks on the store.
    static coro::CoroTask<void> fold_task(
        trace::views::detail::Fold* fold,
        index::schemas::dft::agg::AggregationFold* agg,
        dstd::SharedPtr<const Parsed> c, trace::views::detail::ScanUnit unit,
        dstd::Optional<coro::SpawnFuture<void>> prev) {
        std::exception_ptr error;
        try {
            if (prev) co_await std::move(*prev);
            fold->step({c->batch->events(), unit});
        } catch (...) {
            error = std::current_exception();
        }
        c.reset();
        if (error) std::rethrow_exception(error);
        if (agg && agg->drain_due()) co_await agg->drain();
    }

    // The bloom fold's state is per checkpoint, so batches of different
    // checkpoints are observed at once; a batch waits for the batch of its
    // checkpoint before it. With spilling, the chunks a spill writes are
    // observed first.
    coro::CoroTask<void> step_bloom(dstd::SharedPtr<const Parsed> c) {
        const std::size_t cp = c->checkpoint_idx;
        while (!bloom_steps_.empty() &&
               (bloom_steps_.size() >= MAX_PIECES_HELD ||
                (bloom_->spills() &&
                 bloom_steps_.front().checkpoint_idx +
                         index::extensions::BloomFold::SPILL_LAG <
                     cp)))
            co_await await_bloom_front();
        const auto unit = unit_of(*c);
        const auto lane = bloom_->open({c->batch->events(), unit});
        dstd::Optional<coro::SpawnFuture<void>> prev;
        if (!bloom_steps_.empty() && bloom_steps_.back().checkpoint_idx == cp) {
            prev.emplace(std::move(bloom_steps_.back().done));
            bloom_steps_.pop_back();
        }
        bloom_steps_.push_back(
            {cp, scope_->spawn_future([bloom = bloom_, lane, c = std::move(c),
                                       prev = std::move(prev)]() mutable {
                 return observe_task(bloom, lane, std::move(c),
                                     std::move(prev));
             })});
    }

    static coro::CoroTask<void> observe_task(
        index::extensions::BloomFold* bloom,
        index::extensions::BloomFold::Lane lane,
        dstd::SharedPtr<const Parsed> c,
        dstd::Optional<coro::SpawnFuture<void>> prev) {
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

    // Pieces come here in order, so a string gets the id of its first piece
    // however the parses ran.
    void commit_ids(Parsed& c) {
        auto& local = *c.local;
        remap_.assign(local.size(), 0);
        for (std::size_t n = 0; n < local.entry_count(); ++n) {
            const std::uint32_t id = local.entry_id(n);
            remap_[id] = intern_->get_or_insert(local.resolve(id));
        }
        for (auto& ev : c.batch->events_mut())
            trace::views::detail::remap_ids(ev, remap_);
        c.local.reset();
    }

    coro::CoroTask<void> drain_held() {
        while (!bloom_steps_.empty()) co_await await_bloom_front();
        co_await await_tails();
    }

    // A file with no piece parsing takes a slot by force. Another piece waits
    // for a free worker and its own bytes, and steps the oldest parse while it
    // has neither.
    coro::CoroTask<Admit> admit_piece(std::size_t plaintext) {
        const std::uint64_t bytes = ParseGate::piece_bytes(plaintext);
        for (;;) {
            Admit a;
            if (in_flight_.empty()) {
                const bool first = alive_.load(std::memory_order_acquire) == 0;
                if (first || gate_->try_piece(bytes, a.mem)) {
                    a.extra = !first;
                    a.inline_ = !gate_->spare_workers();
                    gate_->take_slot();
                    co_return a;
                }
                co_await drain_held();
                continue;
            }
            if (gate_->spare_workers() && gate_->try_slot()) {
                if (gate_->try_piece(bytes, a.mem)) {
                    a.extra = true;
                    a.inline_ = false;
                    co_return a;
                }
                gate_->give_slot();
            }
            co_await step_oldest();
        }
    }

    // Steps the folds with the oldest chunk in flight. A parse or a step that
    // threw rethrows here, after every task in flight is awaited, so no task
    // outlives the driver.
    coro::CoroTask<void> step_oldest() {
        auto future = std::move(in_flight_.front());
        in_flight_.pop_front();
        dstd::UniquePtr<Parsed> done;
        std::exception_ptr error;
        try {
            done = co_await std::move(future);
        } catch (...) {
            error = std::current_exception();
        }
        if (!error) {
            try {
                if (done->local) commit_ids(*done);
                if (ahead()) {
                    chained_ = true;
                    co_await step_ahead(std::move(done));
                } else {
                    step(*done);
                    buffers_->give(std::move(done->batch));
                    co_await stage_agg();
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

    dstd::SharedPtr<const ParseConfig> config_;
    coro::Scope* scope_ = nullptr;
    ParseGate* gate_ = nullptr;
    // Once a piece goes to the fold chains every later piece follows it, so a
    // fold sees its pieces one at a time and in order.
    bool chained_ = false;
    std::atomic<std::size_t> alive_{0};
    dstd::Vector<std::uint32_t> remap_;
    dstd::Deque<coro::SpawnFuture<dstd::UniquePtr<Parsed>>> in_flight_;
    struct BloomStep {
        std::size_t checkpoint_idx;
        coro::SpawnFuture<void> done;
    };
    dstd::Deque<BloomStep> bloom_steps_;
    index::extensions::BloomFold* bloom_ = nullptr;
    index::schemas::dft::agg::AggregationFold* agg_ = nullptr;
    dstd::UniquePtr<coro::CoroSemaphore> permits_;
    dstd::Vector<dstd::Optional<coro::SpawnFuture<void>>> tails_;
    // Shared by every file of a build, so a slot keeps its capacity from file
    // to file.
    dstd::Optional<FoldBufferPool> own_buffers_{std::in_place};
    FoldBufferPool* buffers_ = &*own_buffers_;
    dftracer::utils::dstd::StringIntern* intern_;
    dstd::Vector<trace::views::detail::Fold*> folds_;
    index::Decoder decoder_ = index::Decoder::Dftracer;
    const index::RecordSchema* record_schema_ = nullptr;
    bool needs_args_ = false;
    bool capture_schema_ = false;
    bool skip_never_indexed_ = true;
    dstd::String partial_;
    std::size_t plain_offset_ = 0;
    dstd::String file_path_;
    dstd::String index_path_;
    dstd::Vector<dstd::String> extra_fields_;
    index::extensions::PluginBuilders* builders_ = nullptr;
};

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_INDEX_FOLD_DRIVER_H
