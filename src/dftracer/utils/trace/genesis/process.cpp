#include <dftracer/utils/core/common/base64.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/external_sort.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/memory_pool.h>
#include <dftracer/utils/core/common/spill_file.h>
#include <dftracer/utils/core/common/str_format.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/core/coro/async_semaphore.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/coro/yield.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/dataframe/sketch.h>
#include <dftracer/utils/trace/genesis/genesis.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <simdjson.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::genesis {

namespace {

using Sketch = dataframe::BasicDDSketch<2048>;
using utilities::fileio::lines::sources::async_streaming_gz_lines;

enum class Kind { DELTA, GAUGE };
enum class Scope { PID, HOST };

struct Series {
    Kind kind = Kind::DELTA;
    std::vector<std::pair<std::int64_t, double>> samples;
};

struct Proc {
    std::string hhash;
    std::int64_t pid = 0;
    std::int64_t start = -1;
    std::int64_t end = -1;
    bool active = false;
    int set = -1;
    std::vector<std::string> papi_names;
    std::map<std::string, Series> papi;
};

// One call as read from a file; names are ids into the reader's own strings.
struct Call {
    std::int64_t tid;
    std::int64_t host_tid;  // args.tid, or -1
    std::int64_t ts;
    std::int64_t dur;
    std::uint32_t proc;
    std::uint32_t name;
    std::uint32_t cat;
    bool gpu;  // carries args.tid or args.correlation_id
    std::uint8_t pad[3]{};
};
static_assert(std::has_unique_object_representations_v<Call>);

// One call with group-wide ids and its place in the sorted walk.
struct SortRec {
    std::int64_t lane;
    std::int64_t ts;
    std::int64_t dur;
    std::uint64_t seq;
    std::uint32_t proc;
    std::uint32_t name;
    std::uint32_t cat;
    std::uint8_t group;  // 0: outside its process window, 1: in a lane
    std::uint8_t kind;   // 0: host call, 1: attached (GPU) call
    std::uint8_t pad[2]{};
};
static_assert(std::has_unique_object_representations_v<SortRec>);

// Out-of-window calls come first, in file order. In-window calls follow, lane
// by lane; within a lane a host call precedes an attached call that starts at
// the same time, and longer calls precede shorter ones.
struct SortLess {
    bool operator()(const SortRec& a, const SortRec& b) const {
        if (a.group != b.group) return a.group < b.group;
        if (a.group == 0) return a.seq < b.seq;
        if (a.proc != b.proc) return a.proc < b.proc;
        if (a.lane != b.lane) return a.lane < b.lane;
        if (a.ts != b.ts) return a.ts < b.ts;
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.dur != b.dur) return a.dur > b.dur;
        return a.seq < b.seq;
    }
};

struct Stats {
    double min = std::numeric_limits<double>::infinity();
    double max = -std::numeric_limits<double>::infinity();
    double sum = 0.0;
    std::uint64_t n = 0;
    Sketch sketch{SKETCH_ACCURACY};

    // The bytes the sketch store grew by, for the caller to charge.
    [[nodiscard]] std::uint64_t add(double v) {
        min = std::min(min, v);
        max = std::max(max, v);
        sum += v;
        ++n;
        const std::size_t before = sketch.store_bins();
        sketch.add(v);
        return (sketch.store_bins() - before) * sizeof(std::uint32_t);
    }
};

struct CounterAcc {
    Scope scope;
    Kind kind;
    Stats stats;
};

struct PathRec {
    std::string path;
    std::uint32_t name;
    std::uint32_t cat;
    std::int64_t depth;
    std::int64_t first_ts = std::numeric_limits<std::int64_t>::max();
    Stats dur;
    std::vector<std::unique_ptr<CounterAcc>> counters;
};

// Calls per chunk, and the bytes one open reader holds besides its chunk
// (parser, inflate buffers, line buffers).
constexpr std::size_t CHUNK_CALLS = 16384;
constexpr std::uint64_t READER_BYTES = 8ULL * 1024 * 1024;
constexpr std::uint64_t CHARGE_STEP = 64ULL * 1024;
constexpr std::uint64_t SERIES_SAMPLE_BYTES =
    sizeof(std::pair<std::int64_t, double>);

struct GroupError {
    ErrorCode code = ErrorCode::PARSE;
    std::string file;
    std::string reason;
};

template <class T>
using GResult = expected<T, GroupError>;

unexpected<GroupError> fail(std::string file, std::string reason,
                            ErrorCode code = ErrorCode::PARSE) {
    return unexpected<GroupError>(
        GroupError{code, std::move(file), std::move(reason)});
}

unexpected<GroupError> fail_from(const DFTUtilsError& e) {
    return fail("", e.message, e.code);
}

// A group's memory is its share of the budget in four equal parts: the calls
// kept in memory between reading and sorting, the sort buffer, the state
// (counter series, path records), and the open readers (a semaphore).
struct GroupMem {
    explicit GroupMem(std::uint64_t share_bytes)
        : share(share_bytes),
          calls(share_bytes / 4),
          state(share_bytes / 4),
          sort_budget(share_bytes / 4) {}

    std::uint64_t share;
    MemoryPool calls;
    MemoryPool state;
    std::uint64_t sort_budget;

    std::uint64_t spilled_bytes() const { return file_ ? file_->size() : 0; }

    GResult<SpillFile*> spill_file() {
        std::lock_guard<std::mutex> lock(mu_);
        if (!file_) {
            auto created = SpillFile::create();
            if (!created) return fail_from(created.error());
            file_ = std::move(*created);
        }
        return file_.get();
    }

    GroupError budget_error(std::string_view what) const {
        return GroupError{ErrorCode::INVALID_ARGUMENT, "",
                          str_cat("memory budget exceeded by ", what,
                                  " (the share of this run group is ", share,
                                  " bytes); raise --memory-budget")};
    }

    unexpected<GroupError> over_budget(std::string_view what) const {
        return unexpected<GroupError>(budget_error(what));
    }

   private:
    std::mutex mu_;
    std::unique_ptr<SpillFile> file_;
};

// Charges a pool in steps, so a hot loop does not touch the shared counter for
// every item.
class Charge {
   public:
    explicit Charge(GroupMem& mem) noexcept : mem_(&mem), res_(mem.state) {}

    // The budget error naming `what` when the state pool cannot take `n` more
    // bytes.
    GResult<void> take(std::uint64_t n, std::string_view what) noexcept {
        used_ += n;
        if (used_ <= res_.bytes()) return {};
        const std::uint64_t need = used_ - res_.bytes();
        if (res_.try_grow(std::max(need, CHARGE_STEP)) || res_.try_grow(need))
            return {};
        return mem_->over_budget(what);
    }

   private:
    const GroupMem* mem_;
    Reservation res_;
    std::uint64_t used_ = 0;
};

std::int64_t get_i64(simdjson::dom::element e) {
    std::int64_t i = 0;
    if (e.get(i) == simdjson::SUCCESS) return i;
    std::uint64_t u = 0;
    if (e.get(u) == simdjson::SUCCESS) return static_cast<std::int64_t>(u);
    double d = 0;
    if (e.get(d) == simdjson::SUCCESS) return static_cast<std::int64_t>(d);
    return 0;
}

std::optional<double> get_number(simdjson::dom::element e) {
    double d = 0;
    if (e.is_number() && e.get(d) == simdjson::SUCCESS) return d;
    return std::nullopt;
}

std::string_view get_sv(simdjson::dom::object o, std::string_view key) {
    std::string_view v;
    if (o[key].get(v) != simdjson::SUCCESS) return {};
    return v;
}

bool is_per_core_cpu(std::string_view name) {
    return name.size() > 4 && name.substr(0, 4) == "cpu-";
}

std::string strip_spaces(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != ' ') out.push_back(c);
    return out;
}

// Sample i carries the value for (t_{i-1}, t_i]; sample 0 has no known start.
// The calls of a lane come in ts order, so a series is searched from where the
// last call of the lane left it: `Cursor::at` only moves forward while `a`
// does, by an exponential search, instead of a binary search over the whole
// series per call.
struct Cursor {
    std::size_t at = 0;
    std::int64_t last = std::numeric_limits<std::int64_t>::min();

    // The first index from `at` whose time passes `a` (or reaches it, when
    // `inclusive`), as std::upper_bound (std::lower_bound) over the series.
    template <bool INCLUSIVE>
    std::size_t seek(const std::vector<std::pair<std::int64_t, double>>& x,
                     std::int64_t a) {
        if (a < last) at = 0;
        last = a;
        const auto before = [a](const auto& p) {
            return INCLUSIVE ? p.first < a : p.first <= a;
        };
        std::size_t step = 1;
        std::size_t hi = at;
        while (hi < x.size() && before(x[hi])) {
            at = hi + 1;
            hi += step;
            step *= 2;
        }
        hi = std::min(hi, x.size());
        at = static_cast<std::size_t>(
            std::partition_point(x.begin() + static_cast<std::ptrdiff_t>(at),
                                 x.begin() + static_cast<std::ptrdiff_t>(hi),
                                 before) -
            x.begin());
        return at;
    }
};

std::optional<double> prorate(const Series& s, std::int64_t a, std::int64_t b,
                              Cursor& cur) {
    const auto& x = s.samples;
    if (x.size() < 2 || b < x.front().first || a > x.back().first)
        return std::nullopt;
    std::size_t i = std::max<std::size_t>(1, cur.seek<false>(x, a));
    double sum = 0.0;
    for (; i < x.size() && x[i - 1].first < b; ++i) {
        const std::int64_t len = x[i].first - x[i - 1].first;
        const std::int64_t lo = std::max(a, x[i - 1].first);
        const std::int64_t hi = std::min(b, x[i].first);
        if (len > 0 && hi > lo)
            sum += x[i].second * static_cast<double>(hi - lo) /
                   static_cast<double>(len);
    }
    return sum;
}

std::optional<double> weighted_mean(const Series& s, std::int64_t a,
                                    std::int64_t b, Cursor& cur) {
    const auto& x = s.samples;
    if (x.size() < 2 || b <= x.front().first || a > x.back().first)
        return std::nullopt;
    std::size_t i = std::max<std::size_t>(1, cur.seek<true>(x, a));
    if (b == a) return x[i].second;
    double num = 0.0;
    double den = 0.0;
    for (; i < x.size() && x[i - 1].first < b; ++i) {
        const std::int64_t lo = std::max(a, x[i - 1].first);
        const std::int64_t hi = std::min(b, x[i].first);
        if (hi > lo) {
            num += x[i].second * static_cast<double>(hi - lo);
            den += static_cast<double>(hi - lo);
        }
    }
    if (den == 0.0) return std::nullopt;
    return num / den;
}

using simdjson::builder::string_builder;

void append_stats(string_builder& sb, const Stats& s, bool with_n,
                  bool with_sum, std::vector<std::uint8_t>& buf) {
    sb.start_object();
    if (with_n) {
        sb.append_key_value("n", s.n);
        sb.append_comma();
    }
    sb.append_key_value("min", s.min);
    sb.append_comma();
    sb.append_key_value("max", s.max);
    if (with_sum) {
        sb.append_comma();
        sb.append_key_value("sum", s.sum);
    }
    sb.append_comma();
    sb.append_key_value("avg", s.sum / static_cast<double>(s.n));
    static constexpr std::pair<std::string_view, double> QS[] = {{"p25", 0.25},
                                                                 {"p50", 0.5},
                                                                 {"p75", 0.75},
                                                                 {"p90", 0.9},
                                                                 {"p99", 0.99}};
    for (const auto& [k, q] : QS) {
        sb.append_comma();
        sb.append_key_value(k, std::clamp(s.sketch.quantile(q), s.min, s.max));
    }
    s.sketch.serialize_into(buf);
    sb.append_comma();
    sb.append_key_value(
        "sketch", std::string_view(base64_encode(buf.data(), buf.size())));
    sb.end_object();
}

class GroupReader {
   public:
    struct Chunk {
        std::vector<Call> mem;
        Reservation res;
        std::uint64_t offset = 0;
        std::uint64_t count = 0;
        bool spilled = false;
    };

    // Ids of one file's strings and processes in the group's tables.
    struct IdMap {
        std::vector<std::uint32_t> sid;
        std::vector<std::uint32_t> pid;
    };

    explicit GroupReader(GroupMem& mem) : mem_(&mem), charge_(mem) {}

    coro::CoroTask<GResult<void>> read(std::string file) {
        std::optional<std::string> error;
        try {
            // An unfinished run is skipped, not counted from a recovered tail.
            auto gen = async_streaming_gz_lines(file, 0, 0,
                                                /*recover_truncated=*/false);
            std::size_t n = 0;
            while (auto line = co_await gen.next()) {
                auto st = handle_line(file, line->content);
                if (!st) co_return st;
                if (++n % 4096 == 0) co_await coro::maybe_yield();
            }
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (error) co_return fail(file, *error);
        co_return seal(true);
    }

    // Merge another file's tables into this one; files are absorbed in file
    // order so call sequence numbers stay deterministic. Its calls stay in its
    // own chunks.
    IdMap absorb(GroupReader& o) {
        IdMap m;
        m.sid.resize(o.strings_.size());
        for (std::size_t i = 0; i < o.strings_.size(); ++i)
            m.sid[i] = intern(o.strings_[i]);
        m.pid.resize(o.procs_.size());
        for (std::size_t i = 0; i < o.procs_.size(); ++i) {
            Proc& src = o.procs_[i];
            m.pid[i] = proc_index(src.hhash, src.pid);
            Proc& dst = procs_[m.pid[i]];
            if (src.start >= 0) dst.start = src.start;
            if (src.end >= 0) dst.end = src.end;
            dst.active = dst.active || src.active;
            for (const std::string& name : src.papi_names)
                add_papi_name(dst, name);
            for (auto& [k, series] : src.papi) append(dst.papi[k], series);
        }
        for (auto& [hh, h] : o.hosts_)
            for (auto& [k, series] : h) append(host_series(hh)[k], series);
        host_hash_.merge(o.host_hash_);
        for (const auto& [p, tid] : o.gpu_threads_)
            gpu_threads_.emplace(m.pid[p], tid);
        return m;
    }

    void finish() {
        for (auto& [_, h] : hosts_)
            for (auto& [__, s] : h) sort_series(s);
        for (auto& p : procs_) {
            for (auto& [_, s] : p.papi) sort_series(s);
            std::sort(p.papi_names.begin(), p.papi_names.end());
            p.papi_names.erase(
                std::unique(p.papi_names.begin(), p.papi_names.end()),
                p.papi_names.end());
        }
    }

    GResult<void> load_gpu_csv(const std::vector<std::string>& files) {
        static constexpr const char* FIELDS[] = {
            "gpu.power.GPU_", "gpu.utilization.GPU_", "gpu.memory_used.GPU_"};
        for (const auto& f : files) {
            std::ifstream in(f);
            std::string line;
            try {
                while (std::getline(in, line)) {
                    std::vector<std::string_view> cols;
                    std::string_view rest(line);
                    while (true) {
                        const auto c = rest.find(',');
                        std::string_view col = rest.substr(0, c);
                        while (!col.empty() && col.front() == ' ')
                            col.remove_prefix(1);
                        while (!col.empty() && col.back() == ' ')
                            col.remove_suffix(1);
                        cols.push_back(col);
                        if (c == std::string_view::npos) break;
                        rest.remove_prefix(c + 1);
                    }
                    if (cols.size() != 6)
                        return fail(f, "GPU CSV row without 6 columns");
                    const auto hh = host_hash_.find(std::string(cols[1]));
                    if (hh == host_hash_.end())
                        return fail(f, "no HH metadata for host " +
                                           std::string(cols[1]));
                    const double seconds = std::stod(std::string(cols[0]));
                    const auto ts =
                        static_cast<std::int64_t>(std::llround(seconds * 1e6));
                    auto& h = host_series(hh->second);
                    for (std::size_t k = 0; k < 3; ++k) {
                        auto& s = h[FIELDS[k] + std::string(cols[2])];
                        s.kind = Kind::GAUGE;
                        if (auto st = add_sample(
                                s, ts, std::stod(std::string(cols[3 + k])));
                            !st)
                            return st;
                    }
                }
            } catch (const std::exception& e) {
                return fail(f, e.what());
            }
        }
        for (auto& [_, h] : hosts_)
            for (auto& [__, s] : h) sort_series(s);
        return {};
    }

    std::vector<Proc> procs_;
    std::vector<std::string> strings_;
    StringViewMap<std::map<std::string, Series>> hosts_;
    std::map<std::string, std::string> host_hash_;
    std::vector<Chunk> chunks_;
    std::set<std::pair<std::uint32_t, std::int64_t>> gpu_threads_;

   private:
    // The samples stay charged to the reader they were read by, which lives
    // until the group is done, so moving them counts them once.
    static void append(Series& dst, Series& src) {
        dst.kind = src.kind;
        if (dst.samples.empty()) {
            dst.samples = std::move(src.samples);
            return;
        }
        dst.samples.insert(dst.samples.end(), src.samples.begin(),
                           src.samples.end());
        std::vector<std::pair<std::int64_t, double>>().swap(src.samples);
    }

    static void sort_series(Series& s) {
        std::stable_sort(
            s.samples.begin(), s.samples.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
    }

    std::map<std::string, Series>& host_series(std::string_view hhash) {
        auto it = hosts_.find(hhash);
        if (it == hosts_.end())
            it = hosts_.try_emplace(std::string(hhash)).first;
        return it->second;
    }

    std::uint32_t intern(std::string_view s) {
        auto it = string_ids_.find(s);
        if (it != string_ids_.end()) return it->second;
        const auto id = static_cast<std::uint32_t>(strings_.size());
        strings_.emplace_back(s);
        string_ids_.emplace(std::string(s), id);
        return id;
    }

    std::uint32_t proc_index(std::string_view hhash, std::int64_t pid) {
        auto host = proc_ids_.find(hhash);
        if (host == proc_ids_.end())
            host = proc_ids_.try_emplace(std::string(hhash)).first;
        auto [it, fresh] = host->second.try_emplace(
            pid, static_cast<std::uint32_t>(procs_.size()));
        if (fresh) {
            Proc p;
            p.hhash = std::string(hhash);
            p.pid = pid;
            procs_.push_back(std::move(p));
        }
        return it->second;
    }

    // Charges one sample against the group's series pool, then adds it.
    GResult<void> add_sample(Series& series, std::int64_t ts, double value) {
        if (auto st = charge_.take(SERIES_SAMPLE_BYTES, "counter series"); !st)
            return st;
        series.samples.emplace_back(ts, value);
        return {};
    }

    // A counter name once; a process has a few dozen, one record names each.
    static void add_papi_name(Proc& p, std::string_view name) {
        if (std::find(p.papi_names.begin(), p.papi_names.end(), name) ==
            p.papi_names.end())
            p.papi_names.emplace_back(name);
    }

    // The open chunk stays in memory while the group's call pool has room,
    // else it goes to the group's spill file. After the last chunk of a file
    // its buffer is freed, so a reader waiting for its group holds no calls.
    GResult<void> seal(bool last = false) {
        if (cur_.empty()) return {};
        Chunk c;
        c.count = cur_.size();
        const std::uint64_t bytes = c.count * sizeof(Call);
        c.res = Reservation(mem_->calls);
        if (c.res.try_grow(bytes)) {
            c.mem = std::move(cur_);
            if (last) c.mem.shrink_to_fit();
            cur_ = {};
        } else {
            auto file = mem_->spill_file();
            if (!file) return unexpected(file.error());
            auto at = (*file)->append(cur_.data(), bytes);
            if (!at) return fail_from(at.error());
            c.spilled = true;
            c.offset = *at;
            cur_.clear();
            if (last) cur_.shrink_to_fit();
        }
        chunks_.push_back(std::move(c));
        return {};
    }

    GResult<void> handle_line(const std::string& file, std::string_view line) {
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        while (!line.empty() && (line.back() == ',' || line.back() == ' ' ||
                                 line.back() == '\r'))
            line.remove_suffix(1);
        if (line.empty() || line.front() != '{') return {};
        simdjson::dom::object o;
        if (parser_.parse(line.data(), line.size()).get(o) != simdjson::SUCCESS)
            return fail(file, "invalid JSON line");
        simdjson::dom::element ph_el;
        if (o["ph"].get(ph_el) != simdjson::SUCCESS) return {};
        const RecordPhase ph = read_phase(ph_el);
        const std::string_view cat = get_sv(o, "cat");
        const std::string_view name = get_sv(o, "name");
        simdjson::dom::object args;
        const bool has_args = o["args"].get(args) == simdjson::SUCCESS;
        const std::string_view hhash = has_args ? get_sv(args, "hhash") : "";
        simdjson::dom::element e;
        const std::int64_t pid =
            o["pid"].get(e) == simdjson::SUCCESS ? get_i64(e) : 0;
        const std::int64_t ts =
            o["ts"].get(e) == simdjson::SUCCESS ? get_i64(e) : 0;

        switch (ph) {
            case RecordPhase::COMPLETE: {
                const std::uint32_t p = proc_index(hhash, pid);
                if (cat == "dftracer") {
                    if (name == "start") procs_[p].start = ts;
                    if (name == "end") procs_[p].end = ts;
                    return {};
                }
                const std::int64_t tid =
                    o["tid"].get(e) == simdjson::SUCCESS ? get_i64(e) : 0;
                std::int64_t host_tid = -1;
                if (has_args && args["tid"].get(e) == simdjson::SUCCESS)
                    host_tid = get_i64(e);
                const bool gpu = host_tid >= 0 ||
                                 (has_args && args["correlation_id"].error() ==
                                                  simdjson::SUCCESS);
                const std::int64_t dur =
                    o["dur"].get(e) == simdjson::SUCCESS ? get_i64(e) : 0;
                procs_[p].active = true;
                if (gpu) gpu_threads_.emplace(p, tid);
                if (cur_.capacity() == 0) cur_.reserve(CHUNK_CALLS);
                cur_.push_back({tid, host_tid, ts, dur, p, intern(name),
                                intern(cat), gpu});
                if (cur_.size() >= CHUNK_CALLS) return seal();
                return {};
            }
            case RecordPhase::COUNTER:
                if (!has_args) return {};
                if (pid == 0)
                    return handle_host(file, cat, name, hhash, ts, args);
                if (cat == "papi") {
                    Proc& p = procs_[proc_index(hhash, pid)];
                    p.active = true;
                    for (auto [k, v] : args) {
                        const auto num = get_number(v);
                        if (!num) continue;
                        static constexpr std::string_view SUFFIX = "_delta";
                        if (k.size() > SUFFIX.size() &&
                            k.substr(k.size() - SUFFIX.size()) == SUFFIX) {
                            auto& s = p.papi[std::string(
                                k.substr(0, k.size() - SUFFIX.size()))];
                            if (auto st = add_sample(s, ts, *num); !st)
                                return st;
                        } else if (k.substr(0, 5) == "PAPI_") {
                            add_papi_name(p, k);
                        }
                    }
                    return {};
                }
                return fail(file, "unknown per-process counter category " +
                                      std::string(cat));
            case RecordPhase::METADATA:
                if (name == "HH" && has_args)
                    host_hash_[std::string(get_sv(args, "name"))] =
                        std::string(get_sv(args, "value"));
                return {};
            default:
                return {};
        }
    }

    GResult<void> handle_host(const std::string& file, std::string_view cat,
                              std::string_view name, std::string_view hhash,
                              std::int64_t ts, simdjson::dom::object args) {
        const bool sys = cat == "sys";
        const bool net = cat == "net";
        const bool io = cat == "io";
        const bool gpu = cat == "gpu";
        if (!sys && !net && !io && !gpu)
            return fail(file,
                        "unknown host counter category " + std::string(cat));
        const std::string series_name = strip_spaces(name);
        if (sys && is_per_core_cpu(series_name)) return {};
        auto& host = host_series(hhash);
        for (auto [k, v] : args) {
            if (k == "hhash" || k == "num_gpus_per_socket") continue;
            const auto num = get_number(v);
            if (!num) continue;
            std::string key = gpu ? "gpu." : "";
            key += series_name;
            key += '.';
            key += k;
            auto& s = host[key];
            s.kind = (net || (io && k != "ios_in_progress")) ? Kind::DELTA
                                                             : Kind::GAUGE;
            if (auto st = add_sample(s, ts, *num); !st) return st;
        }
        return {};
    }

    GroupMem* mem_;
    Charge charge_;
    std::vector<Call> cur_;
    simdjson::dom::parser parser_;
    StringViewMap<std::uint32_t> string_ids_;
    StringViewMap<ankerl::unordered_dense::map<std::int64_t, std::uint32_t>>
        proc_ids_;
};

void append_run_keys(string_builder& sb, std::string_view id,
                     const RunKeys& k) {
    sb.append_key_value("run", id);
    sb.append_comma();
    sb.append_key_value("app", std::string_view(k.app));
    sb.append_comma();
    sb.append_key_value("system", std::string_view(k.system));
    sb.append_comma();
    sb.append_key_value("unique_input", std::string_view(k.unique_input));
    sb.append_comma();
    sb.append_key_value("nodes", k.nodes);
    sb.append_comma();
    sb.append_key_value("ppn", k.ppn);
    sb.append_comma();
    sb.append_key_value("papi_set", std::string_view(k.papi_set));
}

// One PAPI set's accumulator, fed the group's calls in sorted order. A set
// that fails a check keeps its error and ignores the rest of the walk.
class SetAcc {
   public:
    SetAcc(const GroupReader& r, const RunGroup& g, int set, GroupMem& mem)
        : r_(&r),
          g_(&g),
          set_(set),
          charge_(mem),
          spec_(&g.sets[static_cast<std::size_t>(set)]),
          ppn_(static_cast<double>(spec_->keys.ppn)) {
        auto st = check();
        if (!st) failed_ = st.error();
    }

    bool ok() const { return !failed_; }
    const std::optional<GroupError>& error() const { return failed_; }

    void feed(const SortRec& s) {
        if (failed_) return;
        if (s.group == 0) {
            const auto id = record(0, s.name, s.cat);
            if (!id) return;
            PathRec& rec = *recs_[*id];
            rec.first_ts = std::min(rec.first_ts, s.ts);
            charge_growth(rec.dur.add(static_cast<double>(s.dur)),
                          "duration statistics");
            return;
        }
        if (!lane_open_ || s.proc != lane_proc_ || s.lane != lane_id_)
            open_lane(s);
        if (s.kind != 0) {
            add(s);
            return;
        }
        const std::int64_t end = s.ts + s.dur;
        pop(s.ts);
        if (!stack_.empty() && end > stack_.back().first) {
            failed_ =
                GroupError{ErrorCode::PARSE, spec_->keys.papi_set,
                           str_cat("call ", r_->strings_[s.name], " at ts ",
                                   s.ts, " ends after its parent ",
                                   recs_[stack_.back().second]->path)};
            return;
        }
        if (const auto id = add(s)) stack_.emplace_back(end, *id);
    }

    RunOutput finish() {
        const RunKeys& keys = spec_->keys;
        const std::string id = run_id(keys);
        string_builder sb;
        sb.append_raw(R"({"gtype":"run","version":1,)");
        append_run_keys(sb, id, keys);
        sb.append_comma();
        sb.append_key_value("method", std::string_view("prorate"));
        sb.append_comma();
        sb.append_key_value("sketch_accuracy", SKETCH_ACCURACY);
        sb.append_comma();
        sb.append_key_value("leaf", std::string_view(g_->rel_dir));
        if (!g_->summary_json.empty()) {
            sb.append_comma();
            sb.escape_and_append_with_quotes("summary");
            sb.append_colon();
            sb.append_raw(g_->summary_json);
        }
        sb.append_raw("}\n");

        std::vector<std::uint32_t> counter_order(counter_keys_.size());
        for (std::uint32_t i = 0; i < counter_order.size(); ++i)
            counter_order[i] = i;
        std::sort(counter_order.begin(), counter_order.end(),
                  [&](std::uint32_t a, std::uint32_t b) {
                      return counter_keys_[a] < counter_keys_[b];
                  });
        std::sort(recs_.begin(), recs_.end(), [](const auto& a, const auto& b) {
            return a->path < b->path;
        });
        std::vector<std::uint8_t> buf;
        for (const auto& rec_ptr : recs_) {
            const PathRec& rec = *rec_ptr;
            const std::string_view name = r_->strings_[rec.name];
            sb.append_raw(R"({"gtype":"func",)");
            sb.append_key_value("run", std::string_view(id));
            sb.append_comma();
            sb.append_key_value("path", std::string_view(rec.path));
            const auto semi = rec.path.rfind(';');
            if (semi != std::string::npos) {
                sb.append_comma();
                sb.append_key_value("parent",
                                    std::string_view(rec.path).substr(0, semi));
            }
            sb.append_comma();
            sb.append_key_value("name", name);
            sb.append_comma();
            sb.append_key_value("cat", std::string_view(r_->strings_[rec.cat]));
            sb.append_comma();
            sb.append_key_value("depth", rec.depth);
            sb.append_comma();
            sb.append_key_value("ts", rec.first_ts);
            sb.append_comma();
            sb.append_key_value("count", rec.dur.n);
            sb.append_comma();
            sb.escape_and_append_with_quotes("dur");
            sb.append_colon();
            append_stats(sb, rec.dur, false, true, buf);
            sb.append_raw("}\n");
            for (const std::uint32_t cid : counter_order) {
                if (cid >= rec.counters.size() || !rec.counters[cid]) continue;
                const CounterAcc& acc = *rec.counters[cid];
                sb.append_raw(R"({"gtype":"counter",)");
                sb.append_key_value("run", std::string_view(id));
                sb.append_comma();
                sb.append_key_value("path", std::string_view(rec.path));
                sb.append_comma();
                sb.append_key_value("name", name);
                sb.append_comma();
                sb.append_key_value("metric",
                                    std::string_view(counter_keys_[cid]));
                sb.append_raw(acc.scope == Scope::PID ? R"(,"scope":"pid")"
                                                      : R"(,"scope":"host")");
                sb.append_raw(acc.kind == Kind::DELTA
                                  ? R"(,"kind":"delta","v":)"
                                  : R"(,"kind":"gauge","v":)");
                append_stats(sb, acc.stats, true, acc.kind == Kind::DELTA, buf);
                sb.append_raw("}\n");
            }
        }
        return {g_->dir + '\0' + keys.papi_set, std::string(sb)};
    }

   private:
    struct Source {
        std::uint32_t id;
        const Series* series;
        Scope scope;
        Cursor cursor{};
    };

    GResult<void> check() const {
        std::vector<std::uint32_t> procs;
        for (std::uint32_t i = 0; i < r_->procs_.size(); ++i)
            if (r_->procs_[i].active && r_->procs_[i].set == set_)
                procs.push_back(i);
        const auto want = spec_->keys.nodes * spec_->keys.ppn;
        if (static_cast<std::int64_t>(procs.size()) != want)
            return fail(
                spec_->keys.papi_set,
                str_cat("expected ", want, " processes, found ", procs.size()));
        for (const auto i : procs) {
            const Proc& p = r_->procs_[i];
            if (p.start < 0 || p.end < 0)
                return fail(spec_->keys.papi_set,
                            str_cat("process ", p.hhash, ":", p.pid,
                                    " lacks dftracer start or end"));
        }
        return {};
    }

    void fail_with(GroupError error) {
        failed_ = std::move(error);
        failed_->file = spec_->keys.papi_set;
    }

    void pop(std::int64_t ts) {
        while (!stack_.empty() && stack_.back().first <= ts) stack_.pop_back();
    }

    std::uint32_t counter_id(const std::string& k) {
        auto [it, fresh] = counter_ids_.try_emplace(
            k, static_cast<std::uint32_t>(counter_keys_.size()));
        if (fresh) counter_keys_.push_back(k);
        return it->second;
    }

    void open_lane(const SortRec& s) {
        lane_open_ = true;
        lane_proc_ = s.proc;
        lane_id_ = s.lane;
        stack_.clear();
        sources_.clear();
        const Proc& p = r_->procs_[s.proc];
        for (const auto& [k, series] : p.papi)
            sources_.push_back({counter_id(k), &series, Scope::PID});
        if (const auto h = r_->hosts_.find(p.hhash); h != r_->hosts_.end())
            for (const auto& [k, series] : h->second)
                sources_.push_back({counter_id(k), &series, Scope::HOST});
    }

    // Parent index + 1 (0 at the root) and the name id identify a path.
    std::optional<std::uint32_t> record(std::uint32_t parent_plus_one,
                                        std::uint32_t name, std::uint32_t cat) {
        const std::uint64_t key =
            (static_cast<std::uint64_t>(parent_plus_one) << 32) | name;
        const auto found = children_.find(key);
        if (found != children_.end()) return found->second;
        auto rec = std::make_unique<PathRec>();
        if (parent_plus_one) {
            rec->path = recs_[parent_plus_one - 1]->path + ";";
            rec->depth = recs_[parent_plus_one - 1]->depth + 1;
        } else {
            rec->depth = 0;
        }
        rec->path += r_->strings_[name];
        rec->name = name;
        rec->cat = cat;
        if (auto st = charge_.take(
                sizeof(PathRec) + rec->path.size() + PATH_NODE_BYTES,
                "path records");
            !st) {
            fail_with(st.error());
            return std::nullopt;
        }
        const auto id = static_cast<std::uint32_t>(recs_.size());
        recs_.push_back(std::move(rec));
        children_.emplace(key, id);
        return id;
    }

    std::optional<std::uint32_t> add(const SortRec& s) {
        const std::int64_t end = s.ts + s.dur;
        pop(s.ts);
        const auto id = record(stack_.empty() ? 0 : stack_.back().second + 1,
                               s.name, s.cat);
        if (!id) return std::nullopt;
        PathRec& rec = *recs_[*id];
        rec.first_ts = std::min(rec.first_ts, s.ts);
        if (!charge_growth(rec.dur.add(static_cast<double>(s.dur)),
                           "duration statistics"))
            return std::nullopt;
        for (Source& src : sources_) {
            const Series& series = *src.series;
            const auto v = series.kind == Kind::DELTA
                               ? prorate(series, s.ts, end, src.cursor)
                               : weighted_mean(series, s.ts, end, src.cursor);
            if (!v) continue;
            if (rec.counters.size() <= src.id) rec.counters.resize(src.id + 1);
            auto& acc = rec.counters[src.id];
            if (!acc) {
                if (auto st = charge_.take(sizeof(CounterAcc) + sizeof(void*),
                                           "counter statistics");
                    !st) {
                    fail_with(st.error());
                    return std::nullopt;
                }
                acc = std::make_unique<CounterAcc>(
                    CounterAcc{src.scope, series.kind, {}});
            }
            if (!charge_growth(acc->stats.add(src.scope == Scope::HOST &&
                                                      series.kind == Kind::DELTA
                                                  ? *v / ppn_
                                                  : *v),
                               "counter statistics"))
                return std::nullopt;
        }
        return id;
    }

    static constexpr std::uint64_t PATH_NODE_BYTES = 64;

    // Charges the bytes a sketch store grew by; false once over the share.
    bool charge_growth(std::uint64_t grew, std::string_view what) {
        if (grew == 0) return true;
        if (auto st = charge_.take(grew, what); !st) {
            fail_with(st.error());
            return false;
        }
        return true;
    }

    const GroupReader* r_;
    const RunGroup* g_;
    int set_;
    Charge charge_;
    const SetSpec* spec_;
    double ppn_;
    std::optional<GroupError> failed_;

    std::vector<std::unique_ptr<PathRec>> recs_;
    ankerl::unordered_dense::map<std::uint64_t, std::uint32_t> children_;
    std::vector<std::string> counter_keys_;
    StringViewMap<std::uint32_t> counter_ids_;

    bool lane_open_ = false;
    std::uint32_t lane_proc_ = 0;
    std::int64_t lane_id_ = 0;
    std::vector<std::pair<std::int64_t, std::uint32_t>> stack_;
    std::vector<Source> sources_;
};

struct FilePart {
    std::unique_ptr<GroupReader> reader;
    std::optional<GroupError> error;
};

// Holds one of a group's reader slots until it goes out of scope.
struct SlotGuard {
    coro::CoroSemaphore& slots;
    explicit SlotGuard(coro::CoroSemaphore& s) : slots(s) {}
    ~SlotGuard() { slots.release(1); }
    SlotGuard(const SlotGuard&) = delete;
    SlotGuard& operator=(const SlotGuard&) = delete;
};

coro::CoroTask<FilePart> read_file(std::string file, GroupMem* mem,
                                   coro::CoroSemaphore* slots) {
    co_await slots->acquire(1);
    SlotGuard guard(*slots);
    FilePart part;
    part.reader = std::make_unique<GroupReader>(*mem);
    auto st = co_await part.reader->read(std::move(file));
    if (!st) part.error = std::move(st.error());
    co_return part;
}

void derive_sets(const GroupReader& r, RunGroup& g) {
    const SetSpec tmpl = g.sets.at(0);
    std::set<std::vector<std::string>> lists;
    for (const auto& p : r.procs_)
        if (p.active) lists.insert(p.papi_names);
    g.sets.clear();
    for (const auto& names : lists) {
        SetSpec spec = tmpl;
        spec.papi_counters = names;
        for (const auto& n : names) {
            if (!spec.keys.papi_set.empty()) spec.keys.papi_set += '+';
            spec.keys.papi_set += n;
        }
        if (names.empty()) spec.keys.papi_set = "none";
        g.sets.push_back(std::move(spec));
    }
}

GResult<void> assign_sets(GroupReader& r, const RunGroup& g) {
    for (auto& p : r.procs_) {
        if (!p.active) continue;
        if (g.sets.size() == 1) {
            p.set = 0;
            continue;
        }
        for (std::size_t i = 0; i < g.sets.size(); ++i) {
            if (g.sets[i].papi_counters != p.papi_names) continue;
            if (p.set >= 0)
                return fail("", str_cat("process ", p.hhash, ":", p.pid,
                                        " matches several sets"));
            p.set = static_cast<int>(i);
        }
        if (p.set < 0)
            return fail("", str_cat("process ", p.hhash, ":", p.pid,
                                    " PAPI counters match no set"));
    }
    return {};
}

using CallSorter = ExternalSorter<SortRec, SortLess>;

// Replays every call in file order into the sorter, giving each its group-wide
// ids, its sequence number and its place in a lane. Calls of a set that already
// failed are dropped.
GResult<void> replay(std::vector<FilePart>& parts,
                     const std::vector<GroupReader::IdMap>& maps,
                     const GroupReader& r, const std::deque<SetAcc>& sets,
                     CallSorter& sorter, GroupMem& mem) {
    std::uint64_t seq = 0;
    std::vector<Call> scratch;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const GroupReader::IdMap& m = maps[i];
        for (auto& chunk : parts[i].reader->chunks_) {
            const Call* calls = chunk.mem.data();
            if (chunk.spilled) {
                auto file = mem.spill_file();
                if (!file) return unexpected(file.error());
                scratch.resize(chunk.count);
                auto got = (*file)->read(chunk.offset, scratch.data(),
                                         chunk.count * sizeof(Call));
                if (!got) return fail_from(got.error());
                calls = scratch.data();
            }
            for (std::uint64_t k = 0; k < chunk.count; ++k) {
                const Call& c = calls[k];
                const std::uint64_t sq = seq++;
                const std::uint32_t proc = m.pid[c.proc];
                const Proc& p = r.procs_[proc];
                if (p.set < 0 || !sets[static_cast<std::size_t>(p.set)].ok())
                    continue;
                SortRec s{0, c.ts, c.dur, sq, proc, m.sid[c.name], m.sid[c.cat],
                          0, 0};
                if (c.ts >= p.start && c.ts + c.dur <= p.end) {
                    const bool attached =
                        c.host_tid >= 0 ||
                        r.gpu_threads_.count({proc, c.tid}) != 0;
                    s.group = 1;
                    s.kind = attached ? 1 : 0;
                    s.lane = attached ? (c.host_tid >= 0 ? c.host_tid : p.pid)
                                      : c.tid;
                }
                auto added = sorter.add(s);
                if (!added) return fail_from(added.error());
            }
            chunk.mem = {};
            chunk.res.reset();
        }
    }
    return {};
}

}  // namespace

coro::CoroTask<GroupResult> process_group(CoroScope& ctx, RunGroup g,
                                          std::uint64_t memory_share) {
    GroupResult result;
    auto skip_all = [&](const std::string& file, const std::string& reason) {
        for (const auto& s : g.sets)
            result.skips.push_back(
                {g.dir, file, s.keys.papi_set + ": " + reason});
    };

    GroupMem mem(memory_share);
    coro::CoroSemaphore slots(
        std::max<std::uint64_t>(1, (memory_share / 4) / READER_BYTES));
    std::vector<coro::SpawnFuture<FilePart>> reads;
    reads.reserve(g.files.size());
    for (const auto& f : g.files)
        reads.push_back(
            ctx.spawn([f, mem_ptr = &mem, slots_ptr = &slots](CoroScope&) {
                return read_file(f, mem_ptr, slots_ptr);
            }));
    std::vector<FilePart> parts = co_await coro::when_all(std::move(reads));

    GroupReader r(mem);
    std::vector<GroupReader::IdMap> maps;
    maps.reserve(parts.size());
    for (auto& part : parts) {
        if (part.error) {
            skip_all(part.error->file, part.error->reason);
            co_return result;
        }
        maps.push_back(r.absorb(*part.reader));
    }

    auto prepared = [&]() -> GResult<void> {
        r.finish();
        for (const auto& s : g.sets) {
            auto csv = r.load_gpu_csv(s.gpu_csvs);
            if (!csv) return csv;
        }
        if (g.sets_from_counters) derive_sets(r, g);
        return assign_sets(r, g);
    }();
    if (!prepared) {
        skip_all(prepared.error().file, prepared.error().reason);
        co_return result;
    }

    std::deque<SetAcc> sets;
    for (std::size_t i = 0; i < g.sets.size(); ++i)
        sets.emplace_back(r, g, static_cast<int>(i), mem);

    CallSorter sorter(SortLess{}, mem.sort_budget);
    auto replayed = replay(parts, maps, r, sets, sorter, mem);
    if (!replayed) {
        skip_all(replayed.error().file, replayed.error().reason);
        co_return result;
    }
    auto walked = sorter.drain([&](const SortRec& s) {
        sets[static_cast<std::size_t>(r.procs_[s.proc].set)].feed(s);
    });
    if (!walked) {
        skip_all("", walked.error().message);
        co_return result;
    }

    result.spilled_bytes = mem.spilled_bytes();
    result.sort_runs = sorter.runs();
    for (auto& set : sets) {
        if (set.ok())
            result.runs.push_back(set.finish());
        else
            result.skips.push_back(
                {g.dir, set.error()->file, set.error()->reason});
    }
    co_return result;
}

}  // namespace dftracer::utils::trace::genesis
