// SPDX-License-Identifier: MIT
// Benchmark: reading one event line into a FoldEvent, the DOM reader against
// the on-demand reader, parse included, on a file of JSON lines. Rows:
//
//   dom/args         DOM parse of each line + extract_fold_event_into, args
//   ondemand/args    json::Document::parse + the on-demand reader, args
//   batch/args       DOM parse_many through trace::parse_lines + the DOM reader
//   dom/noargs       as dom/args, no args
//   ondemand/noargs  as ondemand/args, no args
//   cached/args      as ondemand/args, interning through dstd::InternCache
//   cached/noargs    as ondemand/noargs, interning through dstd::InternCache
// clang-format off
//   dom/path         DOM parse of each line + decode_record_into, every leaf by path
//   ondemand/path    json::Document::parse + the on-demand decode_record_into
//   dom/capture      DOM parse of each line + extract_fold_event_into, no args and
//                    two captures (the nested "o.x" and the bare "size")
//   ondemand/capture json::Document::parse + the on-demand reader, same captures
//   cached/path      as ondemand/path, interning through dstd::InternCache
//   cached/capture   as ondemand/capture, interning through dstd::InternCache
//   old/path         what the consumers did before: json::Document::parse, then
//                    ValueAccess::dom (a second parse), then the DOM path reader
//   old/capture      the same with the DOM reader and the two captures
// clang-format on
//
// Each row reuses one parser and one intern table. The rows alternate inside
// every round, and the order flips on odd rounds. The table shows the median
// ns per line over the rounds and the ratio to the DOM row of its kind.
// The checksum is taken once per row outside the timed loop, over the events
// with every intern id resolved to text; rows of one kind must match.
// A line a reader rejects adds no event to the checksum. row_mask, a bit per
// row in the order above, limits the run to some rows, for profiling.
//
// Built only with DFTRACER_UTILS_BUILD_BENCHMARKS=ON and run manually.
//
//   fold_read_bench FILE [rounds] [max_lines] [row_mask]
//   fold_read_bench --smoke

#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/intern_cache.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/value_access.h>
#include <dftracer/utils/trace/parse_inflated.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <simdjson.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace dftracer::utils;
using trace::views::detail::FoldEvent;

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* SMOKE_LINES =
    R"({"id":1,"name":"read","cat":"POSIX","pid":1,"tid":2,"ts":10,"dur":5,"ph":1,"args":{"hhash":"a","fhash":"b","size":4096,"o":{"x":1.5}}}
{"name":"HH","ph":4,"type":1,"args":{"hhash":"e696ebc2","name":"node-0"}}
{"name":"write","cat":"POSIX","pid":1,"tid":2,"ts":20,"ph":"X","args":{"fname":"/a/b","l":[1,2,null,true]}}
)";

struct Hash {
    std::uint64_t value = 1469598103934665603ULL;
    void bytes(const void* data, std::size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) {
            value ^= p[i];
            value *= 1099511628211ULL;
        }
    }
    template <class T>
    void add(const T& v) {
        bytes(&v, sizeof v);
    }
    void text(dstd::StringView s) {
        add(s.size());
        bytes(s.data(), s.size());
    }
};

void hash_event(Hash& h, const FoldEvent& ev,
                const dstd::StringIntern& intern) {
    constexpr std::uint32_t NO_ID = dstd::StringIntern::NO_ID;
    const auto id = [&](std::uint32_t i) {
        h.text(i == NO_ID ? dstd::StringView("-") : intern.resolve(i));
    };
    h.add(ev.phase);
    h.add(ev.pid);
    h.add(ev.tid);
    h.add(ev.ts);
    h.add(ev.dur);
    h.add(ev.has_dur);
    h.add(ev.by_path);
    id(ev.cat_id);
    id(ev.name_id);
    id(ev.fhash_id);
    id(ev.hhash_id);
    for (const auto* list : {&ev.args, &ev.top_fields}) {
        h.add(list->size());
        for (const auto& [key, value] : *list) {
            id(key);
            h.add(value.index());
            switch (value.index()) {
                case 0:
                    h.add(std::get<0>(value));
                    break;
                case 1:
                    h.add(std::get<1>(value));
                    break;
                case 2:
                    id(std::get<2>(value));
                    break;
                default:
                    h.add(std::get<3>(value));
                    break;
            }
        }
    }
    for (const auto& s : ev.specials) {
        id(s.key);
        h.add(s.kind);
        h.add(s.top);
    }
}

struct Acc {
    std::uint64_t events = 0;
    std::uint64_t sink = 0;
    Hash hash;
};

// A timed pass touches the event cheaply; a checksum pass hashes it fully.
template <bool Check>
void account(Acc& acc, const FoldEvent& ev, const dstd::StringIntern& intern) {
    ++acc.events;
    if constexpr (Check)
        hash_event(acc.hash, ev, intern);
    else
        acc.sink += ev.args.size() + ev.specials.size() + ev.ts + ev.name_id;
}

struct Input {
    // The whole file with the "[" and "]" lines blanked and simdjson padding
    // after it.
    dstd::String text;
    // The bytes up to the end of the last line used, all of them without a
    // line limit.
    std::size_t length = 0;
    dstd::Vector<dstd::StringView> lines;
};

void load(Input& in, dstd::String raw, std::size_t max_lines) {
    in.text = std::move(raw);
    std::size_t size = in.text.size();
    if (size == 0 || in.text[size - 1] != '\n') {
        in.text.push_back('\n');
        ++size;
    }
    trace::strip_array_delimiters(in.text.data(), size);
    in.text.resize(size + simdjson::SIMDJSON_PADDING);
    std::size_t at = 0;
    for (; at < size && in.lines.size() < max_lines;) {
        const char* base = in.text.data();
        std::size_t end = at;
        while (base[end] != '\n') ++end;
        std::size_t first = at;
        while (first < end && (base[first] == ' ' || base[first] == '\r'))
            ++first;
        if (first < end) in.lines.emplace_back(base + first, end - first);
        at = end + 1;
    }
    in.length = at;
}

class DomRow {
   public:
    DomRow(const Input& in, bool needs_args)
        : in_(in), needs_args_(needs_args) {
        buf_.resize(simdjson::SIMDJSON_PADDING);
    }
    template <bool Check>
    void run(Acc& acc) {
        for (dstd::StringView line : in_.lines) {
            buf_.assign(line.data(), line.size());
            buf_.resize(line.size() + simdjson::SIMDJSON_PADDING);
            simdjson::dom::element root;
            if (parser_.parse(buf_.data(), line.size(), false).get(root))
                continue;
            trace::views::detail::extract_fold_event_into(ev_, root, intern_,
                                                          needs_args_);
            account<Check>(acc, ev_, intern_);
        }
    }

   private:
    const Input& in_;
    bool needs_args_;
    json::RecordParser parser_;
    dstd::String buf_;
    dstd::StringIntern intern_;
    FoldEvent ev_;
};

class OnDemandRow {
   public:
    OnDemandRow(const Input& in, bool needs_args, bool use_cache = false)
        : in_(in), needs_args_(needs_args), use_cache_(use_cache) {}
    template <bool Check>
    void run(Acc& acc) {
        for (dstd::StringView line : in_.lines) {
            auto value = doc_.parse(line);
            if (!value) continue;
            trace::views::detail::extract_fold_event_into(
                ev_, *value, intern_, needs_args_,
                use_cache_ ? &cache_ : nullptr);
            if (json::detail::ValueAccess::broken(*value)) continue;
            account<Check>(acc, ev_, intern_);
        }
    }

   private:
    const Input& in_;
    bool needs_args_;
    bool use_cache_;
    json::Document doc_;
    dstd::StringIntern intern_;
    dstd::InternCache cache_{intern_};
    FoldEvent ev_;
};

// The names the capture rows capture: one nested and one bare.
const dstd::Vector<dstd::String>& capture_names() {
    static const dstd::Vector<dstd::String> names{"o.x", "size"};
    return names;
}

class DomPathRow {
   public:
    explicit DomPathRow(const Input& in) : in_(in) {
        buf_.resize(simdjson::SIMDJSON_PADDING);
    }
    template <bool Check>
    void run(Acc& acc) {
        for (dstd::StringView line : in_.lines) {
            buf_.assign(line.data(), line.size());
            buf_.resize(line.size() + simdjson::SIMDJSON_PADDING);
            simdjson::dom::element root;
            if (parser_.parse(buf_.data(), line.size(), false).get(root))
                continue;
            trace::views::detail::decode_record_into(ev_, root, intern_,
                                                     /*capture_schema=*/false);
            account<Check>(acc, ev_, intern_);
        }
    }

   private:
    const Input& in_;
    json::RecordParser parser_;
    dstd::String buf_;
    dstd::StringIntern intern_;
    FoldEvent ev_;
};

class OnDemandPathRow {
   public:
    explicit OnDemandPathRow(const Input& in, bool use_cache = false)
        : in_(in), use_cache_(use_cache) {}
    template <bool Check>
    void run(Acc& acc) {
        for (dstd::StringView line : in_.lines) {
            auto value = doc_.parse(line);
            if (!value) continue;
            trace::views::detail::decode_record_into(
                ev_, *value, intern_, /*capture_schema=*/false, nullptr,
                nullptr, nullptr, 0, nullptr, false,
                use_cache_ ? &cache_ : nullptr);
            if (json::detail::ValueAccess::broken(*value)) continue;
            account<Check>(acc, ev_, intern_);
        }
    }

   private:
    const Input& in_;
    bool use_cache_;
    json::Document doc_;
    dstd::StringIntern intern_;
    dstd::InternCache cache_{intern_};
    FoldEvent ev_;
};

class OldPathRow {
   public:
    explicit OldPathRow(const Input& in) : in_(in) {}
    template <bool Check>
    void run(Acc& acc) {
        for (dstd::StringView line : in_.lines) {
            auto value = doc_.parse(line);
            if (!value) continue;
            const auto root = json::detail::ValueAccess::dom(*value);
            if (!root) continue;
            trace::views::detail::decode_record_into(ev_, *root, intern_,
                                                     /*capture_schema=*/false);
            account<Check>(acc, ev_, intern_);
        }
    }

   private:
    const Input& in_;
    json::Document doc_;
    dstd::StringIntern intern_;
    FoldEvent ev_;
};

class DomCaptureRow {
   public:
    explicit DomCaptureRow(const Input& in) : in_(in) {
        buf_.resize(simdjson::SIMDJSON_PADDING);
    }
    template <bool Check>
    void run(Acc& acc) {
        for (dstd::StringView line : in_.lines) {
            buf_.assign(line.data(), line.size());
            buf_.resize(line.size() + simdjson::SIMDJSON_PADDING);
            simdjson::dom::element root;
            if (parser_.parse(buf_.data(), line.size(), false).get(root))
                continue;
            trace::views::detail::extract_fold_event_into(
                ev_, root, intern_, false, &capture_names());
            account<Check>(acc, ev_, intern_);
        }
    }

   private:
    const Input& in_;
    json::RecordParser parser_;
    dstd::String buf_;
    dstd::StringIntern intern_;
    FoldEvent ev_;
};

class OnDemandCaptureRow {
   public:
    explicit OnDemandCaptureRow(const Input& in, bool use_cache = false)
        : in_(in), use_cache_(use_cache) {}
    template <bool Check>
    void run(Acc& acc) {
        for (dstd::StringView line : in_.lines) {
            auto value = doc_.parse(line);
            if (!value) continue;
            trace::views::detail::extract_fold_event_into(
                ev_, *value, intern_, false, use_cache_ ? &cache_ : nullptr,
                &capture_names());
            if (json::detail::ValueAccess::broken(*value)) continue;
            account<Check>(acc, ev_, intern_);
        }
    }

   private:
    const Input& in_;
    bool use_cache_;
    json::Document doc_;
    dstd::StringIntern intern_;
    dstd::InternCache cache_{intern_};
    FoldEvent ev_;
};

class OldCaptureRow {
   public:
    explicit OldCaptureRow(const Input& in) : in_(in) {}
    template <bool Check>
    void run(Acc& acc) {
        for (dstd::StringView line : in_.lines) {
            auto value = doc_.parse(line);
            if (!value) continue;
            const auto root = json::detail::ValueAccess::dom(*value);
            if (!root) continue;
            trace::views::detail::extract_fold_event_into(
                ev_, *root, intern_, false, &capture_names());
            account<Check>(acc, ev_, intern_);
        }
    }

   private:
    const Input& in_;
    json::Document doc_;
    dstd::StringIntern intern_;
    FoldEvent ev_;
};

class BatchRow {
   public:
    explicit BatchRow(const Input& in) : in_(in) {}
    template <bool Check>
    void run(Acc& acc) {
        trace::parse_lines(parser_, in_.text.data(), in_.length,
                           [&](simdjson::dom::element root, dstd::StringView) {
                               trace::views::detail::extract_fold_event_into(
                                   ev_, root, intern_, true);
                               account<Check>(acc, ev_, intern_);
                           });
    }

   private:
    const Input& in_;
    json::RecordParser parser_;
    dstd::StringIntern intern_;
    FoldEvent ev_;
};

struct Result {
    const char* name;
    int reference;  // index of the row this one is compared with, or -1
    double median_ns = 0;
    std::uint64_t events = 0;
    std::uint64_t checksum = 0;
};

double median(dstd::Vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

template <class Row>
double timed(Row& row, Acc& acc, std::size_t lines) {
    const auto start = Clock::now();
    row.template run<false>(acc);
    const auto stop = Clock::now();
    return std::chrono::duration<double, std::nano>(stop - start).count() /
           static_cast<double>(lines);
}

}  // namespace

int main(int argc, char** argv) {
    const bool smoke = argc == 2 && dstd::StringView(argv[1]) == "--smoke";
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: fold_read_bench FILE [rounds] [max_lines]\n"
                     "       fold_read_bench --smoke\n");
        return 2;
    }
    dstd::String raw;
    if (smoke) {
        raw = SMOKE_LINES;
    } else {
        auto file = dstd::fs::read_file(argv[1]);
        if (!file) {
            std::fprintf(stderr, "cannot read %s\n", argv[1]);
            return 2;
        }
        raw = std::move(*file);
    }
    const int rounds = smoke ? 1 : (argc > 2 ? std::atoi(argv[2]) : 7);
    const std::size_t max_lines =
        argc > 3 ? static_cast<std::size_t>(std::atoll(argv[3]))
                 : static_cast<std::size_t>(-1);
    const unsigned row_mask =
        argc > 4 ? static_cast<unsigned>(std::strtoul(argv[4], nullptr, 0))
                 : 0x7FFFu;
    Input in;
    load(in, std::move(raw), max_lines);
    if (in.lines.empty()) {
        std::fprintf(stderr, "no lines\n");
        return 2;
    }

    DomRow dom_args(in, true);
    OnDemandRow od_args(in, true);
    BatchRow batch_args(in);
    DomRow dom_noargs(in, false);
    OnDemandRow od_noargs(in, false);
    OnDemandRow cached_args(in, true, true);
    OnDemandRow cached_noargs(in, false, true);
    DomPathRow dom_path(in);
    OnDemandPathRow od_path(in);
    DomCaptureRow dom_capture(in);
    OnDemandCaptureRow od_capture(in);
    OnDemandPathRow cached_path(in, true);
    OnDemandCaptureRow cached_capture(in, true);
    OldPathRow old_path(in);
    OldCaptureRow old_capture(in);

    Result results[] = {
        {"dom/args", -1},      {"ondemand/args", 0},    {"batch/args", 0},
        {"dom/noargs", -1},    {"ondemand/noargs", 3},  {"cached/args", 0},
        {"cached/noargs", 3},  {"dom/path", -1},        {"ondemand/path", 7},
        {"dom/capture", -1},   {"ondemand/capture", 9}, {"cached/path", 7},
        {"cached/capture", 9}, {"old/path", 7},         {"old/capture", 9}};
    constexpr int ROWS = sizeof(results) / sizeof(results[0]);
    const auto check = [&](int row, auto& r) {
        Acc acc;
        r.template run<true>(acc);
        results[row].events = acc.events;
        results[row].checksum = acc.hash.value;
    };
    const auto wanted = [&](int row) { return (row_mask >> row & 1u) != 0; };
    if (wanted(0)) check(0, dom_args);
    if (wanted(1)) check(1, od_args);
    if (wanted(2)) check(2, batch_args);
    if (wanted(3)) check(3, dom_noargs);
    if (wanted(4)) check(4, od_noargs);
    if (wanted(5)) check(5, cached_args);
    if (wanted(6)) check(6, cached_noargs);
    if (wanted(7)) check(7, dom_path);
    if (wanted(8)) check(8, od_path);
    if (wanted(9)) check(9, dom_capture);
    if (wanted(10)) check(10, od_capture);
    if (wanted(11)) check(11, cached_path);
    if (wanted(12)) check(12, cached_capture);
    if (wanted(13)) check(13, old_path);
    if (wanted(14)) check(14, old_capture);

    dstd::Vector<double> samples[ROWS];
    Acc acc;
    for (int round = 0; round < rounds; ++round) {
        for (int k = 0; k < ROWS; ++k) {
            const int row = round % 2 == 0 ? k : ROWS - 1 - k;
            if (!wanted(row)) continue;
            double ns = 0;
            switch (row) {
                case 0:
                    ns = timed(dom_args, acc, in.lines.size());
                    break;
                case 1:
                    ns = timed(od_args, acc, in.lines.size());
                    break;
                case 2:
                    ns = timed(batch_args, acc, in.lines.size());
                    break;
                case 3:
                    ns = timed(dom_noargs, acc, in.lines.size());
                    break;
                case 4:
                    ns = timed(od_noargs, acc, in.lines.size());
                    break;
                case 5:
                    ns = timed(cached_args, acc, in.lines.size());
                    break;
                case 6:
                    ns = timed(cached_noargs, acc, in.lines.size());
                    break;
                case 7:
                    ns = timed(dom_path, acc, in.lines.size());
                    break;
                case 8:
                    ns = timed(od_path, acc, in.lines.size());
                    break;
                case 9:
                    ns = timed(dom_capture, acc, in.lines.size());
                    break;
                case 10:
                    ns = timed(od_capture, acc, in.lines.size());
                    break;
                case 11:
                    ns = timed(cached_path, acc, in.lines.size());
                    break;
                case 12:
                    ns = timed(cached_capture, acc, in.lines.size());
                    break;
                case 13:
                    ns = timed(old_path, acc, in.lines.size());
                    break;
                default:
                    ns = timed(old_capture, acc, in.lines.size());
                    break;
            }
            samples[row].push_back(ns);
        }
    }

    std::printf("lines %zu, bytes %zu, rounds %d\n", in.lines.size(), in.length,
                rounds);
    std::printf("%-17s %10s %8s %9s %18s\n", "row", "ns/line", "ratio",
                "events", "checksum");
    bool match = true;
    for (int i = 0; i < ROWS; ++i) {
        if (!wanted(i)) continue;
        results[i].median_ns = median(samples[i]);
        const int against = results[i].reference;
        const Result& ref =
            results[against >= 0 && wanted(against) ? against : i];
        match = match && results[i].checksum == ref.checksum &&
                results[i].events == ref.events;
        std::printf("%-17s %10.1f %8.3f %9llu %016llx\n", results[i].name,
                    results[i].median_ns, results[i].median_ns / ref.median_ns,
                    static_cast<unsigned long long>(results[i].events),
                    static_cast<unsigned long long>(results[i].checksum));
    }
    std::printf("checksums %s\n", match ? "match" : "DIFFER");
    return acc.sink == 1 ? 3 : (match ? 0 : 1);
}
