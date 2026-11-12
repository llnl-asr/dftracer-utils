// SPDX-License-Identifier: MIT
// Benchmark: reading large YAML. Two shapes are built in memory:
//
//   records  a block sequence of 11-line maps (id, name, cat, pid, tid, ts,
//            dur and an args map), the shape of a trace as YAML
//   nested   four top maps, four levels, 20 x 20 integer leaves per group
//   multiline  the records, each with a `|` block of 3 to 5 lines and a plain
//            value of two lines
//
// Each shape is read these ways, parse and a full walk included:
//
//   parse     yaml::Document::parse over the whole text, no walk
//   document  yaml::Document::parse over the whole text, then the walk
//   records   yaml::Records over a file of the text, read in 1 MiB blocks,
//             chunks of --chunk MiB
//   records1  the same, chunks of 1 MiB
//   parse_par yaml::Document::parse with its chunks indexed on the threads
//             of a Runtime, no walk
//   doc_par   the same, then the walk
//   parallel  the same as records, with batches of two chunks of --pchunk
//             MiB per thread of a Runtime, the text read in 1 MiB blocks and
//             each chunk parsed and walked in parallel_for
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
//                   [--pchunk MiB] [--threads N]
//                   [--only
//                   parse|document|records|records1|parallel|parse_par|doc_par]
//                   [--shape records|nested|multiline]
//   yaml_read_bench --smoke

#include <bench_ok.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/yaml/document.h>
#include <dftracer/utils/yaml/records.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace dftracer::utils;

namespace {

using Clock = std::chrono::steady_clock;

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

dstd::String make_multiline(std::size_t megabytes, Answer& truth) {
    static const char* const CATS[] = {"POSIX", "STDIO", "MPIIO", "DLIO"};
    Rng rng;
    dstd::String out;
    out.reserve(megabytes * 1000000 + 1024);
    std::uint64_t id = 0;
    while (out.size() < megabytes * 1000000) {
        const std::uint64_t pid = 4000 + rng.next() % 2000;
        const std::uint64_t dur = rng.next() % 5000;
        const char* cat = CATS[rng.next() % 4];
        const std::uint64_t size = 512u << (rng.next() % 8);
        const std::uint64_t lines = 3 + rng.next() % 3;
        truth.sum += dur;
        if (std::strcmp(cat, "POSIX") == 0 && size > 1024) ++truth.count;
        out += dstd::fmt<
            "- id: {}\n  cat: {}\n  pid: {}\n  dur: {}\n  args:\n"
            "    size: {}\n  note: |\n">(id, cat, pid, dur, size);
        for (std::uint64_t l = 0; l < lines; ++l) {
            const dstd::String line =
                dstd::fmt<"note line {} of record {}">(l, id);
            out += "    " + line + "\n";
            truth.sum += line.size() + 1;
        }
        const dstd::String first = dstd::fmt<"first part of {}">(id);
        const dstd::String second = dstd::fmt<"second part of {}">(id);
        out += "  desc: " + first + "\n    " + second + "\n";
        truth.sum += first.size() + 1 + second.size();
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

enum class Shape { Records, Nested, Multiline };

void walk_multiline(yaml::Value record, Answer& answer) {
    walk_record(record, answer);
    if (auto note = record.find("note")) answer.sum += note->raw().size();
    if (auto desc = record.find("desc")) answer.sum += desc->raw().size();
}

void walk(yaml::Value root, Shape shape, Answer& answer) {
    if (shape == Shape::Multiline) {
        root.for_each_element(
            [&](yaml::Value record) { walk_multiline(record, answer); });
    } else if (shape == Shape::Records) {
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

Outcome run_document(const dstd::String& text, Shape shape, bool walks) {
    dstd::String copy = text;
    return measure([&](Answer& answer) {
        auto doc =
            dftu::bench::ok(yaml::Document::parse(std::move(copy), "bench"));
        if (walks) walk(doc.root(), shape, answer);
    });
}

// Document::parse with its chunks indexed on the threads of `runtime`.
Outcome run_document_parallel(const dstd::String& text, Shape shape, bool walks,
                              Runtime& runtime) {
    dstd::String copy = text;
    const auto run_jobs =
        [&](std::size_t n,
            dstd::FunctionRef<void(std::size_t, std::size_t)> body) {
            dftu::bench::ok(
                runtime.parallel_for(static_cast<std::int64_t>(n), 1,
                                     [&](std::int64_t begin, std::int64_t end) {
                                         body(static_cast<std::size_t>(begin),
                                              static_cast<std::size_t>(end));
                                     }));
        };
    return measure([&](Answer& answer) {
        auto doc = dftu::bench::ok(
            yaml::Document::parse(std::move(copy), "bench", run_jobs));
        if (walks) walk(doc.root(), shape, answer);
    });
}

constexpr std::size_t READ_BLOCK = std::size_t{1} << 20;

Outcome run_records(const dstd::Path& path, Shape shape,
                    std::size_t chunk_bytes) {
    yaml::RecordsOptions options;
    options.chunk_bytes = chunk_bytes;
    return measure([&](Answer& answer) {
        auto file =
            dftu::bench::ok(dstd::File::open(path, dstd::OpenOptions{}));
        auto records = yaml::Records::from_blocks(path.str(), options);
        std::uint64_t offset = 0;
        for (;;) {
            auto next = dftu::bench::ok(records.next());
            if (!next) {
                if (!records.needs_input()) break;
                dstd::Span<char> space = records.input_space(READ_BLOCK);
                const std::size_t got = dftu::bench::ok(file.read_at(
                    offset, dstd::Span<std::uint8_t>(
                                reinterpret_cast<std::uint8_t*>(space.data()),
                                space.size())));
                if (got == 0) {
                    records.end_input();
                } else {
                    records.commit_input(got);
                    offset += got;
                }
                continue;
            }
            if (shape == Shape::Multiline)
                walk_multiline(*next, answer);
            else if (shape == Shape::Records)
                walk_record(*next, answer);
            else
                walk_leaves(*next, answer);
        }
    });
}

void walk_one(yaml::Value record, Shape shape, Answer& answer) {
    if (shape == Shape::Multiline)
        walk_multiline(record, answer);
    else if (shape == Shape::Records)
        walk_record(record, answer);
    else
        walk_leaves(record, answer);
}

void read_full(const dstd::File& file, std::uint64_t offset, char* out,
               std::size_t size) {
    while (size > 0) {
        const std::size_t got = dftu::bench::ok(file.read_at(
            offset, dstd::Span<std::uint8_t>(
                        reinterpret_cast<std::uint8_t*>(out), size)));
        if (got == 0) dstd::fatal("bench: the file got shorter");
        offset += got;
        out += got;
        size -= got;
    }
}

Outcome run_parallel(const dstd::Path& path, Shape shape,
                     std::size_t chunk_bytes, Runtime& runtime) {
    yaml::RecordsOptions options;
    options.chunk_bytes = chunk_bytes;
    options.batch_chunks = 2 * runtime.threads();
    return measure([&](Answer& answer) {
        auto file =
            dftu::bench::ok(dstd::File::open(path, dstd::OpenOptions{}));
        auto records = yaml::Records::from_blocks(path.str(), options);
        const std::uint64_t file_size = dftu::bench::ok(file.size());
        // A quarter of a batch at a time keeps the buffer near one batch.
        const std::size_t read_bytes =
            std::max(chunk_bytes * options.batch_chunks / 4, READ_BLOCK);
        dstd::Vector<Answer> parts(options.batch_chunks);
        std::uint64_t offset = 0;
        for (;;) {
            const auto chunks = dftu::bench::ok(records.next_chunks());
            if (chunks.empty()) {
                if (!records.needs_input()) break;
                // The next part of the batch, in blocks read in parallel.
                const std::size_t size = static_cast<std::size_t>(
                    std::min<std::uint64_t>(read_bytes, file_size - offset));
                if (size == 0) {
                    records.end_input();
                    continue;
                }
                dstd::Span<char> space = records.input_space(size);
                const auto blocks = static_cast<std::int64_t>(
                    (size + READ_BLOCK - 1) / READ_BLOCK);
                dftu::bench::ok(runtime.parallel_for(
                    blocks, 1, [&](std::int64_t begin, std::int64_t end) {
                        for (auto b = begin; b < end; ++b) {
                            const auto at =
                                static_cast<std::size_t>(b) * READ_BLOCK;
                            read_full(file, offset + at, space.data() + at,
                                      std::min(READ_BLOCK, size - at));
                        }
                    }));
                records.commit_input(size);
                offset += size;
                continue;
            }
            dftu::bench::ok(runtime.parallel_for(
                static_cast<std::int64_t>(chunks.size()), 1,
                [&](std::int64_t begin, std::int64_t end) {
                    for (auto i = static_cast<std::size_t>(begin);
                         i < static_cast<std::size_t>(end); ++i) {
                        while (auto record = dftu::bench::ok(chunks[i].next()))
                            walk_one(*record, shape, parts[i]);
                    }
                }));
            for (std::size_t i = 0; i < chunks.size(); ++i) {
                answer.sum += parts[i].sum;
                answer.count += parts[i].count;
                parts[i] = Answer{};
            }
        }
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
    std::size_t parallel_chunk_mib = 2;
    std::size_t threads = 0;
    dstd::String dir = "/tmp";
    dstd::StringView only;
    dstd::StringView only_shape;
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
        } else if (arg == "--pchunk" && i + 1 < argc) {
            parallel_chunk_mib = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--dir" && i + 1 < argc) {
            dir = argv[++i];
        } else if (arg == "--only" && i + 1 < argc) {
            only = argv[++i];
        } else if (arg == "--shape" && i + 1 < argc) {
            only_shape = argv[++i];
        } else {
            std::fprintf(
                stderr,
                "usage: yaml_read_bench [--mb N] [--rounds N] "
                "[--chunk MiB] [--pchunk MiB] [--threads N] [--dir PATH] "
                "[--only CASE] [--shape SHAPE] | "
                "--smoke\n");
            return 2;
        }
    }
    std::size_t chunk_bytes = chunk_mib << 20;
    std::size_t parallel_chunk_bytes = parallel_chunk_mib << 20;
    if (smoke) {
        megabytes = 2;
        rounds = 1;
        chunk_bytes = std::size_t{256} << 10;
        parallel_chunk_bytes = std::size_t{64} << 10;
        threads = 2;
    }
    Runtime runtime(threads);

    const dstd::Path file(dstd::fmt<"{}/yaml_read_bench_{}.yaml">(
        dir, static_cast<long>(getpid())));
    int failures = 0;
    std::printf("%-9s %-9s %9s %10s %9s\n", "shape", "case", "MB/s", "extra MB",
                "x input");
    for (const Shape shape :
         {Shape::Records, Shape::Nested, Shape::Multiline}) {
        const char* shape_name = shape == Shape::Records  ? "records"
                                 : shape == Shape::Nested ? "nested"
                                                          : "multiline";
        if (!only_shape.empty() && only_shape != shape_name) continue;
        Answer truth;
        const dstd::String text =
            shape == Shape::Records  ? make_records(megabytes, truth)
            : shape == Shape::Nested ? make_nested(megabytes, truth)
                                     : make_multiline(megabytes, truth);
        dftu::bench::ok(dstd::fs::write_file(file, text));
        const double input_mb = static_cast<double>(text.size()) / 1e6;

        constexpr int CASES = 7;
        static const char* const NAMES[CASES] = {
            "parse",    "document",  "records", "records1",
            "parallel", "parse_par", "doc_par"};
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
                        outcome = run_records(file, shape, chunk_bytes);
                        break;
                    case 3:
                        outcome =
                            run_records(file, shape, std::size_t{1} << 20);
                        break;
                    case 4:
                        outcome = run_parallel(file, shape,
                                               parallel_chunk_bytes, runtime);
                        break;
                    case 5:
                        outcome =
                            run_document_parallel(text, shape, false, runtime);
                        break;
                    default:
                        outcome =
                            run_document_parallel(text, shape, true, runtime);
                        break;
                }
                seconds[which].push_back(outcome.seconds);
            }
        }
        for (const int which : {1, 2, 3, 4, 6}) {
            if (!seconds[which].empty() && !(outcomes[which].answer == truth)) {
                std::fprintf(stderr, "%s: %s answer is not the generated one\n",
                             shape_name, NAMES[which]);
                ++failures;
            }
        }
        const auto row = [&](const char* name, double median_seconds,
                             const Outcome& outcome) {
            std::printf("%-9s %-9s %9.0f %10.1f %9.2f\n", shape_name, name,
                        input_mb / median_seconds,
                        static_cast<double>(outcome.extra_bytes) / 1e6,
                        static_cast<double>(outcome.extra_bytes) /
                            static_cast<double>(text.size()));
        };
        for (int which = 0; which < CASES; ++which)
            if (!seconds[which].empty())
                row(NAMES[which], median(seconds[which]), outcomes[which]);
        std::printf("%-9s answer sum=%llu count=%llu\n", shape_name,
                    static_cast<unsigned long long>(outcomes[1].answer.sum),
                    static_cast<unsigned long long>(outcomes[1].answer.count));
    }
    dftu::bench::ok(dstd::fs::remove(file));
    return failures == 0 ? 0 : 1;
}
