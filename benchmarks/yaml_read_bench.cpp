// SPDX-License-Identifier: MIT
// Benchmark: reading large YAML. Two shapes are built in memory:
//
//   records  a block sequence of 11-line maps (id, name, cat, pid, tid, ts,
//            dur and an args map), the shape of a trace as YAML
//   nested   four top maps, four levels, 20 x 20 integer leaves per group
//
// Each shape is read these ways, parse and a full walk included:
//
//   parse     yaml::Document::parse over the whole text, no walk
//   document  yaml::Document::parse over the whole text, then the walk
//   tape      the same, with the tape reader forced instead of the line index
//   records   yaml::Records over a file of the text, chunks of --chunk MiB
//   records1  the same, chunks of 1 MiB
//   floor     the rapidyaml parse engine with a handler that does nothing,
//             the speed no reader of this parser can pass; no walk
//
// The walk of records sums `dur` and counts the POSIX records whose
// args.size is above 1024; the walk of nested sums every leaf. The document
// and records answers must equal the sums counted while the text is made.
//
// The table has the median MB/s of the input over the rounds (the cases
// alternate inside a round, the order shifting each round) and the bytes the
// case added to the memory the manager counts, at their largest, read every
// 100 microseconds by a thread: the tape of a document, the chunk and its
// tape for records. The input text and the file buffer count in the baseline
// only when they existed before the case started.
//
// Built only with DFTRACER_UTILS_BUILD_BENCHMARKS=ON and run manually.
//
//   yaml_read_bench [--mb N] [--rounds N] [--chunk MiB] [--dir PATH]
//                   [--only parse|document|tape|records|records1|floor]
//   yaml_read_bench --smoke

#include <bench_ok.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/yaml/document.h>
#include <dftracer/utils/yaml/document_test.h>
#include <dftracer/utils/yaml/records.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <rapidyaml.v0.16.0.singlesrc.hpp>

using namespace dftracer::utils;

namespace {

using Clock = std::chrono::steady_clock;

struct NoopState : ryml::ParserState {
    ryml::type_bits bits;
};

// A handler that ignores every event: the cost of the parse engine alone.
struct NoopHandler : ryml::EventHandlerStack<NoopHandler, NoopState> {
    using state = NoopState;
    static constexpr bool requires_strings_on_buffers = false;

    NoopHandler() : EventHandlerStack(ryml::Callbacks()) {
        _stack_reset_root();
        m_curr->bits = 0;
        m_curr->flags |= ryml::RUNK | ryml::RTOP;
    }
    ryml::Callbacks const& callbacks() const { return m_stack.m_callbacks; }
    ryml::TagDirectives& tag_directives() { return tag_directives_; }
    ryml::TagCache& tag_cache() { return tag_cache_; }
    template <ryml::type_bits B>
    bool has_any_() const noexcept {
        return (m_curr->bits & B) != 0;
    }
    void start_parse(const char* filename, ryml::substr src) {
        _stack_start_parse(filename, src);
    }
    void finish_parse() {}
    void cancel_parse() {}
    void begin_stream() const noexcept {}
    void end_stream() const noexcept {}
    void begin_doc() {
        if (_stack_should_push_on_begin_doc()) push_doc();
    }
    void end_doc() {
        if (_stack_should_pop_on_end_doc()) _pop();
    }
    void begin_doc_expl() { push_doc(); }
    void end_doc_expl() {
        if (_stack_should_pop_on_end_doc()) _pop();
    }
    void begin_map_key_flow() { _push(); }
    void begin_map_key_block() { _push(); }
    void begin_seq_key_flow() { _push(); }
    void begin_seq_key_block() { _push(); }
    void begin_map_val_flow() { begin(ryml::MAP); }
    void begin_map_val_block() { begin(ryml::MAP); }
    void begin_seq_val_flow() { begin(ryml::SEQ); }
    void begin_seq_val_block() { begin(ryml::SEQ); }
    void end_map_block() { _pop(); }
    void end_map_flow(bool, ryml::type_bits = ryml::FLOW_ML1) { _pop(); }
    void end_seq_block() { _pop(); }
    void end_seq_flow(bool, ryml::type_bits = ryml::FLOW_ML1) { _pop(); }
    void add_sibling() {}
    void actually_val_is_first_key_of_new_map_flow() { _push(); }
    void actually_val_is_first_key_of_new_map_block() {}
    void set_key_scalar_plain_empty() noexcept {}
    void set_val_scalar_plain_empty() noexcept { m_curr->bits |= ryml::VAL; }
    void set_key_scalar_plain(ryml::csubstr) noexcept {}
    void set_key_scalar_dquoted(ryml::csubstr) noexcept {}
    void set_key_scalar_squoted(ryml::csubstr) noexcept {}
    void set_key_scalar_literal(ryml::csubstr) noexcept {}
    void set_key_scalar_folded(ryml::csubstr) noexcept {}
    void set_val_scalar_plain(ryml::csubstr) noexcept {
        m_curr->bits |= ryml::VAL;
    }
    void set_val_scalar_dquoted(ryml::csubstr) noexcept {
        m_curr->bits |= ryml::VAL;
    }
    void set_val_scalar_squoted(ryml::csubstr) noexcept {
        m_curr->bits |= ryml::VAL;
    }
    void set_val_scalar_literal(ryml::csubstr) noexcept {
        m_curr->bits |= ryml::VAL;
    }
    void set_val_scalar_folded(ryml::csubstr) noexcept {
        m_curr->bits |= ryml::VAL;
    }
    void mark_key_scalar_unfiltered() noexcept {}
    void mark_val_scalar_unfiltered() noexcept {}
    void set_key_anchor(ryml::csubstr) {}
    void set_val_anchor(ryml::csubstr) {}
    void set_key_ref(ryml::csubstr) {}
    void set_val_ref(ryml::csubstr) {}
    void set_key_tag(ryml::csubstr) {}
    void set_val_tag(ryml::csubstr) {}
    void add_directive_yaml(ryml::csubstr) {}
    void add_directive_tag(ryml::csubstr, ryml::csubstr) {}
    ryml::substr arena() { return ryml::substr(arena_, arena_used_); }
    ryml::substr arena_rem() {
        return ryml::substr(arena_ + arena_used_, sizeof(arena_) - arena_used_);
    }
    ryml::substr alloc_arena(std::size_t len) {
        if (arena_used_ + len > sizeof(arena_)) std::abort();
        ryml::substr out(arena_ + arena_used_, len);
        arena_used_ += len;
        return out;
    }
    void _push() {
        _stack_push();
        m_curr->bits = 0;
    }
    void _pop() { _stack_pop(); }

    ryml::id_type m_curr_doc = 0;

   private:
    void begin(ryml::type_bits kind) {
        m_curr->bits |= kind;
        _push();
    }
    void push_doc() {
        _push();
        m_curr->bits = ryml::DOC;
    }

    ryml::TagDirectives tag_directives_;
    ryml::TagCache tag_cache_;
    char arena_[1 << 12];
    std::size_t arena_used_ = 0;
};

}  // namespace

#include <rapidyaml_impl.hpp>

namespace c4::yml {
template class ParseEngine<NoopHandler>;
}  // namespace c4::yml

namespace {

struct Rng {
    std::uint64_t s = 88172645463325252ull;
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
};

struct Answer {
    std::uint64_t sum = 0;
    std::uint64_t count = 0;
    bool operator==(const Answer&) const = default;
};

// `truth` is what a correct reader finds in the text, counted while it is
// generated.
dstd::String make_records(std::size_t megabytes, Answer& truth) {
    static const char* const NAMES[] = {"read",  "write", "open",
                                        "close", "pread", "fsync"};
    static const char* const CATS[] = {"POSIX", "STDIO", "MPIIO", "DLIO"};
    Rng rng;
    dstd::String out;
    out.reserve(megabytes * 1000000 + 1024);
    std::uint64_t id = 0;
    std::uint64_t ts = 1730000000123456ull;
    while (out.size() < megabytes * 1000000) {
        const std::uint64_t pid = 4000 + rng.next() % 2000;
        const std::uint64_t dur = rng.next() % 5000;
        const char* name = NAMES[rng.next() % 6];
        const char* cat = CATS[rng.next() % 4];
        const std::uint64_t file = rng.next() % 100000;
        const std::uint64_t size = 512u << (rng.next() % 8);
        ts += rng.next() % 100;
        truth.sum += dur;
        if (std::strcmp(cat, "POSIX") == 0 && size > 1024) ++truth.count;
        out += dstd::fmt<
            "- id: {}\n  name: {}\n  cat: {}\n  pid: {}\n  tid: {}\n  ts: {}\n"
            "  dur: {}\n  args:\n    fname: /p/lustre/x/file_{}.dat\n"
            "    size: {}\n    ret: {}\n">(id, name, cat, pid, pid, ts, dur,
                                           file, size, size);
        ++id;
    }
    return out;
}

dstd::String make_nested(std::size_t megabytes, Answer& truth) {
    constexpr int SUBS = 20;
    constexpr int LEAVES = 20;
    constexpr int TOP = 4;
    const int groups =
        std::max(1, static_cast<int>(static_cast<double>(megabytes) * 1e6 /
                                     (TOP * 12500.0)));
    Rng rng;
    dstd::String out;
    out.reserve(megabytes * 1000000 + 1024);
    for (int t = 0; t < TOP; ++t) {
        out += dstd::fmt<"top{}:\n">(t);
        for (int a = 0; a < groups; ++a) {
            out += dstd::fmt<"  grp_{}:\n">(a);
            for (int b = 0; b < SUBS; ++b) {
                out += dstd::fmt<"    sub_{}:\n">(b);
                for (int c = 0; c < LEAVES; ++c) {
                    const std::uint64_t leaf = rng.next() % 1000000;
                    truth.sum += leaf;
                    out += dstd::fmt<"      leaf_{}: {}\n">(c, leaf);
                }
            }
        }
    }
    return out;
}

void walk_record(yaml::Value record, Answer& answer) {
    if (auto dur = record.find("dur")) answer.sum += dur->as_uint().value_or(0);
    const auto cat = record.find("cat");
    const auto args = record.find("args");
    if (cat && args && cat->raw() == "POSIX") {
        const auto size = args->find("size");
        if (size && size->as_uint().value_or(0) > 1024) ++answer.count;
    }
}

void walk_leaves(yaml::Value value, Answer& answer) {
    if (value.is_object()) {
        value.for_each_member([&](dstd::StringView, yaml::Value member) {
            walk_leaves(member, answer);
        });
    } else if (auto n = value.as_uint()) {
        answer.sum += *n;
    }
}

enum class Shape { Records, Nested };

void walk(yaml::Value root, Shape shape, Answer& answer) {
    if (shape == Shape::Records) {
        root.for_each_element(
            [&](yaml::Value record) { walk_record(record, answer); });
    } else {
        walk_leaves(root, answer);
    }
}

// The largest live byte count the manager reports, sampled from another
// thread while the case runs.
class PeakSampler {
   public:
    PeakSampler()
        : thread_([this] {
              while (!stop_.load(std::memory_order_relaxed)) {
                  const std::size_t live =
                      memory::detail::Manager::get().stats().live;
                  std::size_t seen = peak_.load(std::memory_order_relaxed);
                  while (live > seen &&
                         !peak_.compare_exchange_weak(
                             seen, live, std::memory_order_relaxed)) {
                  }
                  usleep(100);
              }
          }) {}
    ~PeakSampler() { finish(); }
    std::size_t finish() {
        stop_.store(true, std::memory_order_relaxed);
        thread_.join();
        return peak_.load(std::memory_order_relaxed);
    }

   private:
    std::atomic<bool> stop_{false};
    std::atomic<std::size_t> peak_{0};
    dstd::Thread thread_;
};

struct Outcome {
    double seconds = 0;
    std::size_t extra_bytes = 0;
    Answer answer;
};

template <class Fn>
Outcome measure(Fn&& fn) {
    const std::size_t before = memory::detail::Manager::get().stats().live;
    PeakSampler sampler;
    const auto start = Clock::now();
    Outcome outcome;
    fn(outcome.answer);
    outcome.seconds =
        std::chrono::duration<double>(Clock::now() - start).count();
    const std::size_t peak = sampler.finish();
    outcome.extra_bytes = peak > before ? peak - before : 0;
    return outcome;
}

Outcome run_document(const dstd::String& text, Shape shape, bool walks,
                     bool force_tape = false) {
    dstd::String copy = text;
    yaml::detail::set_force_tape_for_test(force_tape);
    const Outcome outcome = measure([&](Answer& answer) {
        auto doc =
            dftu::bench::ok(yaml::Document::parse(std::move(copy), "bench"));
        if (walks) walk(doc.root(), shape, answer);
    });
    yaml::detail::set_force_tape_for_test(false);
    return outcome;
}

Outcome run_records(const dstd::Path& path, Shape shape,
                    std::size_t chunk_bytes) {
    yaml::RecordsOptions options;
    options.chunk_bytes = chunk_bytes;
    return measure([&](Answer& answer) {
        auto records = dftu::bench::ok(yaml::Records::open(path, options));
        for (;;) {
            auto next = dftu::bench::ok(records.next());
            if (!next) break;
            if (shape == Shape::Records)
                walk_record(*next, answer);
            else
                walk_leaves(*next, answer);
        }
    });
}

Outcome run_floor(const dstd::String& text) {
    dstd::String copy = text;
    return measure([&](Answer&) {
        NoopHandler handler;
        ryml::ParseEngine<NoopHandler> engine(
            &handler, ryml::ParserOptions().detect_flow_ml(false));
        engine.parse_in_place_ev({}, ryml::substr(copy.data(), copy.size()));
    });
}

double median(dstd::Vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t megabytes = 100;
    int rounds = 5;
    std::size_t chunk_mib = 64;
    dstd::String dir = "/tmp";
    dstd::StringView only;
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        const dstd::StringView arg = argv[i];
        if (arg == "--smoke") {
            smoke = true;
        } else if (arg == "--mb" && i + 1 < argc) {
            megabytes = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--rounds" && i + 1 < argc) {
            rounds = std::atoi(argv[++i]);
        } else if (arg == "--chunk" && i + 1 < argc) {
            chunk_mib = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--dir" && i + 1 < argc) {
            dir = argv[++i];
        } else if (arg == "--only" && i + 1 < argc) {
            only = argv[++i];
        } else {
            std::fprintf(
                stderr,
                "usage: yaml_read_bench [--mb N] [--rounds N] "
                "[--chunk MiB] [--dir PATH] [--only CASE] | --smoke\n");
            return 2;
        }
    }
    std::size_t chunk_bytes = chunk_mib << 20;
    if (smoke) {
        megabytes = 2;
        rounds = 1;
        chunk_bytes = std::size_t{256} << 10;
    }

    const dstd::Path file(dstd::fmt<"{}/yaml_read_bench_{}.yaml">(
        dir, static_cast<long>(getpid())));
    int failures = 0;
    std::printf("%-8s %-9s %9s %10s %9s\n", "shape", "case", "MB/s", "extra MB",
                "x input");
    for (const Shape shape : {Shape::Records, Shape::Nested}) {
        const char* shape_name = shape == Shape::Records ? "records" : "nested";
        Answer truth;
        const dstd::String text = shape == Shape::Records
                                      ? make_records(megabytes, truth)
                                      : make_nested(megabytes, truth);
        dftu::bench::ok(dstd::fs::write_file(file, text));
        const double input_mb = static_cast<double>(text.size()) / 1e6;

        constexpr int CASES = 6;
        static const char* const NAMES[CASES] = {
            "parse", "document", "tape", "records", "records1", "floor"};
        dstd::Vector<double> seconds[CASES];
        Outcome outcomes[CASES];
        for (int round = 0; round < rounds; ++round) {
            for (int step = 0; step < CASES; ++step) {
                const int which = (step + round) % CASES;
                if (!only.empty() && only != NAMES[which]) continue;
                Outcome& outcome = outcomes[which];
                switch (which) {
                    case 0:
                        outcome = run_document(text, shape, false);
                        break;
                    case 1:
                        outcome = run_document(text, shape, true);
                        break;
                    case 2:
                        outcome = run_document(text, shape, true, true);
                        break;
                    case 3:
                        outcome = run_records(file, shape, chunk_bytes);
                        break;
                    case 4:
                        outcome =
                            run_records(file, shape, std::size_t{1} << 20);
                        break;
                    default:
                        outcome = run_floor(text);
                        break;
                }
                seconds[which].push_back(outcome.seconds);
            }
        }
        for (const int which : {1, 2, 3, 4}) {
            if (!seconds[which].empty() && !(outcomes[which].answer == truth)) {
                std::fprintf(stderr, "%s: %s answer is not the generated one\n",
                             shape_name, NAMES[which]);
                ++failures;
            }
        }
        const auto row = [&](const char* name, double seconds,
                             const Outcome& outcome) {
            std::printf("%-8s %-9s %9.0f %10.1f %9.2f\n", shape_name, name,
                        input_mb / seconds,
                        static_cast<double>(outcome.extra_bytes) / 1e6,
                        static_cast<double>(outcome.extra_bytes) /
                            static_cast<double>(text.size()));
        };
        for (int which = 0; which < CASES; ++which)
            if (!seconds[which].empty())
                row(NAMES[which], median(seconds[which]), outcomes[which]);
        std::printf("%-8s answer sum=%llu count=%llu\n", shape_name,
                    static_cast<unsigned long long>(outcomes[1].answer.sum),
                    static_cast<unsigned long long>(outcomes[1].answer.count));
    }
    dftu::bench::ok(dstd::fs::remove(file));
    return failures == 0 ? 0 : 1;
}
