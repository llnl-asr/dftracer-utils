#include <dftracer/utils/core/common/filesystem.h>
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
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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

struct Call {
    std::uint32_t proc;
    std::int64_t tid;
    std::int64_t host_tid;  // args.tid, or -1
    std::int64_t ts;
    std::int64_t dur;
    std::uint32_t name;
    std::uint32_t cat;
    std::uint32_t seq;
    bool gpu;  // carries args.tid or args.correlation_id
};

struct Stats {
    double min = std::numeric_limits<double>::infinity();
    double max = -std::numeric_limits<double>::infinity();
    double sum = 0.0;
    std::uint64_t n = 0;
    Sketch sketch{SKETCH_ACCURACY};

    void add(double v) {
        min = std::min(min, v);
        max = std::max(max, v);
        sum += v;
        ++n;
        sketch.add(v);
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

class GroupError : public std::runtime_error {
   public:
    GroupError(std::string file, const std::string& reason)
        : std::runtime_error(reason), file_(std::move(file)) {}
    const std::string& file() const { return file_; }

   private:
    std::string file_;
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
std::optional<double> prorate(const Series& s, std::int64_t a, std::int64_t b) {
    const auto& x = s.samples;
    if (x.size() < 2 || b < x.front().first || a > x.back().first)
        return std::nullopt;
    auto it = std::upper_bound(
        x.begin(), x.end(), a,
        [](std::int64_t t, const auto& p) { return t < p.first; });
    std::size_t i = std::max<std::size_t>(1, it - x.begin());
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
                                    std::int64_t b) {
    const auto& x = s.samples;
    if (x.size() < 2 || b <= x.front().first || a > x.back().first)
        return std::nullopt;
    auto it = std::lower_bound(
        x.begin(), x.end(), a,
        [](const auto& p, std::int64_t t) { return p.first < t; });
    std::size_t i = std::max<std::size_t>(1, it - x.begin());
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

void append_stats(string_builder& sb, const Stats& s, bool with_sum) {
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
}

class GroupReader {
   public:
    coro::CoroTask<void> read(std::string file) {
        try {
            // An unfinished run is skipped, not counted from a recovered tail.
            auto gen = async_streaming_gz_lines(file, 0, 0,
                                                /*recover_truncated=*/false);
            std::size_t n = 0;
            while (auto line = co_await gen.next()) {
                handle_line(file, line->content);
                if (++n % 4096 == 0) co_await coro::maybe_yield();
            }
        } catch (const GroupError&) {
            throw;
        } catch (const std::exception& e) {
            throw GroupError(file, e.what());
        }
    }

    // Append another file's parse; files are absorbed in file order so call
    // sequence numbers stay deterministic.
    void absorb(GroupReader&& o) {
        std::vector<std::uint32_t> sid(o.strings_.size());
        for (std::size_t i = 0; i < o.strings_.size(); ++i)
            sid[i] = intern(o.strings_[i]);
        std::vector<std::uint32_t> pid(o.procs_.size());
        for (std::size_t i = 0; i < o.procs_.size(); ++i) {
            Proc& src = o.procs_[i];
            pid[i] = proc_index(src.hhash, src.pid);
            Proc& dst = procs_[pid[i]];
            if (src.start >= 0) dst.start = src.start;
            if (src.end >= 0) dst.end = src.end;
            dst.active = dst.active || src.active;
            dst.papi_names.insert(dst.papi_names.end(), src.papi_names.begin(),
                                  src.papi_names.end());
            for (auto& [k, series] : src.papi) append(dst.papi[k], series);
        }
        for (Call c : o.calls_) {
            c.proc = pid[c.proc];
            c.name = sid[c.name];
            c.cat = sid[c.cat];
            c.seq = static_cast<std::uint32_t>(calls_.size());
            calls_.push_back(c);
        }
        for (auto& [hh, h] : o.hosts_)
            for (auto& [k, series] : h) append(hosts_[hh][k], series);
        host_hash_.merge(o.host_hash_);
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

    void load_gpu_csv(const std::vector<std::string>& files) {
        static constexpr const char* FIELDS[] = {
            "gpu.power.GPU_", "gpu.utilization.GPU_", "gpu.memory_used.GPU_"};
        for (const auto& f : files) {
            std::ifstream in(f);
            std::string line;
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
                    throw GroupError(f, "GPU CSV row without 6 columns");
                const auto hh = host_hash_.find(std::string(cols[1]));
                if (hh == host_hash_.end())
                    throw GroupError(
                        f, "no HH metadata for host " + std::string(cols[1]));
                const double seconds = std::stod(std::string(cols[0]));
                const auto ts =
                    static_cast<std::int64_t>(std::llround(seconds * 1e6));
                auto& h = hosts_[hh->second];
                for (std::size_t k = 0; k < 3; ++k) {
                    auto& s = h[FIELDS[k] + std::string(cols[2])];
                    s.kind = Kind::GAUGE;
                    s.samples.emplace_back(ts,
                                           std::stod(std::string(cols[3 + k])));
                }
            }
        }
        for (auto& [_, h] : hosts_)
            for (auto& [__, s] : h) sort_series(s);
    }

    std::vector<Proc> procs_;
    std::vector<Call> calls_;
    std::vector<std::string> strings_;
    std::map<std::string, std::map<std::string, Series>> hosts_;
    std::map<std::string, std::string> host_hash_;

   private:
    static void append(Series& dst, Series& src) {
        dst.kind = src.kind;
        dst.samples.insert(dst.samples.end(), src.samples.begin(),
                           src.samples.end());
    }

    static void sort_series(Series& s) {
        std::stable_sort(
            s.samples.begin(), s.samples.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
    }

    std::uint32_t intern(std::string_view s) {
        auto it = string_ids_.find(std::string(s));
        if (it != string_ids_.end()) return it->second;
        const auto id = static_cast<std::uint32_t>(strings_.size());
        strings_.emplace_back(s);
        string_ids_.emplace(strings_.back(), id);
        return id;
    }

    std::uint32_t proc_index(std::string_view hhash, std::int64_t pid) {
        std::string key(hhash);
        key += ':';
        key += std::to_string(pid);
        auto it = proc_ids_.find(key);
        if (it != proc_ids_.end()) return it->second;
        const auto id = static_cast<std::uint32_t>(procs_.size());
        Proc p;
        p.hhash = std::string(hhash);
        p.pid = pid;
        procs_.push_back(std::move(p));
        proc_ids_.emplace(std::move(key), id);
        return id;
    }

    void handle_line(const std::string& file, std::string_view line) {
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        while (!line.empty() && (line.back() == ',' || line.back() == ' ' ||
                                 line.back() == '\r'))
            line.remove_suffix(1);
        if (line.empty() || line.front() != '{') return;
        simdjson::dom::object o;
        if (parser_.parse(line.data(), line.size()).get(o) != simdjson::SUCCESS)
            throw GroupError(file, "invalid JSON line");
        simdjson::dom::element ph_el;
        if (o["ph"].get(ph_el) != simdjson::SUCCESS) return;
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
                    return;
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
                calls_.push_back(
                    {p, tid, host_tid, ts, dur, intern(name), intern(cat),
                     static_cast<std::uint32_t>(calls_.size()), gpu});
                return;
            }
            case RecordPhase::COUNTER:
                if (!has_args) return;
                if (pid == 0) {
                    handle_host(file, cat, name, hhash, ts, args);
                } else if (cat == "papi") {
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
                            s.samples.emplace_back(ts, *num);
                        } else if (k.substr(0, 5) == "PAPI_") {
                            p.papi_names.emplace_back(k);
                        }
                    }
                } else {
                    throw GroupError(file,
                                     "unknown per-process counter category " +
                                         std::string(cat));
                }
                return;
            case RecordPhase::METADATA:
                if (name == "HH" && has_args)
                    host_hash_[std::string(get_sv(args, "name"))] =
                        std::string(get_sv(args, "value"));
                return;
            default:
                return;
        }
    }

    void handle_host(const std::string& file, std::string_view cat,
                     std::string_view name, std::string_view hhash,
                     std::int64_t ts, simdjson::dom::object args) {
        const bool sys = cat == "sys";
        const bool net = cat == "net";
        const bool io = cat == "io";
        const bool gpu = cat == "gpu";
        if (!sys && !net && !io && !gpu)
            throw GroupError(
                file, "unknown host counter category " + std::string(cat));
        const std::string series_name = strip_spaces(name);
        if (sys && is_per_core_cpu(series_name)) return;
        auto& host = hosts_[std::string(hhash)];
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
            s.samples.emplace_back(ts, *num);
        }
    }

    simdjson::dom::parser parser_;
    std::unordered_map<std::string, std::uint32_t> string_ids_;
    std::unordered_map<std::string, std::uint32_t> proc_ids_;
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

// Builds one set's output or throws GroupError with the skip reason.
std::string build_set(const GroupReader& r, const RunGroup& g, int set) {
    const SetSpec& spec = g.sets[static_cast<std::size_t>(set)];
    std::vector<std::uint32_t> procs;
    for (std::uint32_t i = 0; i < r.procs_.size(); ++i)
        if (r.procs_[i].active && r.procs_[i].set == set) procs.push_back(i);
    const auto expected = spec.keys.nodes * spec.keys.ppn;
    if (static_cast<std::int64_t>(procs.size()) != expected)
        throw GroupError(spec.keys.papi_set, "expected " +
                                                 std::to_string(expected) +
                                                 " processes, found " +
                                                 std::to_string(procs.size()));
    for (const auto i : procs) {
        const Proc& p = r.procs_[i];
        if (p.start < 0 || p.end < 0)
            throw GroupError(spec.keys.papi_set,
                             "process " + p.hhash + ":" +
                                 std::to_string(p.pid) +
                                 " lacks dftracer start or end");
    }

    std::vector<PathRec> recs;
    std::unordered_map<std::uint64_t, std::uint32_t> children;
    // Parent index + 1 (0 at the root) and the name id identify a path.
    auto record = [&](std::uint32_t parent_plus_one, std::uint32_t name,
                      std::uint32_t cat) -> std::uint32_t {
        const std::uint64_t key =
            (static_cast<std::uint64_t>(parent_plus_one) << 32) | name;
        auto [it, fresh] =
            children.try_emplace(key, static_cast<std::uint32_t>(recs.size()));
        if (fresh) {
            PathRec rec;
            if (parent_plus_one) {
                rec.path = recs[parent_plus_one - 1].path + ";";
                rec.depth = recs[parent_plus_one - 1].depth + 1;
            } else {
                rec.depth = 0;
            }
            rec.path += r.strings_[name];
            rec.name = name;
            rec.cat = cat;
            recs.push_back(std::move(rec));
        }
        return it->second;
    };

    std::vector<std::string> counter_keys;
    std::unordered_map<std::string, std::uint32_t> counter_ids;
    auto counter_id = [&](const std::string& k) {
        auto [it, fresh] = counter_ids.try_emplace(
            k, static_cast<std::uint32_t>(counter_keys.size()));
        if (fresh) counter_keys.push_back(k);
        return it->second;
    };
    struct Source {
        std::uint32_t id;
        const Series* series;
        Scope scope;
    };

    struct Lane {
        std::vector<const Call*> host;
        std::vector<const Call*> attached;
    };
    std::map<std::pair<std::uint32_t, std::int64_t>, Lane> lanes;
    // A thread that records any GPU event is a GPU tracer thread; all of its
    // events are asynchronous and attach to a host lane.
    std::set<std::pair<std::uint32_t, std::int64_t>> gpu_threads;
    for (const Call& c : r.calls_)
        if (c.gpu) gpu_threads.emplace(c.proc, c.tid);
    for (const Call& c : r.calls_) {
        const Proc& p = r.procs_[c.proc];
        if (p.set != set || !p.active) continue;
        if (c.ts < p.start || c.ts + c.dur > p.end) {
            PathRec& rec = recs[record(0, c.name, c.cat)];
            rec.first_ts = std::min(rec.first_ts, c.ts);
            rec.dur.add(static_cast<double>(c.dur));
            continue;
        }
        if (c.host_tid >= 0 || gpu_threads.count({c.proc, c.tid})) {
            lanes[{c.proc, c.host_tid >= 0 ? c.host_tid : p.pid}]
                .attached.push_back(&c);
        } else {
            lanes[{c.proc, c.tid}].host.push_back(&c);
        }
    }

    const double ppn = static_cast<double>(spec.keys.ppn);
    auto by_start = [](const Call* a, const Call* b) {
        if (a->ts != b->ts) return a->ts < b->ts;
        if (a->dur != b->dur) return a->dur > b->dur;
        return a->seq < b->seq;
    };
    for (auto& [key, lane] : lanes) {
        std::sort(lane.host.begin(), lane.host.end(), by_start);
        std::sort(lane.attached.begin(), lane.attached.end(), by_start);
        const Proc& p = r.procs_[key.first];
        std::vector<Source> sources;
        for (const auto& [k, s] : p.papi)
            sources.push_back({counter_id(k), &s, Scope::PID});
        if (const auto h = r.hosts_.find(p.hhash); h != r.hosts_.end())
            for (const auto& [k, s] : h->second)
                sources.push_back({counter_id(k), &s, Scope::HOST});

        std::vector<std::pair<std::int64_t, std::uint32_t>> stack;
        auto add = [&](const Call* c) -> std::uint32_t {
            const std::int64_t end = c->ts + c->dur;
            while (!stack.empty() && stack.back().first <= c->ts)
                stack.pop_back();
            const auto rec_id = record(
                stack.empty() ? 0 : stack.back().second + 1, c->name, c->cat);
            PathRec& rec = recs[rec_id];
            rec.first_ts = std::min(rec.first_ts, c->ts);
            rec.dur.add(static_cast<double>(c->dur));
            for (const Source& src : sources) {
                const Series& s = *src.series;
                const auto v = s.kind == Kind::DELTA
                                   ? prorate(s, c->ts, end)
                                   : weighted_mean(s, c->ts, end);
                if (!v) continue;
                if (rec.counters.size() <= src.id)
                    rec.counters.resize(src.id + 1);
                auto& acc = rec.counters[src.id];
                if (!acc)
                    acc = std::make_unique<CounterAcc>(
                        CounterAcc{src.scope, s.kind, {}});
                acc->stats.add(src.scope == Scope::HOST && s.kind == Kind::DELTA
                                   ? *v / ppn
                                   : *v);
            }
            return rec_id;
        };

        std::size_t a = 0;
        for (const Call* c : lane.host) {
            for (; a < lane.attached.size() && lane.attached[a]->ts < c->ts;
                 ++a)
                add(lane.attached[a]);
            const std::int64_t end = c->ts + c->dur;
            while (!stack.empty() && stack.back().first <= c->ts)
                stack.pop_back();
            if (!stack.empty() && end > stack.back().first)
                throw GroupError(spec.keys.papi_set,
                                 "call " + r.strings_[c->name] + " at ts " +
                                     std::to_string(c->ts) +
                                     " ends after its parent " +
                                     recs[stack.back().second].path);
            stack.emplace_back(end, add(c));
        }
        for (; a < lane.attached.size(); ++a) add(lane.attached[a]);
    }

    const std::string id = run_id(spec.keys);
    string_builder sb;
    sb.append_raw(
        R"({"name":"RUN","cat":"dftracer","pid":0,"tid":0,"ts":0,"ph":4,"args":{)");
    append_run_keys(sb, id, spec.keys);
    sb.append_comma();
    sb.append_key_value("method", std::string_view("prorate"));
    sb.append_comma();
    sb.append_key_value("sketch_accuracy", SKETCH_ACCURACY);
    sb.append_comma();
    sb.append_key_value("leaf", std::string_view(g.rel_dir));
    if (!g.summary_json.empty()) {
        sb.append_comma();
        sb.escape_and_append_with_quotes("summary");
        sb.append_colon();
        sb.append_raw(g.summary_json);
    }
    sb.append_raw("}}\n");

    std::vector<std::uint32_t> counter_order(counter_keys.size());
    for (std::uint32_t i = 0; i < counter_order.size(); ++i)
        counter_order[i] = i;
    std::sort(counter_order.begin(), counter_order.end(),
              [&](std::uint32_t a, std::uint32_t b) {
                  return counter_keys[a] < counter_keys[b];
              });
    std::sort(recs.begin(), recs.end(), [](const PathRec& a, const PathRec& b) {
        return a.path < b.path;
    });
    for (const PathRec& rec : recs) {
        sb.start_object();
        sb.append_key_value("name", std::string_view(r.strings_[rec.name]));
        sb.append_comma();
        sb.append_key_value("cat", std::string_view(r.strings_[rec.cat]));
        sb.append_raw(R"(,"pid":0,"tid":0,)");
        sb.append_key_value("ts", rec.first_ts);
        sb.append_raw(R"(,"ph":3,"args":{)");
        // The run's keys live on its RUN line (the run dictionary).
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
        sb.append_key_value("depth", rec.depth);
        sb.append_comma();
        sb.append_key_value("count", rec.dur.n);
        sb.append_comma();
        sb.escape_and_append_with_quotes("dur");
        sb.append_colon();
        sb.start_object();
        append_stats(sb, rec.dur, true);
        sb.end_object();
        sb.append_comma();
        sb.escape_and_append_with_quotes("counters");
        sb.append_colon();
        sb.start_object();
        bool first = true;
        for (const std::uint32_t cid : counter_order) {
            if (cid >= rec.counters.size() || !rec.counters[cid]) continue;
            const CounterAcc& acc = *rec.counters[cid];
            if (!first) sb.append_comma();
            first = false;
            sb.escape_and_append_with_quotes(counter_keys[cid]);
            sb.append_colon();
            sb.append_raw(acc.scope == Scope::PID ? R"({"scope":"pid",)"
                                                  : R"({"scope":"host",)");
            sb.append_raw(acc.kind == Kind::DELTA ? R"("kind":"delta",)"
                                                  : R"("kind":"gauge",)");
            append_stats(sb, acc.stats, acc.kind == Kind::DELTA);
            sb.end_object();
        }
        sb.end_object();
        sb.append_raw("}}\n");
    }
    return std::string(sb);
}

}  // namespace

namespace {

struct FilePart {
    std::unique_ptr<GroupReader> reader;
    std::string error_file;
    std::string error;
};

coro::CoroTask<FilePart> read_file(std::string file) {
    FilePart part;
    part.reader = std::make_unique<GroupReader>();
    try {
        co_await part.reader->read(file);
    } catch (const GroupError& e) {
        part.error_file = e.file();
        part.error = e.what();
    }
    co_return part;
}

struct SetOut {
    RunOutput run;
    std::optional<Skip> skip;
};

coro::CoroTask<SetOut> build_set_task(const GroupReader* r, const RunGroup* g,
                                      int set) {
    SetOut out;
    try {
        out.run = {g->dir + '\0' +
                       g->sets[static_cast<std::size_t>(set)].keys.papi_set,
                   build_set(*r, *g, set)};
    } catch (const GroupError& e) {
        out.skip = Skip{g->dir, e.file(), e.what()};
    }
    co_return out;
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

void assign_sets(GroupReader& r, const RunGroup& g) {
    for (auto& p : r.procs_) {
        if (!p.active) continue;
        if (g.sets.size() == 1) {
            p.set = 0;
            continue;
        }
        for (std::size_t i = 0; i < g.sets.size(); ++i) {
            if (g.sets[i].papi_counters != p.papi_names) continue;
            if (p.set >= 0)
                throw GroupError("", "process " + p.hhash + ":" +
                                         std::to_string(p.pid) +
                                         " matches several sets");
            p.set = static_cast<int>(i);
        }
        if (p.set < 0)
            throw GroupError("", "process " + p.hhash + ":" +
                                     std::to_string(p.pid) +
                                     " PAPI counters match no set");
    }
}

}  // namespace

coro::CoroTask<GroupResult> process_group(CoroScope& ctx, RunGroup g) {
    GroupResult result;
    auto skip_all = [&](const std::string& file, const std::string& reason) {
        for (const auto& s : g.sets)
            result.skips.push_back(
                {g.dir, file, s.keys.papi_set + ": " + reason});
    };

    std::vector<coro::SpawnFuture<FilePart>> reads;
    reads.reserve(g.files.size());
    for (const auto& f : g.files)
        reads.push_back(ctx.spawn([f](CoroScope&) { return read_file(f); }));
    std::vector<FilePart> parts = co_await coro::when_all(std::move(reads));

    GroupReader r;
    for (auto& part : parts) {
        if (!part.error.empty()) {
            skip_all(part.error_file, part.error);
            co_return result;
        }
        r.absorb(std::move(*part.reader));
    }
    parts.clear();
    try {
        r.finish();
        for (const auto& s : g.sets) r.load_gpu_csv(s.gpu_csvs);
        if (g.sets_from_counters) derive_sets(r, g);
        assign_sets(r, g);
    } catch (const GroupError& e) {
        skip_all(e.file(), e.what());
        co_return result;
    }

    std::vector<coro::SpawnFuture<SetOut>> builds;
    builds.reserve(g.sets.size());
    for (std::size_t i = 0; i < g.sets.size(); ++i) {
        const GroupReader* rp = &r;
        const RunGroup* gp = &g;
        const int set = static_cast<int>(i);
        builds.push_back(ctx.spawn(
            [rp, gp, set](CoroScope&) { return build_set_task(rp, gp, set); }));
    }
    for (auto& out : co_await coro::when_all(std::move(builds))) {
        if (out.skip)
            result.skips.push_back(std::move(*out.skip));
        else
            result.runs.push_back(std::move(out.run));
    }
    co_return result;
}

}  // namespace dftracer::utils::trace::genesis
