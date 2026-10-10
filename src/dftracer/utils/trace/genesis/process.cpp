#include <dftracer/utils/core/common/base64.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/external_sort.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/memory_pool.h>
#include <dftracer/utils/core/common/scoped_fd.h>
#include <dftracer/utils/core/common/spill_file.h>
#include <dftracer/utils/core/common/str_format.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/core/coro/async_semaphore.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/coro/yield.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/dataframe/sketch.h>
#include <dftracer/utils/trace/genesis/genesis.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <fcntl.h>
#include <simdjson.h>
#include <sys/stat.h>

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

    // Adds `o`; the sketch merge is exact. The bytes the store grew by, for
    // the caller to charge.
    [[nodiscard]] std::uint64_t merge(const Stats& o) {
        min = std::min(min, o.min);
        max = std::max(max, o.max);
        sum += o.sum;
        n += o.n;
        const std::size_t before = sketch.store_bins();
        sketch.merge(o.sketch);
        return (sketch.store_bins() - before) * sizeof(std::uint32_t);
    }

    std::uint64_t store_bytes() const {
        return sketch.store_bins() * sizeof(std::uint32_t);
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

// Calls per chunk, and the bytes one open reader holds besides its chunk and
// its decoded gzip member (parser, input buffer, line buffers).
constexpr std::size_t CHUNK_CALLS = 16384;
constexpr std::uint64_t READER_BYTES = 8ULL * 1024 * 1024;
constexpr std::uint64_t CHARGE_STEP = 64ULL * 1024;
constexpr std::uint64_t BORROW_STEP = 256ULL * 1024 * 1024;
constexpr std::uint64_t SERIES_SAMPLE_BYTES =
    sizeof(std::pair<std::int64_t, double>);

struct GroupError {
    ErrorCode code = ErrorCode::PARSE;
    std::string file;
    std::string reason;
    bool over_budget = false;
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
    const BorrowFn* borrow = nullptr;

    // Raises the state pool by memory the caller lends, at least
    // BORROW_STEP at a time so the lender is asked rarely; false when it
    // lends nothing.
    bool borrow_state(std::uint64_t need) {
        if (!borrow || !*borrow) return false;
        const std::uint64_t ask = std::max(need, BORROW_STEP);
        std::uint64_t got = (*borrow)(ask);
        if (got == 0 && ask > need) got = (*borrow)(need);
        if (got == 0) return false;
        state.add_capacity(got);
        return true;
    }

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
                                  " bytes); raise --memory-budget"),
                          true};
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
        // Another task may take the borrowed bytes first; then ask again.
        for (int tries = 0; tries < 4 && mem_->borrow_state(need); ++tries)
            if (res_.try_grow(need)) return {};
        return mem_->over_budget(what);
    }

    // Takes `n` counted bytes over from `o`, for structures moved from `o`.
    void take_from(Charge& o, std::uint64_t n) noexcept {
        n = std::min(n, o.used_);
        o.used_ -= n;
        used_ += n;
        res_.take_from(o.res_, n);
    }

   private:
    GroupMem* mem_;
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

// The fields handle_line reads, each from the first key of its name.
struct Fields {
    std::optional<simdjson::dom::element> ph, pid, ts, tid, dur, arg_tid;
    std::string_view cat, name, hhash;
    simdjson::dom::object args;
    bool has_args = false;
    bool correlation_id = false;
    std::uint16_t seen = 0;

    // Whether `bit` is new, marking it seen.
    bool first(std::uint16_t bit) {
        if (seen & bit) return false;
        seen = static_cast<std::uint16_t>(seen | bit);
        return true;
    }
    static std::string_view str(simdjson::dom::element v) {
        std::string_view s;
        return v.get(s) == simdjson::SUCCESS ? s : std::string_view{};
    }

    void top(std::string_view k, simdjson::dom::element v) {
        if (k == "ph") {
            if (first(1)) ph = v;
        } else if (k == "ts") {
            if (first(2)) ts = v;
        } else if (k == "cat") {
            if (first(4)) cat = str(v);
        } else if (k == "pid") {
            if (first(8)) pid = v;
        } else if (k == "tid") {
            if (first(16)) tid = v;
        } else if (k == "dur") {
            if (first(32)) dur = v;
        } else if (k == "name") {
            if (first(64)) name = str(v);
        } else if (k == "args") {
            if (first(128)) has_args = v.get(args) == simdjson::SUCCESS;
        }
    }
    void arg(std::string_view k, simdjson::dom::element v) {
        if (k == "hhash") {
            if (first(256)) hhash = str(v);
        } else if (k == "tid") {
            if (first(512)) arg_tid = v;
        } else if (k == "correlation_id") {
            correlation_id = true;
        }
    }
};

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
using Sample = std::pair<std::int64_t, double>;

// A series' samples and bounds, copied next to the walk's other per-counter
// state when a lane opens: the walk checks every counter of every call, and
// reading the bounds through the Series costs a cache miss per counter.
struct SeriesView {
    const Sample* x = nullptr;
    std::size_t n = 0;
    std::int64_t first = 0;
    std::int64_t last = 0;
    Kind kind = Kind::DELTA;
};

SeriesView view_of(const Series& s) {
    SeriesView v;
    v.x = s.samples.data();
    v.n = s.samples.size();
    if (v.n) {
        v.first = s.samples.front().first;
        v.last = s.samples.back().first;
    }
    v.kind = s.kind;
    return v;
}

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
    std::size_t seek(const Sample* x, std::size_t n, std::int64_t a) {
        if (a < last) at = 0;
        last = a;
        const auto before = [a](const Sample& p) {
            return INCLUSIVE ? p.first < a : p.first <= a;
        };
        std::size_t step = 1;
        std::size_t hi = at;
        while (hi < n && before(x[hi])) {
            at = hi + 1;
            hi += step;
            step *= 2;
        }
        hi = std::min(hi, n);
        at = static_cast<std::size_t>(
            std::partition_point(x + at, x + hi, before) - x);
        return at;
    }
};

std::optional<double> prorate(const SeriesView& s, std::int64_t a,
                              std::int64_t b, Cursor& cur) {
    if (s.n < 2 || b < s.first || a > s.last) return std::nullopt;
    const Sample* x = s.x;
    std::size_t i = std::max<std::size_t>(1, cur.seek<false>(x, s.n, a));
    double sum = 0.0;
    for (; i < s.n && x[i - 1].first < b; ++i) {
        const std::int64_t len = x[i].first - x[i - 1].first;
        const std::int64_t lo = std::max(a, x[i - 1].first);
        const std::int64_t hi = std::min(b, x[i].first);
        if (len > 0 && hi > lo)
            sum += x[i].second * static_cast<double>(hi - lo) /
                   static_cast<double>(len);
    }
    return sum;
}

std::optional<double> weighted_mean(const SeriesView& s, std::int64_t a,
                                    std::int64_t b, Cursor& cur) {
    if (s.n < 2 || b <= s.first || a > s.last) return std::nullopt;
    const Sample* x = s.x;
    std::size_t i = std::max<std::size_t>(1, cur.seek<true>(x, s.n, a));
    if (b == a) return x[i].second;
    double num = 0.0;
    double den = 0.0;
    for (; i < s.n && x[i - 1].first < b; ++i) {
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
        // One pass over the keys of the record and one over its args, instead
        // of a lookup per key that scans the fields from the start each time.
        // The first of a repeated key wins, as with a lookup.
        Fields f;
        for (auto [k, v] : o) f.top(k, v);
        if (!f.ph) return {};
        const RecordPhase ph = read_phase(*f.ph);
        const std::string_view cat = f.cat;
        const std::string_view name = f.name;
        const bool has_args = f.has_args;
        simdjson::dom::object args = f.args;
        if (has_args)
            for (auto [k, v] : args) f.arg(k, v);
        const std::string_view hhash = f.hhash;
        const std::int64_t pid = f.pid ? get_i64(*f.pid) : 0;
        const std::int64_t ts = f.ts ? get_i64(*f.ts) : 0;

        switch (ph) {
            case RecordPhase::COMPLETE: {
                const std::uint32_t p = proc_index(hhash, pid);
                if (cat == "dftracer") {
                    if (name == "start") procs_[p].start = ts;
                    if (name == "end") procs_[p].end = ts;
                    return {};
                }
                const std::int64_t tid = f.tid ? get_i64(*f.tid) : 0;
                const std::int64_t host_tid =
                    f.arg_tid ? get_i64(*f.arg_tid) : -1;
                const bool gpu = host_tid >= 0 || f.correlation_id;
                const std::int64_t dur = f.dur ? get_i64(*f.dur) : 0;
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
        walk(s);
        if (failed_) fail_at_ = s;
    }

    // Adds a partial accumulator of the same set, built from other processes.
    // A failure of either is kept: the one at the earlier call in sort order,
    // which is the failure a single walk over all calls stops at.
    void merge(SetAcc& o) {
        if (o.failed_ && (!failed_ || (o.fail_at_ && fail_at_ &&
                                       SortLess{}(*o.fail_at_, *fail_at_)))) {
            failed_ = o.failed_;
            fail_at_ = o.fail_at_;
        }
        if (failed_) return;
        // Views into recs_, whose paths outlive the map.
        InternedStringViewMap<std::uint32_t> by_path;
        by_path.reserve(recs_.size());
        for (std::uint32_t i = 0; i < recs_.size(); ++i)
            by_path.emplace(recs_[i]->path, i);
        for (auto& orec : o.recs_) {
            std::vector<std::unique_ptr<CounterAcc>> theirs =
                std::move(orec->counters);
            orec->counters.clear();
            const auto found = by_path.find(orec->path);
            PathRec* rec = nullptr;
            if (found == by_path.end()) {
                charge_.take_from(
                    o.charge_, sizeof(PathRec) + orec->path.size() +
                                   PATH_NODE_BYTES + orec->dur.store_bytes());
                recs_.push_back(std::move(orec));
                rec = recs_.back().get();
                by_path.emplace(rec->path,
                                static_cast<std::uint32_t>(recs_.size() - 1));
            } else {
                rec = recs_[found->second].get();
                rec->first_ts = std::min(rec->first_ts, orec->first_ts);
                if (!charge_growth(rec->dur.merge(orec->dur),
                                   "duration statistics"))
                    return;
            }
            for (std::size_t cid = 0; cid < theirs.size(); ++cid) {
                if (!theirs[cid]) continue;
                const std::uint32_t id = counter_id(o.counter_keys_[cid]);
                if (rec->counters.size() <= id) rec->counters.resize(id + 1);
                auto& mine = rec->counters[id];
                if (!mine) {
                    charge_.take_from(o.charge_,
                                      sizeof(CounterAcc) + sizeof(void*) +
                                          theirs[cid]->stats.store_bytes());
                    mine = std::move(theirs[cid]);
                } else if (!charge_growth(mine->stats.merge(theirs[cid]->stats),
                                          "counter statistics")) {
                    return;
                }
            }
        }
    }

    // Fixes the order of the records and counters; once, before append().
    void prepare_output() {
        id_ = run_id(spec_->keys);
        counter_order_.resize(counter_keys_.size());
        for (std::uint32_t i = 0; i < counter_order_.size(); ++i)
            counter_order_[i] = i;
        std::sort(counter_order_.begin(), counter_order_.end(),
                  [&](std::uint32_t a, std::uint32_t b) {
                      return counter_keys_[a] < counter_keys_[b];
                  });
        std::sort(recs_.begin(), recs_.end(), [](const auto& a, const auto& b) {
            return a->path < b->path;
        });
    }

    // The run record, then one item per call path with its counters.
    std::size_t output_items() const { return recs_.size() + 1; }

    // The run and func records go to `sb`, the counter records to `cb`.
    void append_item(std::size_t item, string_builder& sb, string_builder& cb,
                     std::vector<std::uint8_t>& buf) const {
        if (item == 0) {
            append_run(sb);
            return;
        }
        const PathRec& rec = *recs_[item - 1];
        const std::string_view name = r_->strings_[rec.name];
        sb.append_raw(R"({"gtype":"func",)");
        sb.append_key_value("run", std::string_view(id_));
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
        for (const std::uint32_t cid : counter_order_) {
            if (cid >= rec.counters.size() || !rec.counters[cid]) continue;
            const CounterAcc& acc = *rec.counters[cid];
            cb.append_raw(R"({"gtype":"counter",)");
            cb.append_key_value("run", std::string_view(id_));
            cb.append_comma();
            cb.append_key_value("path", std::string_view(rec.path));
            cb.append_comma();
            cb.append_key_value("name", name);
            cb.append_comma();
            cb.append_key_value("metric", std::string_view(counter_keys_[cid]));
            cb.append_raw(acc.scope == Scope::PID ? R"(,"scope":"pid")"
                                                  : R"(,"scope":"host")");
            cb.append_raw(acc.kind == Kind::DELTA ? R"(,"kind":"delta","v":)"
                                                  : R"(,"kind":"gauge","v":)");
            append_stats(cb, acc.stats, true, acc.kind == Kind::DELTA, buf);
            cb.append_raw("}\n");
        }
    }

   private:
    void append_run(string_builder& sb) const {
        const RunKeys& keys = spec_->keys;
        sb.append_raw(R"({"gtype":"run","version":1,)");
        append_run_keys(sb, id_, keys);
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
    }

    struct Source {
        SeriesView series;
        Cursor cursor{};
        std::uint32_t id;
        Scope scope;
    };

    void walk(const SortRec& s) {
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
            sources_.push_back(
                {view_of(series), {}, counter_id(k), Scope::PID});
        if (const auto h = r_->hosts_.find(p.hhash); h != r_->hosts_.end())
            for (const auto& [k, series] : h->second)
                sources_.push_back(
                    {view_of(series), {}, counter_id(k), Scope::HOST});
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
            const SeriesView& series = src.series;
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
    std::optional<SortRec> fail_at_;

    std::vector<std::unique_ptr<PathRec>> recs_;
    ankerl::unordered_dense::map<std::uint64_t, std::uint32_t> children_;
    std::vector<std::string> counter_keys_;
    std::vector<std::uint32_t> counter_order_;
    std::string id_;
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
    std::uint64_t n;
    SlotGuard(coro::CoroSemaphore& s, std::uint64_t taken)
        : slots(s), n(taken) {}
    ~SlotGuard() { slots.release(n); }
    SlotGuard(const SlotGuard&) = delete;
    SlotGuard& operator=(const SlotGuard&) = delete;
};

// A size that could not be read: the reader takes all of its group's slots,
// so it is never counted as smaller than it is.
constexpr std::uint64_t UNKNOWN_SLOTS =
    std::numeric_limits<std::uint64_t>::max();

// Size probes open a file each, and every file of every group is probed at
// once, so the probes are bounded to stay under the open file limit.
constexpr std::uint64_t PROBE_LIMIT = 256;

coro::CoroSemaphore& probe_slots() {
    static coro::CoroSemaphore slots(PROBE_LIMIT);
    return slots;
}

struct ReaderSize {
    std::uint64_t slots = UNKNOWN_SLOTS;
    std::uint64_t file_bytes = 0;
};

// The READER_BYTES slots a reader of `file` takes: one for its buffers and as
// many as its decoded gzip member needs, the size the decoder takes from the
// file's last trailer. A file whose size cannot be read takes UNKNOWN_SLOTS.
coro::CoroTask<ReaderSize> reader_size(const std::string& file) {
    ReaderSize out;
    co_await probe_slots().acquire(1);
    SlotGuard probe(probe_slots(), 1);
    const ssize_t fd = co_await io::open(file.c_str(), O_RDONLY);
    if (fd < 0) co_return out;
    ScopedFd guard(static_cast<int>(fd));
    struct stat st{};
    if (::fstat(guard.get(), &st) != 0) co_return out;
    out.file_bytes = static_cast<std::uint64_t>(st.st_size);
    if (st.st_size < 18) {
        out.slots = 1;
        co_return out;
    }
    unsigned char trailer[4];
    if (co_await io::pread(guard.get(), trailer, sizeof trailer,
                           st.st_size - static_cast<off_t>(sizeof trailer)) !=
        static_cast<ssize_t>(sizeof trailer))
        co_return out;
    const std::uint64_t isize = static_cast<std::uint64_t>(trailer[0]) |
                                static_cast<std::uint64_t>(trailer[1]) << 8 |
                                static_cast<std::uint64_t>(trailer[2]) << 16 |
                                static_cast<std::uint64_t>(trailer[3]) << 24;
    out.slots = 1 + (isize + isize / 16 + READER_BYTES - 1) / READER_BYTES;
    co_return out;
}

coro::CoroTask<FilePart> read_file(std::string file, GroupMem* mem,
                                   coro::CoroSemaphore* slots,
                                   std::uint64_t total_slots,
                                   GroupProgress* progress) {
    const ReaderSize size = co_await reader_size(file);
    const std::uint64_t want = std::min(size.slots, total_slots);
    co_await slots->acquire(want);
    SlotGuard guard(*slots, want);
    FilePart part;
    part.reader = std::make_unique<GroupReader>(*mem);
    auto st = co_await part.reader->read(std::move(file));
    if (!st) part.error = std::move(st.error());
    if (progress) {
        progress->read_bytes.fetch_add(size.file_bytes,
                                       std::memory_order_relaxed);
        progress->updated();
    }
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

// What a group keeps while its runs are written: the memory share, the strings
// and processes the records name, and the sets' records.
struct GroupState {
    GroupState(RunGroup group, std::uint64_t share)
        : g(std::move(group)), mem(share), r(mem) {}

    RunGroup g;
    GroupMem mem;
    GroupReader r;
    std::deque<SetAcc> sets;
};

class SetSource final : public RunOutput::Source {
   public:
    SetSource(std::shared_ptr<GroupState> group, const SetAcc* set)
        : group_(std::move(group)), set_(set) {}

    std::size_t items() const override { return set_->output_items(); }

    void append(std::size_t item, std::string& head,
                std::string& body) const override {
        head_.clear();
        body_.clear();
        set_->append_item(item, head_, body_, buf_);
        head.append(std::string_view(head_));
        body.append(std::string_view(body_));
    }

   private:
    std::shared_ptr<GroupState> group_;
    const SetAcc* set_;
    mutable string_builder head_;
    mutable string_builder body_;
    mutable std::vector<std::uint8_t> buf_;
};

// One call chunk of one file, with the sequence number of its first call.
struct ChunkRef {
    std::size_t part;
    std::size_t chunk;
    std::uint64_t seq;
};

// The group's chunks in file order. Sequence numbers count every call, so they
// do not depend on how the chunks are split between tasks.
std::vector<ChunkRef> chunk_refs(const std::vector<FilePart>& parts,
                                 std::uint64_t* calls) {
    std::vector<ChunkRef> out;
    std::uint64_t seq = 0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto& chunks = parts[i].reader->chunks_;
        for (std::size_t k = 0; k < chunks.size(); ++k) {
            out.push_back({i, k, seq});
            seq += chunks[k].count;
        }
    }
    *calls = seq;
    return out;
}

// Gives each call of `refs` its group-wide ids, its sequence number and its
// place in a lane, and hands it to `emit`. Calls of a set that already failed
// are dropped. Each chunk's memory is freed once replayed.
template <class Emit>
GResult<void> replay_chunks(std::vector<FilePart>& parts,
                            const std::vector<GroupReader::IdMap>& maps,
                            const GroupReader& r,
                            const std::deque<SetAcc>& sets, GroupMem& mem,
                            const ChunkRef* refs, std::size_t n,
                            GroupProgress* progress, Emit&& emit) {
    std::vector<Call> scratch;
    for (std::size_t j = 0; j < n; ++j) {
        const ChunkRef& ref = refs[j];
        const GroupReader::IdMap& m = maps[ref.part];
        auto& chunk = parts[ref.part].reader->chunks_[ref.chunk];
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
            const std::uint64_t sq = ref.seq + k;
            const std::uint32_t proc = m.pid[c.proc];
            const Proc& p = r.procs_[proc];
            if (p.set < 0 || !sets[static_cast<std::size_t>(p.set)].ok())
                continue;
            SortRec s{0, c.ts, c.dur, sq, proc, m.sid[c.name], m.sid[c.cat],
                      0, 0};
            if (c.ts >= p.start && c.ts + c.dur <= p.end) {
                const bool attached =
                    c.host_tid >= 0 || r.gpu_threads_.count({proc, c.tid}) != 0;
                s.group = 1;
                s.kind = attached ? 1 : 0;
                s.lane =
                    attached ? (c.host_tid >= 0 ? c.host_tid : p.pid) : c.tid;
            }
            if (auto st = emit(s); !st) return st;
        }
        if (progress) {
            progress->sorted.fetch_add(chunk.count, std::memory_order_relaxed);
            progress->updated();
        }
        chunk.mem = {};
        chunk.res.reset();
    }
    return {};
}

// Tasks that replay a group too large for its sort buffer, each into sorted
// runs of its own; the sorter would write runs of that size anyway.
constexpr std::size_t REPLAY_TASKS = 16;

// Parts a group's processes are sorted and walked in, at most; see
// process_group.
constexpr std::size_t WALK_WAYS = 64;

// Replays every call in file order into the sorter of its process's part
// (process p into sorters[p % sorters.size()]). A group whose calls fit the
// sort buffer is replayed in one pass and sorted in memory. A larger one is
// split into REPLAY_TASKS ranges of chunks that are read, mapped and sorted at
// the same time, each with its part of the sort buffer, so the spilled chunks
// are read back in parallel. Each part's merge gives the same order either
// way, because the order is total.
coro::CoroTask<GResult<void>> replay(
    CoroScope& ctx, std::vector<FilePart>& parts,
    const std::vector<GroupReader::IdMap>& maps, const GroupReader& r,
    const std::deque<SetAcc>& sets, std::deque<CallSorter>& sorters,
    GroupMem& mem, GroupProgress* progress) {
    const std::size_t ways = sorters.size();
    std::uint64_t calls = 0;
    const std::vector<ChunkRef> refs = chunk_refs(parts, &calls);
    if (progress) progress->calls.store(calls, std::memory_order_relaxed);
    if (calls * sizeof(SortRec) <= mem.sort_budget || refs.size() < 2) {
        co_return replay_chunks(parts, maps, r, sets, mem, refs.data(),
                                refs.size(), progress,
                                [&](const SortRec& s) -> GResult<void> {
                                    auto added = sorters[s.proc % ways].add(s);
                                    if (!added) return fail_from(added.error());
                                    return {};
                                });
    }
    const std::size_t tasks = std::min(REPLAY_TASKS, refs.size());
    const std::size_t cap = std::max<std::size_t>(
        1024, mem.sort_budget / tasks / ways / sizeof(SortRec));
    // Contiguous ranges of about calls / tasks calls each.
    std::vector<std::size_t> starts{0};
    for (std::size_t j = 1; j < refs.size() && starts.size() < tasks; ++j)
        if (refs[j].seq >= calls / tasks * starts.size()) starts.push_back(j);
    starts.push_back(refs.size());
    std::vector<coro::SpawnFuture<GResult<void>>> runs;
    for (std::size_t t = 0; t + 1 < starts.size(); ++t) {
        const ChunkRef* first = refs.data() + starts[t];
        const std::size_t n = starts[t + 1] - starts[t];
        runs.push_back(ctx.spawn(
            [&parts, &maps, &r, &sets, &sorters, &mem, first, n, cap, ways,
             progress](CoroScope&) -> coro::CoroTask<GResult<void>> {
                std::vector<std::vector<SortRec>> bufs(ways);
                auto st =
                    replay_chunks(parts, maps, r, sets, mem, first, n, progress,
                                  [&](const SortRec& s) -> GResult<void> {
                                      const std::size_t w = s.proc % ways;
                                      auto& buf = bufs[w];
                                      if (buf.capacity() == 0) buf.reserve(cap);
                                      buf.push_back(s);
                                      if (buf.size() < cap) return {};
                                      auto added = sorters[w].add_run(buf);
                                      if (!added)
                                          return fail_from(added.error());
                                      return {};
                                  });
                for (std::size_t w = 0; st && w < ways; ++w) {
                    if (bufs[w].empty()) continue;
                    auto added = sorters[w].add_run(bufs[w]);
                    if (!added) st = fail_from(added.error());
                }
                co_return st;
            }));
    }
    auto done = co_await coro::when_all(std::move(runs));
    for (auto& st : done)
        if (!st) co_return st;
    co_return GResult<void>{};
}

}  // namespace

RunOutput::RunOutput(std::unique_ptr<Source> source)
    : source_(std::move(source)) {}
RunOutput::RunOutput(RunOutput&&) noexcept = default;
RunOutput& RunOutput::operator=(RunOutput&&) noexcept = default;
RunOutput::~RunOutput() = default;

bool RunOutput::next(std::string& head, std::string& body, std::size_t bytes) {
    const std::size_t start = head.size() + body.size();
    const std::size_t items = source_->items();
    while (next_item_ < items &&
           head.size() + body.size() - start < std::max<std::size_t>(bytes, 1))
        source_->append(next_item_++, head, body);
    return head.size() + body.size() > start;
}

std::string RunOutput::lines() const {
    std::string out;
    for (std::size_t i = 0; i < source_->items(); ++i)
        source_->append(i, out, out);
    return out;
}

coro::CoroTask<GroupResult> process_group(CoroScope& ctx, RunGroup group,
                                          std::uint64_t memory_share,
                                          GroupProgress* progress,
                                          const BorrowFn* borrow) {
    GroupResult result;
    if (progress) progress->enter(GroupProgress::Phase::READ);
    auto state = std::make_shared<GroupState>(std::move(group), memory_share);
    RunGroup& g = state->g;
    GroupMem& mem = state->mem;
    mem.borrow = borrow;
    GroupReader& r = state->r;
    auto skip_all = [&](const std::string& file, const std::string& reason) {
        for (const auto& s : g.sets)
            result.skips.push_back(
                {g.dir, file, s.keys.papi_set + ": " + reason});
    };

    const std::uint64_t total_slots =
        std::max<std::uint64_t>(1, (memory_share / 4) / READER_BYTES);
    coro::CoroSemaphore slots(total_slots);
    std::vector<coro::SpawnFuture<FilePart>> reads;
    reads.reserve(g.files.size());
    for (const auto& f : g.files)
        reads.push_back(ctx.spawn([f, mem_ptr = &mem, slots_ptr = &slots,
                                   total_slots, progress](CoroScope&) {
            return read_file(f, mem_ptr, slots_ptr, total_slots, progress);
        }));
    std::vector<FilePart> parts = co_await coro::when_all(std::move(reads));
    if (progress) progress->enter(GroupProgress::Phase::SORT);

    std::vector<GroupReader::IdMap> maps;
    maps.reserve(parts.size());
    for (auto& part : parts) {
        if (part.error) {
            result.over_budget = part.error->over_budget;
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
        result.over_budget = prepared.error().over_budget;
        skip_all(prepared.error().file, prepared.error().reason);
        co_return result;
    }

    // The processes go into `ways` parts, process p into part p % ways, so the
    // split and the merge order depend on the group alone and the output is
    // the same whatever the thread count. Each part is sorted and walked into
    // partial accumulators of its own, at the same time as the others, and
    // the partials are merged in part order.
    const std::size_t ways = std::min<std::size_t>(
        WALK_WAYS, std::max<std::size_t>(1, r.procs_.size()));
    std::vector<std::deque<SetAcc>> partials(ways);
    for (auto& part_sets : partials)
        for (std::size_t i = 0; i < g.sets.size(); ++i)
            part_sets.emplace_back(r, g, static_cast<int>(i), mem);
    std::deque<CallSorter> sorters;
    for (std::size_t w = 0; w < ways; ++w)
        sorters.emplace_back(
            SortLess{}, std::max<std::uint64_t>(1, mem.sort_budget / ways));
    auto replayed = co_await replay(ctx, parts, maps, r, partials[0], sorters,
                                    mem, progress);
    if (!replayed) {
        result.over_budget = replayed.error().over_budget;
        skip_all(replayed.error().file, replayed.error().reason);
        co_return result;
    }
    // The walk reads no files and keeps no calls of its own: the readers'
    // quarter and the calls pool, empty once replayed, go to its state.
    mem.state.add_capacity(memory_share / 4 +
                           (mem.calls.capacity() - mem.calls.used()));
    if (progress) progress->enter(GroupProgress::Phase::WALK);
    std::vector<coro::SpawnFuture<Result<void>>> walks;
    walks.reserve(ways);
    for (std::size_t w = 0; w < ways; ++w)
        walks.push_back(ctx.spawn([&sorters, &partials, &r, w,
                                   progress](CoroScope&)
                                      -> coro::CoroTask<Result<void>> {
            std::deque<SetAcc>& part_sets = partials[w];
            std::uint64_t fed = 0;
            auto walked = sorters[w].drain([&](const SortRec& s) {
                part_sets[static_cast<std::size_t>(r.procs_[s.proc].set)].feed(
                    s);
                if (progress && ++fed == CHUNK_CALLS) {
                    progress->walked.fetch_add(fed, std::memory_order_relaxed);
                    progress->updated();
                    fed = 0;
                }
            });
            if (progress)
                progress->walked.fetch_add(fed, std::memory_order_relaxed);
            co_return walked;
        }));
    auto walked = co_await coro::when_all(std::move(walks));
    for (auto& st : walked) {
        if (!st) {
            skip_all("", st.error().message);
            co_return result;
        }
    }
    std::deque<SetAcc>& sets = state->sets;
    sets = std::move(partials[0]);
    for (std::size_t w = 1; w < ways; ++w) {
        for (std::size_t i = 0; i < sets.size(); ++i)
            sets[i].merge(partials[w][i]);
        partials[w].clear();
    }

    result.spilled_bytes = mem.spilled_bytes();
    for (const auto& sorter : sorters) result.sort_runs += sorter.runs();
    parts.clear();
    for (auto& set : sets) {
        if (set.ok()) {
            set.prepare_output();
            result.runs.emplace_back(std::make_unique<SetSource>(state, &set));
        } else {
            result.over_budget = result.over_budget || set.error()->over_budget;
            result.skips.push_back(
                {g.dir, set.error()->file, set.error()->reason});
        }
    }
    co_return result;
}

}  // namespace dftracer::utils::trace::genesis
