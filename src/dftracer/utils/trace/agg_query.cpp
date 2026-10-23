#include <dftracer/utils/common/error.h>
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
#include <dftracer/utils/trace/agg_query.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/parse_inflated.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/utilities/fileio/file_process_types.h>
#include <dftracer/utils/utilities/fileio/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>

#include <algorithm>
#include <atomic>
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

constexpr std::uint32_t REJECT = 0xFFFFFFFFu;
constexpr std::uint32_t UNSET = 0xFFFFFFFEu;
constexpr std::uint32_t MAX_CACHED_ID = 1u << 20;
constexpr char EXTRA_SEP = '\x1f';
constexpr std::size_t STREAM_BUFFER_BYTES = 4u << 20;
constexpr std::size_t PARSE_LINES = 8192;

/// id -> result id, looked up once per id; ids past MAX_CACHED_ID are looked
/// up each time.
class IdCache {
   public:
    template <class Fn>
    std::uint32_t get(std::uint32_t id, Fn&& compute) {
        if (id >= MAX_CACHED_ID) return compute(id);
        if (id >= ids_.size())
            ids_.resize(std::max<std::size_t>(id + 1, ids_.size() * 2), UNSET);
        if (ids_[id] == UNSET) ids_[id] = compute(id);
        return ids_[id];
    }

   private:
    dstd::Vector<std::uint32_t> ids_;
};

/// Where a stored partial stands against the pid filter.
enum class PidFit { All, None, Mixed };

/// One index root, open for the length of the query. The reads on it are
/// thread safe.
struct Source {
    Source(const AggQuerySource& s, std::uint64_t default_params_hash)
        : path(s.index_path),
          idx(path, index::store::IndexOpenMode::ReadOnly),
          db(idx.db()),
          params_hash(s.params_hash ? *s.params_hash : default_params_hash) {}

    std::string path;
    index::store::IndexDatabase idx;
    std::shared_ptr<index::store::RocksDatabase> db;
    std::uint64_t params_hash;
};

/// The (cat, name, host, extra, kind) of a partial, as result ids; the extra
/// id is NO_ID for none.
struct Group {
    std::uint32_t cat;
    std::uint32_t name;
    std::uint32_t host;
    std::uint32_t extra;
    AggKind kind;

    bool operator==(const Group&) const = default;
};

struct GroupHash {
    using is_avalanching = void;
    std::size_t operator()(const Group& g) const noexcept {
        using dftracer::utils::hash::splitmix64;
        return static_cast<std::size_t>(splitmix64(
            splitmix64(splitmix64((std::uint64_t{g.cat} << 32) | g.name) ^
                       ((std::uint64_t{g.host} << 32) | g.extra)) ^
            static_cast<std::uint64_t>(g.kind)));
    }
};

/// A chunk to read: the groups whose partials could not be added whole; `all`
/// reads every group.
struct ChunkJob {
    std::uint32_t chunk = 0;
    bool all = false;
    dstd::Vector<Group> groups;
};

class Task;

class Runner {
   public:
    Runner(const AggQuery& query, AggQueryResult& result)
        : q(query),
          out(result),
          strings(result.strings()),
          window_cfg(query.config),
          read_cfg(query.config) {
        for (const auto& c : q.categories) {
            std::string storage;
            categories.emplace(
                dstd::String(trace::internal::to_lower_ascii(c, storage)));
        }
        for (const auto& n : q.names) names.emplace(n);
        for (const auto& h : q.hosts) hosts.emplace(h);
        pids.assign(q.pids.begin(), q.pids.end());
        std::sort(pids.begin(), pids.end());
        window_cfg.time_interval_us = q.window_us;
        read_cfg.compute_percentiles = q.quantiles;
        if (!q.args) {
            read_cfg.track_default_args = false;
            read_cfg.custom_metric_fields.clear();
        }
        has_time = q.ts_begin > 0 || q.ts_end < UINT64_MAX;
        reread_only = q.by_file || q.filter.has_value();
    }

    coro::CoroTask<void> run();

    std::uint64_t bucket(std::uint64_t ts) const {
        return agg::compute_time_bucket(ts, window_cfg);
    }
    void fail(std::exception_ptr e) {
        std::lock_guard<std::mutex> lock(mu);
        if (!error) error = std::move(e);
        failed.store(true, std::memory_order_release);
    }
    bool is_failed() const { return failed.load(std::memory_order_acquire); }
    void merge(Task& task);

    const AggQuery& q;
    AggQueryResult& out;
    dstd::StringIntern& strings;
    /// Ids of the events a re-read decodes; shared by the tasks.
    dstd::StringIntern read;
    agg::AggregationConfig window_cfg;
    agg::AggregationConfig read_cfg;
    dstd::HashSet<dstd::String> categories;
    dstd::HashSet<dstd::String> names;
    dstd::HashSet<dstd::String> hosts;
    dstd::Vector<std::uint64_t> pids;
    bool has_time = false;
    bool reread_only = false;

    std::unique_ptr<coro::CoroSemaphore> permits;
    dstd::Vector<std::unique_ptr<Source>> sources;

   private:
    coro::CoroTask<void> file_task(std::size_t source, std::size_t file);
    coro::CoroTask<void> chunk_task(Source* source, const std::string* path,
                                    const index::gzip::ChunkSpan* span,
                                    const ChunkJob* job);

    std::mutex mu;
    std::exception_ptr error;
    std::atomic<bool> failed{false};
};

/// Returns its permit when the task ends, on any path.
struct Permit {
    explicit Permit(coro::CoroSemaphore& s) : sem(s) {}
    Permit(const Permit&) = delete;
    Permit& operator=(const Permit&) = delete;
    ~Permit() { sem.release(1); }
    coro::CoroSemaphore& sem;
};

/// The state of one task: a file scan or a chunk re-read. It folds into its
/// own map, which the runner merges into the result when the task ends.
class Task {
   public:
    Task(Runner& r, Source& s) : r_(r), src_(s) {}

    AggQueryResult::Rows rows;
    AggQueryStats stats;

    /// Streams the partials of `file` and adds the ones that answer whole.
    /// Fills `jobs` with the chunks to re-read, in chunk order, and `spans`
    /// with the chunk table of the file.
    coro::CoroTask<void> scan_file(const std::string& path,
                                   dstd::Vector<ChunkJob>& jobs,
                                   dstd::Vector<index::gzip::ChunkSpan>& spans);

    coro::CoroTask<void> read_chunk(const std::string& path,
                                    const index::gzip::ChunkSpan& span,
                                    const ChunkJob& job,
                                    const index::gzip::ChunkSpan* spans);

   private:
    struct ArgId {
        std::uint32_t length;
        std::uint32_t id;
    };

    struct Reread {
        std::uint32_t chunk;
        Group group;
    };

    template <class Filter>
    std::uint32_t map_root(std::uint32_t id, const Filter& filter) {
        const auto text = agg::tier::read_dictionary_entry(*src_.db, id);
        if (!text)
            throw DFTUtilsException::cat(ErrorCode::Parse,
                                         "aggregation query: dictionary id ",
                                         id, " is missing in ", src_.path);
        if (!filter.empty() &&
            filter.find(std::string_view(*text)) == filter.end())
            return REJECT;
        return r_.strings.get_or_insert(*text);
    }

    std::uint32_t map_root_arg(std::uint32_t id) {
        return root_arg_.get(id, [&](std::uint32_t i) {
            const auto text = agg::tier::read_dictionary_entry(*src_.db, i);
            if (!text)
                throw DFTUtilsException::cat(
                    ErrorCode::Parse, "aggregation query: dictionary id ", i,
                    " is missing in ", src_.path);
            return r_.strings.get_or_insert(*text);
        });
    }

    PidFit pid_fit(const agg::AggPartial& p) const {
        if (r_.pids.empty()) return PidFit::All;
        if (!p.pids.is_exact()) return PidFit::Mixed;
        const auto ids = p.pids.ids();
        std::size_t in = 0;
        for (std::uint64_t id : ids)
            if (std::binary_search(r_.pids.begin(), r_.pids.end(), id)) ++in;
        return in == ids.size() ? PidFit::All
               : in == 0        ? PidFit::None
                                : PidFit::Mixed;
    }

    // Values were added event by event, and no quantile store holds them.
    static bool quantiles_missing(const agg::AggPartial& p) {
        if ((p.duration_values != 0 && p.duration_q.count() == 0) ||
            (p.size_values != 0 && p.size_q.count() == 0) ||
            (p.offset_values != 0 && p.offset_q.count() == 0))
            return true;
        for (const auto& a : p.args)
            if (a.values != 0 && a.quantiles.count() == 0) return true;
        return false;
    }

    void queue_reread(std::uint32_t chunk, const Group& group) {
        if (!r_.q.reread) {
            ++stats.unanswered;
            return;
        }
        rereads_.push_back({chunk, group});
    }

    void take_partial(std::uint32_t chunk, const Group& group,
                      std::span<const std::uint8_t> bytes);

    coro::CoroTask<void> whole_file_chunks(const std::string& path,
                                           std::size_t chunk_count,
                                           dstd::Vector<ChunkJob>& jobs);

    // The bytes of the file's chunks before `chunk` that follow the last
    // newline: the head of a line that a member boundary cut.
    coro::CoroTask<std::string> tail_before(
        const std::string& path, std::uint32_t chunk,
        const index::gzip::ChunkSpan* spans);

    coro::CoroTask<std::shared_ptr<utilities::reader::internal::Reader>>
    open_reader(const std::string& path);

    void parse_lines_of(std::string& pending, std::uint32_t chunk,
                        const dstd::HashSet<Group, GroupHash>* wanted);

    std::uint32_t read_cat(std::uint32_t id) {
        return build_cat_.get(id, [&](std::uint32_t i) {
            const std::string_view raw = i == dstd::StringIntern::NO_ID
                                             ? std::string_view{}
                                             : r_.read.resolve(i);
            std::string storage;
            const std::string_view lower =
                trace::internal::to_lower_ascii(raw, storage);
            if (!r_.categories.empty() &&
                r_.categories.find(lower) == r_.categories.end())
                return REJECT;
            return r_.strings.get_or_insert(lower);
        });
    }

    std::uint32_t read_name(std::uint32_t id) {
        return build_name_.get(id, [&](std::uint32_t i) {
            const std::string_view raw = i == dstd::StringIntern::NO_ID
                                             ? std::string_view{}
                                             : r_.read.resolve(i);
            if (!r_.names.empty() && r_.names.find(raw) == r_.names.end())
                return REJECT;
            return r_.strings.get_or_insert(raw);
        });
    }

    std::uint32_t read_host(std::uint32_t id) {
        return build_host_.get(id, [&](std::uint32_t i) {
            const std::string_view raw = i == dstd::StringIntern::NO_ID
                                             ? std::string_view{}
                                             : r_.read.resolve(i);
            if (!r_.hosts.empty() && r_.hosts.find(raw) == r_.hosts.end())
                return REJECT;
            return r_.strings.get_or_insert(raw);
        });
    }

    std::uint32_t arg_id(std::string_view key) {
        // Arg names are views into the read intern or the config, which keep
        // their bytes, so the address names the string.
        auto it = arg_ids_.find(key.data());
        if (it != arg_ids_.end() && it->second.length == key.size())
            return it->second.id;
        const std::uint32_t id = r_.strings.get_or_insert(key);
        arg_ids_[key.data()] = {static_cast<std::uint32_t>(key.size()), id};
        return id;
    }

    std::uint32_t extra_id(const agg::PodAccessor& acc) {
        extra_buf_.clear();
        bool any = false;
        bool first = true;
        for (const auto& k : r_.q.config.extra_group_keys) {
            if (!first) extra_buf_ += EXTRA_SEP;
            first = false;
            const auto v = acc.arg_string(k);
            if (!v.empty()) any = true;
            extra_buf_.append(v);
        }
        return any ? r_.strings.get_or_insert(extra_buf_)
                   : dstd::StringIntern::NO_ID;
    }

    void add_event(const FoldEvent& ev,
                   const dstd::HashSet<Group, GroupHash>* wanted);

    Runner& r_;
    Source& src_;
    IdCache root_cat_, root_name_, root_host_, root_extra_, root_arg_;
    IdCache build_cat_, build_name_, build_host_;
    dstd::Vector<Reread> rereads_;
    views::detail::FoldEventBuffer buffer_;
    dstd::HashMap<const void*, ArgId, dftracer::utils::PtrHash,
                  std::equal_to<const void*>>
        arg_ids_;
    dstd::String extra_buf_;
    json::RecordParser parser_;
    std::shared_ptr<std::string> buf_ = std::make_shared<std::string>();
};

void Runner::merge(Task& task) {
    std::lock_guard<std::mutex> lock(mu);
    for (auto& [key, partial] : task.rows) {
        auto it = out.rows.find(key);
        if (it == out.rows.end())
            out.rows.emplace(key, std::move(partial));
        else
            it->second.merge(partial);
    }
    auto& s = out.stats;
    const auto& t = task.stats;
    s.files += t.files;
    s.files_without_partials += t.files_without_partials;
    s.partials_read += t.partials_read;
    s.partials_merged += t.partials_merged;
    s.partials_pruned += t.partials_pruned;
    s.partials_reread += t.partials_reread;
    s.chunks_reread += t.chunks_reread;
    s.unanswered += t.unanswered;
    s.events_read += t.events_read;
    s.max_batch_events = std::max(s.max_batch_events, t.max_batch_events);
}

coro::CoroTask<void> Runner::run() {
    std::size_t workers = q.workers;
    if (workers == 0) {
        const Executor* ex = Executor::current();
        workers =
            ex != nullptr ? std::max<std::size_t>(1, ex->get_num_threads()) : 1;
    }
    permits = std::make_unique<coro::CoroSemaphore>(workers);
    const std::uint64_t default_params_hash = q.config.params_hash();
    for (const auto& s : q.sources)
        sources.push_back(std::make_unique<Source>(s, default_params_hash));

    co_await run_coro_scope([&](CoroScope& scope) -> coro::CoroTask<void> {
        for (std::size_t si = 0; si < q.sources.size(); ++si)
            for (std::size_t fi = 0; fi < q.sources[si].files.size(); ++fi)
                scope.spawn(
                    [this, si, fi](CoroScope&) { return file_task(si, fi); });
        co_return;
    });
    if (error) std::rethrow_exception(error);
    out.stats.groups = out.rows.size();
}

coro::CoroTask<void> Runner::file_task(std::size_t source, std::size_t file) {
    Source& src = *sources[source];
    const std::string path(q.sources[source].files[file]);
    dstd::Vector<ChunkJob> jobs;
    dstd::Vector<index::gzip::ChunkSpan> spans;
    {
        co_await permits->acquire(1);
        Permit permit(*permits);
        try {
            if (!is_failed()) {
                Task task(*this, src);
                co_await task.scan_file(path, jobs, spans);
                merge(task);
            }
        } catch (...) {
            fail(std::current_exception());
        }
    }
    if (jobs.empty() || is_failed()) co_return;
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
        if (is_failed()) co_return;
        Task task(*this, *source);
        co_await task.read_chunk(*path, spans[job->chunk], *job, spans);
        merge(task);
    } catch (...) {
        fail(std::current_exception());
    }
}

coro::CoroTask<void> Task::scan_file(
    const std::string& path, dstd::Vector<ChunkJob>& jobs,
    dstd::Vector<index::gzip::ChunkSpan>& spans) {
    const auto& idx = src_.idx;
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
        agg::tier::for_each_partial(
            *src_.db, fid,
            [&](std::uint32_t chunk, std::uint32_t cat_root,
                std::uint32_t name_root, std::uint32_t host_root,
                std::uint32_t extra_root, AggKind kind,
                std::span<const std::uint8_t> bytes) {
                ++seen;
                ++stats.partials_read;
                const std::uint32_t cat =
                    root_cat_.get(cat_root, [&](std::uint32_t id) {
                        return map_root(id, r_.categories);
                    });
                const std::uint32_t name = root_name_.get(
                    name_root,
                    [&](std::uint32_t id) { return map_root(id, r_.names); });
                const std::uint32_t host = root_host_.get(
                    host_root,
                    [&](std::uint32_t id) { return map_root(id, r_.hosts); });
                if (cat == REJECT || name == REJECT || host == REJECT) {
                    ++stats.partials_pruned;
                    return true;
                }
                const std::uint32_t extra =
                    root_extra_.get(extra_root, [&](std::uint32_t id) {
                        const auto text =
                            agg::tier::read_dictionary_entry(*src_.db, id);
                        if (!text)
                            throw DFTUtilsException::cat(
                                ErrorCode::Parse,
                                "aggregation query: dictionary id ", id,
                                " is missing in ", src_.path);
                        return text->empty() ? dstd::StringIntern::NO_ID
                                             : r_.strings.get_or_insert(*text);
                    });
                take_partial(chunk, {cat, name, host, extra, kind}, bytes);
                return true;
            });
    }

    if (seen == 0) {
        ++stats.files_without_partials;
        if (!r_.q.reread) {
            ++stats.unanswered;
            co_return;
        }
        co_await whole_file_chunks(path, spans.size(), jobs);
        co_return;
    }

    for (std::size_t i = 0; i < rereads_.size();) {
        ChunkJob job;
        job.chunk = rereads_[i].chunk;
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

// The chunks of a file with no partials: those the time range leaves.
coro::CoroTask<void> Task::whole_file_chunks(const std::string& path,
                                             std::size_t chunk_count,
                                             dstd::Vector<ChunkJob>& jobs) {
    dstd::Vector<std::uint64_t> chunks;
    bool pruned_to_list = false;
    if (r_.has_time) {
        index::plan::PruneRequest request;
        request.index_path = src_.path;
        request.file_path = path;
        request.db = &src_.idx;
        request.time_range = std::make_pair(
            static_cast<double>(r_.q.ts_begin),
            r_.q.ts_end == UINT64_MAX ? 0.0 : static_cast<double>(r_.q.ts_end));
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
        job.all = true;
        jobs.push_back(std::move(job));
    }
}

// Decides one stored partial: pruned, merged whole, or queued for a re-read
// of its chunk.
void Task::take_partial(std::uint32_t chunk, const Group& group,
                        std::span<const std::uint8_t> bytes) {
    if (r_.reread_only && !r_.has_time && r_.pids.empty()) {
        queue_reread(chunk, group);
        return;
    }
    agg::AggPartial::Fields keep;
    keep.quantiles = r_.q.quantiles && !r_.reread_only;
    keep.args = r_.q.args && !r_.reread_only;
    auto parsed = agg::AggPartial::deserialize(
        dstd::Span<const std::uint8_t>(bytes.data(), bytes.size()),
        r_.q.config.sketch_accuracy, keep);
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
    // Every event of the partial starts in [ts, ts_last_start].
    if (p.ts_last_start < r_.q.ts_begin || p.ts >= r_.q.ts_end) {
        ++stats.partials_pruned;
        return;
    }
    const PidFit fit = pid_fit(p);
    if (fit == PidFit::None) {
        ++stats.partials_pruned;
        return;
    }
    const bool inside = p.ts >= r_.q.ts_begin && p.ts_last_start < r_.q.ts_end;
    if (r_.reread_only || !inside || fit != PidFit::All ||
        (r_.q.quantiles && quantiles_missing(p))) {
        queue_reread(chunk, group);
        return;
    }

    AggKey key;
    key.cat = group.cat;
    key.name = group.name;
    if (r_.q.by_host) key.host = group.host;
    if (r_.q.by_extra) key.extra = group.extra;
    if (r_.q.by_kind) key.kind = group.kind;
    if (r_.q.by_pid) {
        const auto ids = p.pids.ids();
        if (!p.pids.is_exact() || ids.size() != 1) {
            queue_reread(chunk, group);
            return;
        }
        key.pid = ids[0];
    }
    if (r_.q.by_tid) {
        const auto ids = p.tids.ids();
        if (!p.tids.is_exact() || ids.size() != 1) {
            queue_reread(chunk, group);
            return;
        }
        key.tid = ids[0];
    }
    if (r_.q.window_us != 0) {
        key.window = r_.bucket(p.ts);
        if (r_.bucket(p.ts_last_start) != key.window) {
            queue_reread(chunk, group);
            return;
        }
    }

    if (r_.q.args && !p.args.empty()) {
        for (auto& a : p.args) a.id = map_root_arg(a.id);
        std::sort(p.args.begin(), p.args.end(),
                  [](const agg::ArgStat& a, const agg::ArgStat& b) {
                      return a.id < b.id;
                  });
    }
    ++stats.partials_merged;
    auto it = rows.find(key);
    if (it == rows.end())
        rows.emplace(key, std::move(p));
    else
        it->second.merge(p);
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

// Parses the complete lines of `pending` (through its last newline) as the
// events of `chunk` and leaves the rest. A line belongs to the chunk that
// holds its newline, as in the index fold.
void Task::parse_lines_of(std::string& pending, std::uint32_t chunk,
                          const dstd::HashSet<Group, GroupHash>* wanted) {
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
                if (r_.q.filter && !r_.q.filter->evaluate(rec.json)) return;
                FoldEvent& ev = buffer_.append();
                views::detail::build_fold_event_into(
                    ev, rec.ev, rec.args_dom, rec.has_args, r_.read, true);
            });
        stats.max_batch_events =
            std::max(stats.max_batch_events, buffer_.size());
        for (const FoldEvent& ev : buffer_.events()) add_event(ev, wanted);
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
    dstd::HashSet<Group, GroupHash> wanted;
    if (!job.all) wanted.insert(job.groups.begin(), job.groups.end());
    const dstd::HashSet<Group, GroupHash>* filter = job.all ? nullptr : &wanted;

    auto reader = co_await open_reader(path);
    auto stream = reader->stream(rd::StreamConfig()
                                     .stream_type(rd::StreamType::Bytes)
                                     .range_type(rd::RangeType::ByteRange)
                                     .buffer_size(STREAM_BUFFER_BYTES)
                                     .from(span.uc_offset)
                                     .to(span.uc_offset + span.uc_size));
    std::string pending;
    bool decided = job.chunk == 0;
    while (!stream->done()) {
        auto piece = co_await stream->read_async();
        if (piece.empty()) break;
        pending.append(piece.data(), piece.size());
        if (!decided) {
            const std::size_t nl = pending.find('\n');
            if (nl == std::string::npos) continue;
            decided = true;
            // The head of the chunk is a whole line only when it parses as
            // one object: the tail of a line cut by the member boundary has a
            // brace the head does not close. Any other head, and an empty one,
            // continues a line of the chunks before.
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

void Task::add_event(const FoldEvent& ev,
                     const dstd::HashSet<Group, GroupHash>* wanted) {
    const agg::PodAccessor acc(ev, r_.read);
    if (!agg::partial_holds(acc)) return;
    if (ev.ts < r_.q.ts_begin || ev.ts >= r_.q.ts_end) return;
    if (!r_.pids.empty() &&
        !std::binary_search(r_.pids.begin(), r_.pids.end(), ev.pid))
        return;
    const std::uint32_t cat = read_cat(ev.cat_id);
    if (cat == REJECT) return;
    const std::uint32_t name = read_name(ev.name_id);
    if (name == REJECT) return;
    const std::uint32_t host = read_host(ev.hhash_id);
    if (host == REJECT) return;
    const std::uint32_t extra = extra_id(acc);
    const AggKind kind = agg::partial_kind(acc);
    if (wanted != nullptr &&
        wanted->find(Group{cat, name, host, extra, kind}) == wanted->end())
        return;

    AggKey key;
    key.cat = cat;
    key.name = name;
    if (r_.q.by_host) key.host = host;
    if (r_.q.by_kind) key.kind = kind;
    if (r_.q.by_pid) key.pid = ev.pid;
    if (r_.q.by_tid) key.tid = ev.tid;
    if (r_.q.by_file) {
        const auto fhash = acc.arg_string("fhash");
        key.file = fhash.empty() ? 0 : agg::agg_hash_of_hex(fhash);
        if (!fhash.empty() && !hash::parse_hex64(fhash))
            key.file_text = r_.strings.get_or_insert(fhash);
    }
    if (r_.q.window_us != 0) key.window = r_.bucket(ev.ts);
    if (r_.q.by_extra) key.extra = extra;

    auto it = rows.find(key);
    if (it == rows.end())
        it = rows.emplace(key, agg::AggPartial(r_.q.config.sketch_accuracy))
                 .first;
    auto id_of = [this](std::string_view k) { return arg_id(k); };
    agg::fold_into_partial(it->second, acc, r_.read_cfg, id_of);
    ++stats.events_read;
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
