#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dstd/hash/hex64.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/ptr_hash.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/index/gzip/member.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial_update.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/store/file.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/runtime/coro/async_semaphore.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/agg_event_fold.h>
#include <dftracer/utils/trace/agg_query.h>
#include <dftracer/utils/trace/parse_inflated.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/utilities/fileio/file_process_types.h>
#include <dftracer/utils/utilities/fileio/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>

namespace dftracer::utils::trace {

namespace {

namespace agg = index::schemas::dft::agg;
using views::detail::FoldEvent;

constexpr std::size_t STREAM_BUFFER_BYTES = 4u << 20;
constexpr std::size_t PARSE_LINES = 8192;

enum class PidFit { All, None, Mixed };

/// One index root, open for the length of the query, safe to read from any
/// task. The files of a root that does not exist or cannot be opened are read
/// whole.
struct Source {
    Source(const AggQuerySource& s, std::uint64_t default_params_hash)
        : path(s.index_path),
          params_hash(s.params_hash ? *s.params_hash : default_params_hash) {
        if (path.empty() || !dftracer::utils::input_exists(path)) return;
        idx.emplace(path, index::store::IndexOpenMode::ReadOnly);
    }

    std::string path;
    dstd::Optional<index::store::IndexDatabase> idx;
    std::uint64_t params_hash;
};

/// A chunk to read, for the groups whose partials could not be added whole.
/// `all` reads every group.
struct ChunkJob {
    std::uint32_t chunk = 0;
    int file_id = -1;
    bool all = false;
    /// The file has no chunk table, so chunk 0 is the whole of it.
    bool whole_file = false;
    dstd::Vector<AggGroup> groups;
};

class Task;

class Runner {
   public:
    Runner(const AggQuery& query, AggQueryResult& result)
        : read(),
          rules(query, read, result.strings()),
          out(result),
          has_time(query.ts_begin > 0 || query.ts_end < UINT64_MAX),
          reread_only(query.by_file || query.filter.has_value()) {}

    coro::CoroTask<void> run();

    void fail(std::exception_ptr e) {
        std::lock_guard<std::mutex> lock(mu);
        if (!error) error = std::move(e);
        failed.store(true, std::memory_order_release);
    }
    bool is_failed() const { return failed.load(std::memory_order_acquire); }
    bool cancelled() const { return rules.q.cancelled && rules.q.cancelled(); }
    void merge(Task& task);

    dstd::StringIntern read;
    AggEventRules rules;
    AggQueryResult& out;
    bool has_time = false;
    bool reread_only = false;

    std::unique_ptr<coro::CoroSemaphore> permits;
    dstd::Vector<std::unique_ptr<Source>> sources;

   private:
    /// One lock for all rows made every worker wait on each merge.
    static constexpr std::size_t MERGE_STRIPES = 64;
    struct Stripe {
        std::mutex mu;
        AggQueryResult::Rows rows;
    };
    static std::size_t stripe_of(const AggKey& key) {
        return (AggKeyHash{}(key) >> 32U) % MERGE_STRIPES;
    }
    void collect();

    coro::CoroTask<void> file_task(std::size_t source, std::size_t file);
    coro::CoroTask<void> chunk_task(Source* source, const std::string* path,
                                    const index::gzip::ChunkSpan* span,
                                    const ChunkJob* job);

    std::mutex mu;
    std::array<Stripe, MERGE_STRIPES> stripes;
    std::exception_ptr error;
    std::atomic<bool> failed{false};
};

struct Permit {
    explicit Permit(coro::CoroSemaphore& s) : sem(s) {}
    Permit(const Permit&) = delete;
    Permit& operator=(const Permit&) = delete;
    ~Permit() { sem.release(1); }
    coro::CoroSemaphore& sem;
};

class Task {
   public:
    Task(Runner& r, Source& s)
        : r_(r), src_(s), folder_(r.rules, rows, stats) {}

    AggQueryResult::Rows rows;
    AggQueryStats stats;

    /// Fills `jobs` with the chunks to re-read, in chunk order, and `spans`
    /// with the chunk table of the file.
    coro::CoroTask<void> scan_file(const std::string& path,
                                   dstd::Vector<ChunkJob>& jobs,
                                   dstd::Vector<index::gzip::ChunkSpan>& spans);

    coro::CoroTask<void> trace_jobs(
        const std::string& path, dstd::Vector<ChunkJob>& jobs,
        dstd::Vector<index::gzip::ChunkSpan>& spans);

    coro::CoroTask<void> read_chunk(const std::string& path,
                                    const index::gzip::ChunkSpan& span,
                                    const ChunkJob& job,
                                    const index::gzip::ChunkSpan* spans);

   private:
    struct Reread {
        std::uint32_t chunk;
        AggGroup group;
    };

    template <class Filter>
    std::uint32_t map_root(std::uint32_t id, const Filter& filter) {
        const std::string_view text = text_of(id);
        if (!filter.empty() && filter.find(text) == filter.end())
            return AGG_REJECT;
        return r_.rules.strings.get_or_insert(text);
    }

    std::string_view text_of(std::uint32_t id) const {
        const auto at = texts_.find(id);
        if (at == texts_.end())
            throw DFTUtilsException::cat(ErrorCode::Parse,
                                         "aggregation query: dictionary id ",
                                         id, " is missing in ", src_.path);
        return std::string_view(at->second.data(), at->second.size());
    }

    PidFit pid_fit(const agg::AggPartial& p) const {
        if (r_.rules.pids.empty()) return PidFit::All;
        if (!p.pids.is_exact()) return PidFit::Mixed;
        const auto ids = p.pids.ids();
        std::size_t in = 0;
        for (std::uint64_t id : ids)
            if (std::binary_search(r_.rules.pids.begin(), r_.rules.pids.end(),
                                   id))
                ++in;
        return in == ids.size() ? PidFit::All
               : in == 0        ? PidFit::None
                                : PidFit::Mixed;
    }

    static bool quantiles_missing(const agg::AggPartial& p) {
        return (p.duration_values != 0 && p.duration_q.count() == 0) ||
               (p.size_values != 0 && p.size_q.count() == 0) ||
               (p.offset_values != 0 && p.offset_q.count() == 0);
    }

    void queue_reread(std::uint32_t chunk, const AggGroup& group) {
        if (!r_.rules.q.reread) {
            ++stats.unanswered;
            return;
        }
        rereads_.push_back({chunk, group});
    }

    void take_partial(std::uint32_t chunk, const AggGroup& group,
                      std::span<const std::uint8_t> bytes);
    bool bins_answer(const agg::AggPartial& p) const;
    void take_bins(const agg::AggPartial& p, AggKey key);

    coro::CoroTask<void> whole_file_chunks(const std::string& path, int fid,
                                           std::size_t chunk_count,
                                           dstd::Vector<ChunkJob>& jobs);

    // The head of a line that a member boundary cut: the bytes before `chunk`
    // that follow the last newline.
    coro::CoroTask<std::string> tail_before(
        const std::string& path, std::uint32_t chunk,
        const index::gzip::ChunkSpan* spans);

    coro::CoroTask<std::shared_ptr<utilities::reader::internal::Reader>>
    open_reader(const std::string& path);

    void parse_lines_of(std::string& pending, std::uint32_t chunk,
                        const dstd::HashSet<AggGroup, AggGroupHash>* wanted);

    /// False, with nothing added, when the index holds no column block.
    bool add_chunk_columns(std::uint32_t chunk, int file_id,
                           const dstd::HashSet<AggGroup, AggGroupHash>* wanted);

    Runner& r_;
    Source& src_;
    AggEventFolder folder_;
    AggIdCache root_cat_, root_name_, root_host_, root_extra_;
    dstd::HashMap<std::uint32_t, dstd::String> texts_;
    dstd::Vector<Reread> rereads_;
    views::detail::FoldEventBuffer buffer_;
    FoldEvent column_event_;
    json::RecordParser parser_;
    std::shared_ptr<std::string> buf_ = std::make_shared<std::string>();
};

void Runner::merge(Task& task) {
    dstd::Vector<std::pair<std::uint32_t, AggQueryResult::Rows::value_type*>>
        order;
    order.reserve(task.rows.size());
    for (auto& row : task.rows)
        order.emplace_back(static_cast<std::uint32_t>(stripe_of(row.first)),
                           &row);
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (std::size_t at = 0; at < order.size();) {
        Stripe& stripe = stripes[order[at].first];
        std::lock_guard<std::mutex> lock(stripe.mu);
        const std::uint32_t id = order[at].first;
        for (; at < order.size() && order[at].first == id; ++at) {
            auto& [key, partial] = *order[at].second;
            auto it = stripe.rows.find(key);
            if (it == stripe.rows.end())
                stripe.rows.emplace(key, std::move(partial));
            else
                it->second.merge(partial);
        }
    }
    std::lock_guard<std::mutex> lock(mu);
    auto& s = out.stats;
    const auto& t = task.stats;
    s.files += t.files;
    s.files_without_partials += t.files_without_partials;
    s.partials_read += t.partials_read;
    s.partials_merged += t.partials_merged;
    s.partials_pruned += t.partials_pruned;
    s.partials_reread += t.partials_reread;
    s.partials_binned += t.partials_binned;
    s.chunks_reread += t.chunks_reread;
    s.chunks_from_columns += t.chunks_from_columns;
    s.unanswered += t.unanswered;
    s.events_read += t.events_read;
    s.max_batch_events = std::max(s.max_batch_events, t.max_batch_events);
}

coro::CoroTask<void> Runner::run() {
    std::size_t workers = rules.q.workers;
    if (workers == 0) {
        const Executor* ex = Executor::current();
        workers =
            ex != nullptr ? std::max<std::size_t>(1, ex->get_num_threads()) : 1;
    }
    permits = std::make_unique<coro::CoroSemaphore>(workers);
    const std::uint64_t default_params_hash = rules.q.config.params_hash();
    for (const auto& s : rules.q.sources)
        sources.push_back(std::make_unique<Source>(s, default_params_hash));

    co_await run_coro_scope([&](CoroScope& scope) -> coro::CoroTask<void> {
        for (std::size_t si = 0; si < rules.q.sources.size(); ++si)
            for (std::size_t fi = 0; fi < rules.q.sources[si].files.size();
                 ++fi)
                scope.spawn(
                    [this, si, fi](CoroScope&) { return file_task(si, fi); });
        co_return;
    });
    if (error) std::rethrow_exception(error);
    collect();
    out.stats.groups = out.rows.size();
}

void Runner::collect() {
    std::size_t total = out.rows.size();
    for (const Stripe& stripe : stripes) total += stripe.rows.size();
    out.rows.reserve(total);
    for (Stripe& stripe : stripes) {
        for (auto& [key, partial] : stripe.rows) {
            auto it = out.rows.find(key);
            if (it == out.rows.end())
                out.rows.emplace(key, std::move(partial));
            else
                it->second.merge(partial);
        }
        stripe.rows = AggQueryResult::Rows();
    }
}

coro::CoroTask<void> Runner::file_task(std::size_t source, std::size_t file) {
    Source& src = *sources[source];
    const std::string path(rules.q.sources[source].files[file]);
    dstd::Vector<ChunkJob> jobs;
    dstd::Vector<index::gzip::ChunkSpan> spans;
    {
        co_await permits->acquire(1);
        Permit permit(*permits);
        try {
            if (!is_failed()) {
                Task task(*this, src);
                if (rules.q.from_trace)
                    co_await task.trace_jobs(path, jobs, spans);
                else
                    co_await task.scan_file(path, jobs, spans);
                merge(task);
            }
        } catch (...) {
            fail(std::current_exception());
        }
    }
    if (jobs.empty() || is_failed() || cancelled()) co_return;
    co_await run_coro_scope([&](CoroScope& scope) -> coro::CoroTask<void> {
        for (const ChunkJob& job : jobs)
            scope.spawn([this, &src, &path, &spans, &job](CoroScope&) {
                return chunk_task(&src, &path, spans.data(), &job);
            });
        co_return;
    });
}

coro::CoroTask<void> Runner::chunk_task(Source* source, const std::string* path,
                                        const index::gzip::ChunkSpan* spans,
                                        const ChunkJob* job) {
    co_await permits->acquire(1);
    Permit permit(*permits);
    try {
        if (is_failed() || cancelled()) co_return;
        Task task(*this, *source);
        const index::gzip::ChunkSpan whole_file;
        const index::gzip::ChunkSpan& span =
            job->whole_file ? whole_file : spans[job->chunk];
        co_await task.read_chunk(*path, span, *job, spans);
        merge(task);
    } catch (...) {
        fail(std::current_exception());
    }
}

coro::CoroTask<void> Task::scan_file(
    const std::string& path, dstd::Vector<ChunkJob>& jobs,
    dstd::Vector<index::gzip::ChunkSpan>& spans) {
    if (!src_.idx)
        throw DFTUtilsException::cat(ErrorCode::NotFound,
                                     "aggregation query: ", src_.path,
                                     " is not an open index");
    const auto& idx = *src_.idx;
    const int fid =
        idx.get_file_info_id(index::store::internal::get_logical_path(path));
    if (fid < 0)
        throw DFTUtilsException::cat(ErrorCode::NotFound,
                                     "aggregation query: ", path,
                                     " is not in the index ", src_.path);
    ++stats.files;
    const auto table = idx.query_chunk_spans(fid);
    spans.assign(table.begin(), table.end());

    const auto state =
        idx.extension_state(fid, index::store::IndexExtension::Agg);
    const bool usable = state && state->ready &&
                        state->version == index::store::layout::ext_version(
                                              index::store::layout::Ext::Agg) &&
                        state->params_hash == src_.params_hash;

    std::size_t seen = 0;
    if (usable) {
        agg::tier::load_file_texts(idx, fid, texts_);
        agg::tier::for_each_partial(
            *src_.idx, fid,
            [&](std::uint32_t chunk, std::uint32_t cat_root,
                std::uint32_t name_root, std::uint32_t host_root,
                std::uint32_t extra_root, AggKind kind,
                std::span<const std::uint8_t> bytes) {
                ++seen;
                ++stats.partials_read;
                const std::uint32_t cat =
                    root_cat_.get(cat_root, [&](std::uint32_t id) {
                        return map_root(id, r_.rules.categories);
                    });
                const std::uint32_t name =
                    root_name_.get(name_root, [&](std::uint32_t id) {
                        return map_root(id, r_.rules.names);
                    });
                const std::uint32_t host =
                    root_host_.get(host_root, [&](std::uint32_t id) {
                        return map_root(id, r_.rules.hosts);
                    });
                if (cat == AGG_REJECT || name == AGG_REJECT ||
                    host == AGG_REJECT) {
                    ++stats.partials_pruned;
                    return true;
                }
                const std::uint32_t extra =
                    root_extra_.get(extra_root, [&](std::uint32_t id) {
                        const std::string_view text = text_of(id);
                        return text.empty()
                                   ? dstd::StringIntern::NO_ID
                                   : r_.rules.strings.get_or_insert(text);
                    });
                take_partial(chunk, {cat, name, host, extra, kind}, bytes);
                return true;
            });
    }

    if (seen == 0) {
        ++stats.files_without_partials;
        if (!r_.rules.q.reread) {
            ++stats.unanswered;
            co_return;
        }
        co_await whole_file_chunks(path, fid, spans.size(), jobs);
        co_return;
    }

    for (std::size_t i = 0; i < rereads_.size();) {
        ChunkJob job;
        job.chunk = rereads_[i].chunk;
        job.file_id = fid;
        if (job.chunk >= spans.size())
            throw DFTUtilsException::cat(
                ErrorCode::Parse, "aggregation query: a partial of ", path,
                " names chunk ", job.chunk, " past its ", spans.size());
        for (; i < rereads_.size() && rereads_[i].chunk == job.chunk; ++i) {
            job.groups.push_back(rereads_[i].group);
            ++stats.partials_reread;
        }
        jobs.push_back(std::move(job));
    }
}

coro::CoroTask<void> Task::trace_jobs(
    const std::string& path, dstd::Vector<ChunkJob>& jobs,
    dstd::Vector<index::gzip::ChunkSpan>& spans) {
    ++stats.files;
    int fid = -1;
    if (src_.idx) {
        const int found = src_.idx->get_file_info_id(
            index::store::internal::get_logical_path(path));
        if (found >= 0) {
            fid = found;
            const auto table = src_.idx->query_chunk_spans(fid);
            spans.assign(table.begin(), table.end());
        }
    }
    if (spans.empty()) {
        ChunkJob job;
        job.all = true;
        job.whole_file = true;
        jobs.push_back(std::move(job));
        co_return;
    }
    co_await whole_file_chunks(path, fid, spans.size(), jobs);
}

coro::CoroTask<void> Task::whole_file_chunks(const std::string& path, int fid,
                                             std::size_t chunk_count,
                                             dstd::Vector<ChunkJob>& jobs) {
    dstd::Vector<std::uint64_t> chunks;
    bool pruned_to_list = false;
    if (r_.has_time && src_.idx) {
        index::plan::PruneRequest request;
        request.index_path = src_.path;
        request.file_path = path;
        request.db = &*src_.idx;
        request.time_range =
            std::make_pair(static_cast<double>(r_.rules.q.ts_begin),
                           r_.rules.q.ts_end == UINT64_MAX
                               ? 0.0
                               : static_cast<double>(r_.rules.q.ts_end));
        request.chunk_count = chunk_count;
        request.by_start = true;
        auto pruned = co_await index::plan::prune_file(std::move(request));
        if (!pruned) throw DFTUtilsException(pruned.error());
        if (!pruned->file_may_match) co_return;
        if (!pruned->all_chunks) {
            chunks.assign(pruned->candidates.begin(), pruned->candidates.end());
            pruned_to_list = true;
        }
    }
    if (!pruned_to_list)
        for (std::size_t c = 0; c < chunk_count; ++c) chunks.push_back(c);
    for (std::uint64_t c : chunks) {
        if (c >= chunk_count) continue;
        ChunkJob job;
        job.chunk = static_cast<std::uint32_t>(c);
        job.file_id = fid;
        job.all = true;
        jobs.push_back(std::move(job));
    }
}

void Task::take_partial(std::uint32_t chunk, const AggGroup& group,
                        std::span<const std::uint8_t> bytes) {
    if (r_.reread_only && !r_.has_time && r_.rules.pids.empty()) {
        queue_reread(chunk, group);
        return;
    }
    agg::AggPartial::Fields keep;
    keep.quantiles = r_.rules.q.quantiles && !r_.reread_only;
    keep.bins = r_.rules.q.sums_only && !r_.reread_only;
    auto parsed = agg::AggPartial::deserialize(
        dstd::Span<const std::uint8_t>(bytes.data(), bytes.size()),
        r_.rules.q.config.sketch_accuracy, keep);
    if (!parsed)
        throw DFTUtilsException(ErrorCode::Parse,
                                "aggregation partial: malformed record");
    agg::AggPartial& p = *parsed;
    if (p.count == 0) {
        ++stats.partials_pruned;
        return;
    }
    if (p.ts > p.ts_last_start) {
        queue_reread(chunk, group);
        return;
    }
    if (p.ts_last_start < r_.rules.q.ts_begin || p.ts >= r_.rules.q.ts_end) {
        ++stats.partials_pruned;
        return;
    }
    const PidFit fit = pid_fit(p);
    if (fit == PidFit::None) {
        ++stats.partials_pruned;
        return;
    }
    const bool inside =
        p.ts >= r_.rules.q.ts_begin && p.ts_last_start < r_.rules.q.ts_end;
    if (r_.reread_only || !inside || fit != PidFit::All ||
        (r_.rules.q.quantiles && quantiles_missing(p))) {
        queue_reread(chunk, group);
        return;
    }

    AggKey key;
    key.cat = group.cat;
    key.name = group.name;
    if (r_.rules.q.by_host) key.host = group.host;
    if (r_.rules.q.by_extra) key.extra = group.extra;
    if (r_.rules.q.by_kind) key.kind = group.kind;
    bool split = false;
    if (r_.rules.q.by_pid) {
        const auto ids = p.pids.ids();
        if (!p.pids.is_exact() || ids.size() != 1)
            split = true;
        else
            key.pid = ids[0];
    }
    if (r_.rules.q.by_tid) {
        const auto ids = p.tids.ids();
        if (!p.tids.is_exact() || ids.size() != 1)
            split = true;
        else
            key.tid = ids[0];
    }
    if (r_.rules.q.window_us != 0) {
        key.window = r_.rules.bucket(p.ts);
        if (r_.rules.bucket(p.ts_last_start) != key.window) split = true;
    }
    if (split) {
        if (!bins_answer(p)) {
            queue_reread(chunk, group);
            return;
        }
        take_bins(p, key);
        return;
    }

    ++stats.partials_merged;
    auto it = rows.find(key);
    if (it == rows.end())
        rows.emplace(key, std::move(p));
    else
        it->second.merge(p);
}

bool Task::bins_answer(const agg::AggPartial& p) const {
    if (!r_.rules.q.sums_only || r_.rules.q.quantiles) return false;
    if (p.bin_interval == 0 || p.bins.empty()) return false;
    return r_.rules.q.window_us == 0 ||
           r_.rules.q.window_us % p.bin_interval == 0;
}

void Task::take_bins(const agg::AggPartial& p, AggKey key) {
    const auto stat_of = [](std::uint64_t n, std::uint64_t sum) {
        dataframe::FieldStat s;
        if (n == 0) return s;
        s.n = n;
        s.sum = static_cast<double>(sum);
        s.domain = dataframe::FieldStatDomain::U64;
        s.esum = std::bit_cast<std::int64_t>(sum);
        return s;
    };
    for (const agg::TimeBin& b : p.bins) {
        if (r_.rules.q.by_pid) key.pid = b.pid;
        if (r_.rules.q.by_tid) key.tid = b.tid;
        if (r_.rules.q.window_us != 0)
            key.window = (b.bin * p.bin_interval / r_.rules.q.window_us) *
                         r_.rules.q.window_us;
        auto it = rows.find(key);
        if (it == rows.end())
            it =
                rows.emplace(key,
                             agg::AggPartial(r_.rules.q.config.sketch_accuracy))
                    .first;
        agg::AggPartial& row = it->second;
        row.count += b.count;
        row.duration.merge(stat_of(b.duration_n, b.duration_sum));
        row.size.merge(stat_of(b.size_n, b.size_sum));
        row.duration_values += b.duration_n;
        row.size_values += b.size_n;
    }
    ++stats.partials_binned;
}

coro::CoroTask<std::shared_ptr<utilities::reader::internal::Reader>>
Task::open_reader(const std::string& path) {
    auto input =
        utilities::fileio::IndexedReadInput::from_file(path).with_index(
            src_.path);
    utilities::fileio::IndexedFileReaderUtility utility;
    co_return co_await utility(input);
}

coro::CoroTask<std::string> Task::tail_before(
    const std::string& path, std::uint32_t chunk,
    const index::gzip::ChunkSpan* spans) {
    namespace rd = utilities::reader::internal;
    auto reader = co_await open_reader(path);
    std::string tail;
    for (std::uint32_t k = chunk; k-- > 0;) {
        auto stream =
            reader->stream(rd::StreamConfig()
                               .stream_type(rd::StreamType::Bytes)
                               .range_type(rd::RangeType::ByteRange)
                               .buffer_size(STREAM_BUFFER_BYTES)
                               .from(spans[k].uc_offset)
                               .to(spans[k].uc_offset + spans[k].uc_size));
        std::string after;  // bytes after the last newline seen in chunk k
        bool found = false;
        while (!stream->done()) {
            auto piece = co_await stream->read_async();
            if (piece.empty()) break;
            const std::string_view data(piece.data(), piece.size());
            const std::size_t nl = data.rfind('\n');
            if (nl == std::string_view::npos) {
                after.append(data);
            } else {
                after.assign(data.substr(nl + 1));
                found = true;
            }
        }
        tail.insert(0, after);
        if (found) break;
    }
    co_return tail;
}

// Parses the complete lines of `pending` as the events of `chunk` and leaves
// the rest. A line belongs to the chunk that holds its newline.
void Task::parse_lines_of(std::string& pending, std::uint32_t chunk,
                          const dstd::HashSet<AggGroup, AggGroupHash>* wanted) {
    const std::size_t nl = pending.rfind('\n');
    if (nl == std::string::npos) return;
    const std::size_t whole = nl + 1;
    std::size_t off = 0;
    while (off < whole) {
        std::size_t end = off;
        for (std::size_t lines = 0; end < whole && lines < PARSE_LINES; ++lines)
            end = pending.find('\n', end) + 1;
        buf_->assign(pending, off, end - off);
        off = end;
        std::size_t total = buf_->size();
        trace::strip_array_delimiters(buf_->data(), total);
        buf_->resize(total + simdjson::SIMDJSON_PADDING, '\0');
        std::size_t line_number = 0;
        trace::parse_buffer(
            parser_, buf_, total, chunk, line_number,
            [&](const trace::EventRecord& rec) {
                if (r_.rules.q.filter && !r_.rules.q.filter->evaluate(rec.json))
                    return;
                FoldEvent& ev = buffer_.append();
                views::detail::build_fold_event_into(ev, rec.ev, rec.args_dom,
                                                     rec.has_args,
                                                     r_.rules.read, true);
            });
        stats.max_batch_events =
            std::max(stats.max_batch_events, buffer_.size());
        for (const FoldEvent& ev : buffer_.events())
            folder_.add_event(ev, wanted);
        buffer_.clear();
    }
    pending.erase(0, whole);
}

coro::CoroTask<void> Task::read_chunk(const std::string& path,
                                      const index::gzip::ChunkSpan& span,
                                      const ChunkJob& job,
                                      const index::gzip::ChunkSpan* spans) {
    namespace rd = utilities::reader::internal;
    ++stats.chunks_reread;
    dstd::HashSet<AggGroup, AggGroupHash> wanted;
    if (!job.all) wanted.insert(job.groups.begin(), job.groups.end());
    const dstd::HashSet<AggGroup, AggGroupHash>* filter =
        job.all ? nullptr : &wanted;
    if (!r_.rules.q.from_trace && !r_.rules.q.filter &&
        r_.rules.q.config.extra_group_keys.empty() &&
        add_chunk_columns(job.chunk, job.file_id, filter)) {
        co_return;
    }

    auto reader = co_await open_reader(path);
    const std::uint64_t from = job.whole_file ? 0 : span.uc_offset;
    const std::uint64_t to = job.whole_file ? reader->get_max_bytes()
                                            : span.uc_offset + span.uc_size;
    if (from >= to) co_return;
    auto stream = reader->stream(rd::StreamConfig()
                                     .stream_type(rd::StreamType::Bytes)
                                     .range_type(rd::RangeType::ByteRange)
                                     .buffer_size(STREAM_BUFFER_BYTES)
                                     .from(from)
                                     .to(to));
    std::string pending;
    bool decided = job.chunk == 0;
    while (!stream->done()) {
        if (r_.cancelled()) co_return;
        auto piece = co_await stream->read_async();
        if (piece.empty()) break;
        pending.append(piece.data(), piece.size());
        if (!decided) {
            const std::size_t nl = pending.find('\n');
            if (nl == std::string::npos) continue;
            decided = true;
            // The head is a whole line only when it parses as one object. The
            // tail of a line cut by the member boundary has a brace the head
            // does not close, and an empty head continues a line before.
            auto head = parser_.parse(pending.data(), nl);
            const bool whole =
                nl > 0 && !head.error() && head.value_unsafe().is_object();
            if (!whole) {
                std::string tail = co_await tail_before(path, job.chunk, spans);
                pending.insert(0, tail);
            }
        }
        parse_lines_of(pending, job.chunk, filter);
    }
}

bool Task::add_chunk_columns(
    std::uint32_t chunk, int file_id,
    const dstd::HashSet<AggGroup, AggGroupHash>* wanted) {
    agg::ChunkColumns block;
    if (!src_.idx ||
        !agg::tier::get_chunk_columns(*src_.idx, file_id, chunk, block))
        return false;
    dstd::Vector<std::uint32_t> read_ids(block.ids.size(), AGG_UNSET);
    const auto read_id = [&](std::uint32_t local) {
        std::uint32_t& id = read_ids[local];
        if (id == AGG_UNSET) {
            const dstd::String& text = block.texts[local];
            id = text.empty() ? dstd::StringIntern::NO_ID
                              : r_.rules.read.get_or_insert(text);
        }
        return id;
    };
    dstd::Vector<std::uint32_t> path_ids(block.columns.size());
    for (std::size_t c = 0; c < block.columns.size(); ++c)
        path_ids[c] =
            r_.rules.read.get_or_insert(block.texts[block.columns[c].path]);
    dstd::Vector<std::size_t> at(block.columns.size(), 0);
    for (std::size_t row = 0; row < block.rows; ++row) {
        views::detail::reset_fold_event(column_event_);
        column_event_.ts = block.ts[row];
        column_event_.dur = block.dur[row];
        column_event_.has_dur = block.has_dur[row] != 0;
        column_event_.pid = block.pid[row];
        column_event_.tid = block.tid[row];
        column_event_.phase = static_cast<RecordPhase>(block.phase[row]);
        column_event_.cat_id = read_id(block.cat[row]);
        column_event_.name_id = read_id(block.name[row]);
        column_event_.hhash_id = read_id(block.host[row]);
        column_event_.fhash_id = read_id(block.file[row]);
        for (std::size_t c = 0; c < block.columns.size(); ++c) {
            const agg::ChunkColumn& column = block.columns[c];
            if (at[c] >= column.rows.size() || column.rows[at[c]] != row)
                continue;
            const std::size_t k = at[c]++;
            views::detail::FoldEvent::ArgValue value = column.double_at(k);
            if (column.kind == agg::ColumnKind::Int64)
                value = column.int64_at(k);
            else if (column.kind == agg::ColumnKind::Uint64)
                value = column.uint64_at(k);
            column_event_.args.emplace_back(path_ids[c], value);
        }
        folder_.add_event(column_event_, wanted);
    }
    ++stats.chunks_from_columns;
    return true;
}

}  // namespace

coro::CoroTask<Result<AggQueryResult>> run_agg_query(const AggQuery& query) {
    AggQueryResult result;
    try {
        Runner runner(query, result);
        co_await runner.run();
    } catch (const DFTUtilsException& e) {
        co_return make_error(e.code(), e.what());
    } catch (const std::exception& e) {
        co_return make_error(ErrorCode::Unknown, e.what());
    }
    co_return result;
}

}  // namespace dftracer::utils::trace
