// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/dstd/hash/hex64.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/ptr_hash.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/gzip/member.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial_update.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/schemas/dft/agg/chunk_columns.h>
#include <dftracer/utils/index/store/file.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/json/record_parser.h>
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
        if (path.empty() ||
            !dftracer::utils::common::input_exists(index::store_of(path)))
            return;
        snapshot.emplace(common::unwrap(index::plan::open_snapshot(path)));
    }

    std::string path;
    dstd::Optional<index::Snapshot> snapshot;
    std::uint64_t params_hash;
};

/// A chunk to read, for the groups whose partials could not be added whole.
/// `all` reads every group.
struct ChunkJob {
    std::uint32_t chunk = 0;
    /// The file the chunk belongs to, absent when the index does not hold it.
    dstd::Optional<index::FileRef> file;
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
    void take_stats(const AggQueryStats& t);
    void submit(std::size_t seq, std::size_t sub, AggQueryResult::Rows& rows);

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
    using Row = AggQueryResult::Rows::value_type;
    /// The floating moments of one file's row, kept by an ordered query.
    struct Moments {
        std::size_t seq = 0;
        /// 0 for the file's partials, 1 + the job for a chunk re-read.
        std::size_t sub = 0;
        AggKey key;
        dataframe::FieldStat duration;
        dataframe::FieldStat size;
        dataframe::FieldStat offset;
    };
    /// An ordered query also keeps the moments of each file's rows. After the
    /// last file each stripe merges them again in file order and replaces the
    /// moments its rows merged in the order the files finished.
    struct Stripe {
        std::mutex mu;
        AggQueryResult::Rows rows;
        AggQueryResult::SumRows sums;
        /// One list per file, so an append under the lock moves no moments.
        dstd::Vector<dstd::Vector<Moments>> moments;
        void add(const dstd::Vector<Row*>& part);
        void add_sums(const dstd::Vector<Row*>& part);
        void drain(bool offset);
    };
    static std::size_t stripe_of(const AggKey& key) {
        return (AggKeyHash{}(key) >> 32U) % MERGE_STRIPES;
    }
    /// Stripes moved into the result at once. Each wave grows the result by
    /// its rows only and frees its stripes, so the peak stays near one copy.
    static constexpr std::size_t JOIN_WAVE = 8;
    bool sums_rows() const { return rules.q.sums_only && !rules.q.quantiles; }
    coro::CoroTask<void> collect();
    template <class Map>
    coro::CoroTask<void> join(Map Stripe::* member, Map& into);
    template <class Map>
    coro::CoroTask<void> move_task(Map Stripe::* member, std::size_t at,
                                   typename Map::value_container_type* values,
                                   std::size_t offset);
    coro::CoroTask<void> drain_task(std::size_t at);

    coro::CoroTask<void> file_task(std::size_t source, std::size_t file,
                                   std::size_t seq);
    coro::CoroTask<void> chunk_task(Source* source, const std::string* path,
                                    const index::gzip::ChunkSpan* span,
                                    const ChunkJob* job, std::size_t seq,
                                    std::size_t sub);

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
            throw common::DFTUtilsException::cat(
                common::ErrorCode::Parse, "aggregation query: dictionary id ",
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

    coro::CoroTask<void> whole_file_chunks(const std::string& path,
                                           const index::FileRef* file,
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
    bool add_chunk_columns(const ChunkJob& job,
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

void trim_row(agg::AggPartial& p, const AggKeep& keep) {
    const auto drop = [](agg::DistinctIds& ids, bool kept) {
        if (!kept && !ids.empty()) ids = agg::DistinctIds();
    };
    drop(p.pids, keep.pids);
    drop(p.tids, keep.tids);
    drop(p.files, keep.files);
    drop(p.pid_tids, keep.pid_tids);
    drop(p.pid_files, keep.pid_files);
    if (!keep.pid_spans) p.pid_spans.clear();
    if (!keep.offset) {
        p.offset = {};
        p.offset_q = agg::QuantileStore(p.accuracy);
        p.offset_values = 0;
    }
}

void Runner::Stripe::add(const dstd::Vector<Row*>& part) {
    for (Row* row : part) {
        auto it = rows.find(row->first);
        if (it == rows.end())
            rows.emplace(row->first, std::move(row->second));
        else
            it->second.merge(row->second);
    }
}

void Runner::Stripe::add_sums(const dstd::Vector<Row*>& part) {
    for (Row* row : part) {
        const agg::AggPartial& p = row->second;
        AggSums one{p.count, p.duration, p.size};
        auto it = sums.find(row->first);
        if (it == sums.end())
            sums.emplace(row->first, one);
        else
            it->second.merge(one);
    }
}

void Runner::Stripe::drain(bool offset) {
    dstd::Vector<std::pair<std::size_t, const Moments*>> order;
    for (const auto& file : moments) {
        for (const Moments& m : file) {
            const auto it = rows.find(m.key);
            order.emplace_back(static_cast<std::size_t>(it - rows.begin()), &m);
        }
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first
                                  : std::pair(a.second->seq, a.second->sub) <
                                        std::pair(b.second->seq, b.second->sub);
    });
    for (std::size_t at = 0; at < order.size();) {
        Moments merged = *order[at].second;
        const std::size_t row = order[at].first;
        for (++at; at < order.size() && order[at].first == row; ++at) {
            merged.duration.merge(order[at].second->duration);
            merged.size.merge(order[at].second->size);
            if (offset) merged.offset.merge(order[at].second->offset);
        }
        agg::AggPartial& p = (rows.begin() + row)->second;
        p.duration = merged.duration;
        p.size = merged.size;
        if (offset) p.offset = merged.offset;
    }
    moments = dstd::Vector<dstd::Vector<Moments>>();
}

coro::CoroTask<void> Runner::drain_task(std::size_t at) {
    stripes[at].drain(rules.q.keep.offset);
    co_return;
}

void Runner::submit(std::size_t seq, std::size_t sub,
                    AggQueryResult::Rows& file_rows) {
    std::array<dstd::Vector<Row*>, MERGE_STRIPES> parts;
    std::array<dstd::Vector<Moments>, MERGE_STRIPES> moments;
    for (auto& row : file_rows) {
        auto& [key, partial] = row;
        trim_row(partial, rules.q.keep);
        const std::size_t at = stripe_of(key);
        // The moments merge only once, in file order, at the end.
        if (rules.q.ordered) {
            Moments& m = moments[at].emplace_back();
            m.seq = seq;
            m.sub = sub;
            m.key = key;
            m.duration = partial.duration;
            m.size = partial.size;
            partial.duration = {};
            partial.size = {};
            if (rules.q.keep.offset) {
                m.offset = partial.offset;
                partial.offset = {};
            }
        }
        parts[at].push_back(&row);
    }
    for (std::size_t at = 0; at < MERGE_STRIPES; ++at) {
        if (parts[at].empty()) continue;
        Stripe& stripe = stripes[at];
        std::lock_guard<std::mutex> lock(stripe.mu);
        if (!moments[at].empty())
            stripe.moments.push_back(std::move(moments[at]));
        if (sums_rows())
            stripe.add_sums(parts[at]);
        else
            stripe.add(parts[at]);
    }
}

void Runner::take_stats(const AggQueryStats& t) {
    std::lock_guard<std::mutex> lock(mu);
    auto& s = out.stats;
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
        const coro::Executor* ex = coro::Executor::current();
        workers =
            ex != nullptr ? std::max<std::size_t>(1, ex->concurrency()) : 1;
    }
    permits = std::make_unique<coro::CoroSemaphore>(workers);
    const std::uint64_t default_params_hash = rules.q.config.params_hash();
    for (const auto& s : rules.q.sources)
        sources.push_back(std::make_unique<Source>(s, default_params_hash));

    co_await coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        std::size_t seq = 0;
        for (std::size_t si = 0; si < rules.q.sources.size(); ++si)
            for (std::size_t fi = 0; fi < rules.q.sources[si].files.size();
                 ++fi, ++seq)
                scope.spawn(
                    [this, si, fi, seq]() { return file_task(si, fi, seq); });
        co_return;
    });
    if (error) std::rethrow_exception(error);
    if (rules.q.ordered) {
        co_await coro::with_scope(
            [&](coro::Scope& scope) -> coro::CoroTask<void> {
                for (std::size_t at = 0; at < MERGE_STRIPES; ++at)
                    scope.spawn([this, at]() { return drain_task(at); });
                co_return;
            });
    }
    co_await collect();
    out.stats.groups = out.rows.size() + out.sums.size();
}

// A key lives in one stripe, so the stripes join without a lookup.
coro::CoroTask<void> Runner::collect() {
    co_await join(&Stripe::rows, out.rows);
    co_await join(&Stripe::sums, out.sums);
}

template <class Map>
coro::CoroTask<void> Runner::join(Map Stripe::* member, Map& into) {
    using Values = typename Map::value_container_type;
    std::size_t total = 0;
    for (const Stripe& stripe : stripes) total += (stripe.*member).size();
    if (total == 0) co_return;
    Values values;
    values.reserve(total);
    for (std::size_t first = 0; first < MERGE_STRIPES; first += JOIN_WAVE) {
        const std::size_t last = std::min(first + JOIN_WAVE, MERGE_STRIPES);
        std::array<std::size_t, JOIN_WAVE> offsets{};
        std::size_t end = values.size();
        for (std::size_t at = first; at < last; ++at) {
            offsets[at - first] = end;
            end += (stripes[at].*member).size();
        }
        values.resize(end);
        co_await coro::with_scope(
            [&](coro::Scope& scope) -> coro::CoroTask<void> {
                for (std::size_t at = first; at < last; ++at)
                    scope.spawn([this, member, at, values = &values,
                                 offset = offsets[at - first]]() {
                        return move_task(member, at, values, offset);
                    });
                co_return;
            });
    }
    into.replace(std::move(values));
}

template <class Map>
coro::CoroTask<void> Runner::move_task(
    Map Stripe::* member, std::size_t at,
    typename Map::value_container_type* values, std::size_t offset) {
    Map& rows = stripes[at].*member;
    auto part = std::move(rows).extract();
    rows = Map();
    std::move(part.begin(), part.end(),
              values->begin() + static_cast<std::ptrdiff_t>(offset));
    co_return;
}

coro::CoroTask<void> Runner::file_task(std::size_t source, std::size_t file,
                                       std::size_t seq) {
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
                take_stats(task.stats);
                submit(seq, 0, task.rows);
            }
        } catch (...) {
            fail(std::current_exception());
        }
    }
    if (jobs.empty() || is_failed() || cancelled()) co_return;
    co_await coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        for (std::size_t at = 0; at < jobs.size(); ++at)
            scope.spawn([this, &src, &path, &spans, &jobs, seq, at]() {
                return chunk_task(&src, &path, spans.data(), &jobs[at], seq,
                                  at + 1);
            });
        co_return;
    });
}

coro::CoroTask<void> Runner::chunk_task(Source* source, const std::string* path,
                                        const index::gzip::ChunkSpan* spans,
                                        const ChunkJob* job, std::size_t seq,
                                        std::size_t sub) {
    co_await permits->acquire(1);
    Permit permit(*permits);
    try {
        if (is_failed() || cancelled()) co_return;
        Task task(*this, *source);
        const index::gzip::ChunkSpan whole_file;
        const index::gzip::ChunkSpan& span =
            job->whole_file ? whole_file : spans[job->chunk];
        co_await task.read_chunk(*path, span, *job, spans);
        take_stats(task.stats);
        submit(seq, sub, task.rows);
    } catch (...) {
        fail(std::current_exception());
    }
}

coro::CoroTask<void> Task::scan_file(
    const std::string& path, dstd::Vector<ChunkJob>& jobs,
    dstd::Vector<index::gzip::ChunkSpan>& spans) {
    if (!src_.snapshot)
        throw common::DFTUtilsException::cat(common::ErrorCode::NotFound,
                                             "aggregation query: ", src_.path,
                                             " is not an open index");
    const auto file =
        src_.snapshot->file(index::store::internal::get_logical_path(path));
    if (!file)
        throw common::DFTUtilsException::cat(
            common::ErrorCode::NotFound, "aggregation query: ", path,
            " is not in the index ", src_.path);
    ++stats.files;
    spans = common::unwrap(index::spans(*src_.snapshot, *file));

    const bool usable =
        src_.snapshot->extension(*file, index::store::IndexExtension::Agg,
                                 src_.params_hash) == index::Freshness::Current;

    std::size_t seen = 0;
    if (usable) {
        common::unwrap(index::agg_file_texts(*src_.snapshot, *file, texts_));
        common::unwrap(index::for_each_agg_partial(
            *src_.snapshot, *file,
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
            }));
    }

    if (seen == 0) {
        ++stats.files_without_partials;
        if (!r_.rules.q.reread) {
            ++stats.unanswered;
            co_return;
        }
        co_await whole_file_chunks(path, &*file, spans.size(), jobs);
        co_return;
    }

    for (std::size_t i = 0; i < rereads_.size();) {
        ChunkJob job;
        job.chunk = rereads_[i].chunk;
        job.file = *file;
        if (job.chunk >= spans.size())
            throw common::DFTUtilsException::cat(
                common::ErrorCode::Parse, "aggregation query: a partial of ",
                path, " names chunk ", job.chunk, " past its ", spans.size());
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
    dstd::Optional<index::FileRef> file;
    if (src_.snapshot) {
        if (auto found = src_.snapshot->file(
                index::store::internal::get_logical_path(path)))
            file = std::move(*found);
    }
    if (file) spans = common::unwrap(index::spans(*src_.snapshot, *file));
    if (spans.empty()) {
        ChunkJob job;
        job.all = true;
        job.whole_file = true;
        jobs.push_back(std::move(job));
        co_return;
    }
    const index::FileRef* held = file ? &*file : nullptr;
    co_await whole_file_chunks(path, held, spans.size(), jobs);
}

coro::CoroTask<void> Task::whole_file_chunks(const std::string& path,
                                             const index::FileRef* file,
                                             std::size_t chunk_count,
                                             dstd::Vector<ChunkJob>& jobs) {
    dstd::Vector<std::uint64_t> chunks;
    bool pruned_to_list = false;
    if (r_.has_time && src_.snapshot) {
        index::plan::PruneRequest request;
        request.index_path = src_.path;
        request.file_path = path;
        request.snapshot = &*src_.snapshot;
        request.time_range =
            std::make_pair(static_cast<double>(r_.rules.q.ts_begin),
                           r_.rules.q.ts_end == UINT64_MAX
                               ? 0.0
                               : static_cast<double>(r_.rules.q.ts_end));
        request.chunk_count = chunk_count;
        request.by_start = true;
        auto pruned = co_await index::plan::prune_file(std::move(request));
        if (!pruned) throw common::DFTUtilsException(pruned.error());
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
        if (file != nullptr) job.file = *file;
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
    const AggKeep& kept = r_.rules.q.keep;
    keep.offset = kept.offset;
    keep.pid_spans = kept.pid_spans;
    keep.files = kept.files;
    keep.pid_tids = kept.pid_tids;
    keep.pid_files = kept.pid_files;
    keep.pids = kept.pids || r_.rules.q.by_pid || !r_.rules.pids.empty();
    keep.tids = kept.tids || r_.rules.q.by_tid;
    auto parsed = agg::AggPartial::deserialize(
        dstd::Span<const std::uint8_t>(bytes.data(), bytes.size()),
        r_.rules.q.config.sketch_accuracy, keep);
    if (!parsed)
        throw common::DFTUtilsException(
            common::ErrorCode::Parse, "aggregation partial: malformed record");
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
    trim_row(p, kept);
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
        add_chunk_columns(job, filter)) {
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
    const ChunkJob& job, const dstd::HashSet<AggGroup, AggGroupHash>* wanted) {
    if (!src_.snapshot || !job.file) return false;
    auto found = common::unwrap(
        index::agg_chunk_columns(*src_.snapshot, *job.file, job.chunk));
    if (!found) return false;
    agg::ChunkColumns block = std::move(*found);
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

coro::CoroTask<common::Result<AggQueryResult>> run_agg_query(
    const AggQuery& query) {
    AggQueryResult result;
    try {
        Runner runner(query, result);
        co_await runner.run();
    } catch (const common::DFTUtilsException& e) {
        co_return dstd::Unexpected<dstd::Error>(e.error());
    } catch (const std::exception& e) {
        co_return common::make_error(common::ErrorCode::Unknown, e.what());
    }
    co_return result;
}

}  // namespace dftracer::utils::trace
