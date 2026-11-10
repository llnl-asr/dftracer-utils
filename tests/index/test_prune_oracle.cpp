// SPDX-License-Identifier: MIT
// Prune oracle: file verdicts and chunk sets of every pruning consumer over a
// fixed, generated corpus and query list, compared with a recording. The View
// consumer is `gather_units`, the path every View scan plans through. Dynamic
// narrowing is not recorded: it races the running scan, and it prunes through
// ChunkPruner, which is. Set
// DFTRACER_UTILS_PRUNE_ORACLE_RECORD=<path> to write a new recording instead.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/index/plan/rowsets.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <generator_ok.h>
#include <index_test_helpers.h>
#include <mmix_lcg.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::index;

namespace {

constexpr const char* NAMES[] = {"read",  "write",   "open", "close",
                                 "train", "forward", "mmap", "fsync"};
constexpr const char* CATS[] = {"POSIX", "STDIO", "COMPUTE", "AI"};
constexpr const char* FILES[] = {"/data/a.dat", "/data/b.dat", "/data/c.dat"};
constexpr const char* HOSTS[] = {"node-1", "node-2"};
constexpr const char* FHASH[] = {"1a2b3c4d5e6f7081", "92a3b4c5d6e7f809",
                                 "a0b1c2d3e4f50617"};
constexpr const char* HHASH[] = {"c0ffee00c0ffee00", "deadbeefdeadbeef"};
constexpr const char* EXECS[] = {"/usr/bin/python", "/usr/bin/ior"};
constexpr const char* SHASH[] = {"5a5a5a5a00000001", "5a5a5a5a00000002"};

constexpr std::size_t MEMBER_BYTES = 32 * 1024;
constexpr std::size_t LARGE_MEMBER_BYTES = 256 * 1024;
// The 4 MB default buffer would zero-fill per scan unit for no gain.
constexpr std::size_t SCAN_BUFFER_BYTES = MEMBER_BYTES;

// Deterministic linear congruential generator; the corpus must not change
// between the recording and the check.
struct Lcg {
    std::uint64_t s;
    std::uint64_t next() {
        s = mmix_lcg_step(s);
        return s >> 33;
    }
};

// Events come in runs of one name/category/pid so chunks differ; hash args and
// FH/HH metadata make fhash/hhash resolvable through the dictionaries. Seed 2
// holds a block of metadata records longer than a member, so one member has no
// data event and no statistics rows. Seed 3 uses thousands of distinct names in
// large members, so its name dictionaries overflow and postings and blooms have
// to answer; every trace jumps in time mid-run so the ts histogram prunes where
// min/max cannot.
dstd::String write_trace(const dstd::String& dir, int seed, int events) {
    Lcg rng{static_cast<std::uint64_t>(seed) * 7919};
    dstd::String out;
    out += "[\n";
    for (int f = 0; f < 3; ++f)
        out += common::str_cat(
            R"({"name":"FH","ph":"M","pid":1,"tid":1,"args":{"name":")",
            FILES[f], R"(","value":")", FHASH[f], "\"}}\n");
    for (int h = 0; h < 2; ++h)
        out += common::str_cat(
            R"({"name":"HH","ph":"M","pid":1,"tid":1,"args":{"name":")",
            HOSTS[h], R"(","value":")", HHASH[h], "\"}}\n");
    for (int x = 0; x < 2; ++x)
        out += common::str_cat(
            R"({"name":"SH","ph":"M","pid":1,"tid":1,"args":{"name":")",
            EXECS[x], R"(","value":")", SHASH[x], "\"}}\n");
    std::uint64_t ts = 1000000 + static_cast<std::uint64_t>(seed) * 50000;
    int run_left = 0, name = 0, cat = 0, pid = 1, file = 0, host = 0;
    for (int i = 0; i < events; ++i) {
        if (run_left == 0) {
            run_left = 200 + static_cast<int>(rng.next() % 400);
            name = static_cast<int>(rng.next() % 8);
            cat = static_cast<int>(rng.next() % 4);
            pid = 1 + static_cast<int>(rng.next() % 4);
            file = static_cast<int>(rng.next() % 3);
            host = static_cast<int>(rng.next() % 2);
        }
        --run_left;
        if (seed == 2 && i == events / 3)
            for (int t = 0; t < 2000; ++t)
                out += common::str_cat(
                    R"({"name":"thread_name","ph":"M","pid":1,"tid":)",
                    1000 + t, R"(,"args":{"name":"worker-)", t, "\"}}\n");
        ts += 5 + rng.next() % 50;
        if (i == events / 2) ts += 400000;
        const auto dur = 10 + (rng.next() % 1000);
        const auto size = (rng.next() % 64) * 128;
        dstd::String op = NAMES[name];
        if (seed == 3) {
            // Three draws in a fixed order; argument evaluation order is
            // unspecified and differs between compilers.
            const auto a = static_cast<unsigned long long>(rng.next());
            const auto b = static_cast<unsigned long long>(rng.next());
            const auto c = static_cast<unsigned long long>(rng.next());
            char buf[32];
            std::snprintf(buf, sizeof(buf), "n%08llx%08llx%08llx", a, b, c);
            op = (i < 200 && i % 5 == 0) ? "target_op" : buf;
        }
        out += common::str_cat(
            R"({"id":)", i, R"(,"name":")", op, R"(","cat":")", CATS[cat],
            R"(","pid":)", pid, R"(,"tid":)", pid * 10, R"(,"ph":"X","ts":)",
            ts, R"(,"dur":)", dur, R"(,"args":{"size":)", size, R"(,"fhash":")",
            FHASH[file], R"(","hhash":")", HHASH[host], R"(","shash":")",
            SHASH[pid % 2], "\"}}\n");
    }
    out += "]\n";
    const auto plain = dir + "/trace" + dstd::to_string(seed) + ".pfw";
    dstd::fs::write_file(dstd::PathView(plain), out).discard();
    const auto gz = plain + ".gz";
    const std::size_t member = seed == 3 ? LARGE_MEMBER_BYTES : MEMBER_BYTES;
    REQUIRE(
        dftu_utils_test::compress_file_to_gzip_multimember(plain, gz, member));
    dstd::fs::remove(plain).discard();
    return gz;
}

const dstd::Vector<dstd::String> QUERIES = {
    R"(name == "read")",
    R"(name == "absent")",
    R"(name != "read")",
    R"(name in ["read", "write"])",
    R"(name not in ["read", "write", "open"])",
    R"(cat == "COMPUTE")",
    R"(cat != "POSIX")",
    R"(cat in ["AI", "STDIO"])",
    R"(pid == 2)",
    R"(pid > 3)",
    R"(dur >= 900)",
    R"(dur < 20)",
    R"(args.size == 256)",
    R"(args.size > 8000)",
    R"(ts >= 1200000)",
    R"(ts > 1100000 and ts < 1150000)",
    R"(name == "read" and pid == 1)",
    R"(name == "read" or cat == "COMPUTE")",
    R"(not (dur > 25))",
    R"(not (name == "read"))",
    R"(not (cat in ["POSIX", "STDIO"]))",
    R"(name like "re%")",
    R"(fhash == "/data/b.dat")",
    R"(fhash == "92a3b4c5d6e7f809")",
    R"(hhash == "node-2")",
    R"(hhash == "deadbeefdeadbeef" and name == "write")",
    R"((name == "train" or name == "forward") and not (pid == 1))",
    R"(name == "target_op")",
    R"(name == "absent_op")",
    R"(name in ["target_op", "absent_op"])",
    R"(not (name == "target_op"))",
    R"(ts > 1150000 and ts < 1400000)",
    R"(ts >= 1400000 and ts <= 1450000)",
    R"(not (cat == "COMPUTE"))",
    R"(name == "thread_name")",
    // The key sets an arrow into the files and hosts row sets pushes into
    // the scan, as `fhash -> files.path == "/data/b.dat"` does.
    R"(fhash in ["92a3b4c5d6e7f809"])",
    R"(fhash in ["92a3b4c5d6e7f809", "a0b1c2d3e4f50617"])",
    R"(hhash in ["deadbeefdeadbeef"])",
    R"(fhash in ["1a2b3c4d5e6f7081"] and name == "read")",
};

using TimeRange = dstd::Optional<dstd::Pair<double, double>>;
const dstd::Vector<TimeRange> TIME_RANGES = {
    dstd::nullopt,
    dstd::Pair<double, double>{1050000.0, 1150000.0},
    dstd::Pair<double, double>{0.0, 1020000.0},
    dstd::Pair<double, double>{1300000.0, 0.0},
};

template <class Values>
dstd::String join(const Values& v) {
    dstd::String s;
    for (auto x : v) {
        if (!s.empty()) s += ',';
        s += dstd::to_string(x);
    }
    return s;
}

dstd::String range_str(const TimeRange& r) {
    if (!r) return "-";
    return dstd::to_string(static_cast<long long>(r->first)) + ".." +
           dstd::to_string(static_cast<long long>(r->second));
}

struct Corpus {
    dftu_utils_test::TestEnvironment env{10};
    dstd::String index_dir;
    dstd::Vector<dstd::String> traces;

    Corpus() {
        index_dir = env.dir() + "/idx";
        for (int seed : {1, 2, 3})
            traces.push_back(write_trace(env.dir(), seed, 6000));
        index::IndexerOptions opts;
        opts.index_dir = index_dir;
        index::Indexer::open(dftu_utils_test::dstd_paths(traces), opts).build();
    }
    dstd::String index_of(const dstd::String& trace) const {
        return dstd::String(
            trace::internal::determine_index_path(trace, index_dir));
    }
};

dstd::Vector<dstd::String> run_oracle(const Corpus& c) {
    dstd::Vector<dstd::String> lines;
    for (const auto& trace : c.traces) {
        const dstd::String base(dstd::PathView(trace).filename());
        const auto index_path = c.index_of(trace);
        const auto snapshot = dftu_utils_test::index_snapshot(index_path);
        const auto file =
            dftu_utils_test::require_indexed_file(snapshot, trace);
        const auto members =
            dftu_utils_test::index_take(index::members(snapshot, file)).size();
        const auto uc =
            dftu_utils_test::index_take(index::file_metadata(snapshot, file))
                ->max_bytes;

        for (const auto& text : QUERIES) {
            const auto q = dftu_utils_test::ok(duql::Query::from_string(text));
            const dstd::String key = base + "|" + text + "|";

            plan::ChunkPruner pruner;
            auto one = dftu_utils_test::ok(
                pruner(plan::ChunkPrunerInput{
                           dstd::String(dstd::StringView(index_path)),
                           dstd::String(dstd::StringView(trace)), q})
                    .get());
            lines.push_back(
                key + "-|pruner|" +
                dstd::to_string(static_cast<int>(one.success)) + "/" +
                dstd::to_string(static_cast<int>(one.file_may_match)) + "/" +
                dstd::to_string(one.total_checkpoints) + "|" +
                join(one.candidate_checkpoints));

            plan::ChunkPrunerBatchInput batch;
            batch.index_path = index_path;
            batch.items.push_back({dstd::String(dstd::StringView(trace)), q});
            auto many = pruner.process_batch(batch);
            REQUIRE(many);
            const auto& b = many->outputs.at(0);
            lines.push_back(
                key + "-|batch|" +
                dstd::to_string(static_cast<int>(b.success)) + "/" +
                dstd::to_string(static_cast<int>(b.file_may_match)) + "/" +
                dstd::to_string(b.total_checkpoints) + "|" +
                join(b.candidate_checkpoints));

            for (const auto& tr : TIME_RANGES) {
                trace::views::detail::ViewPlan vp;
                vp.files.push_back({dstd::String(trace),
                                    dstd::String(index_path), uc, members, 0});
                vp.query = q;
                vp.time_range = tr;
                const auto vdef = trace::views::detail::make_vdef(vp, false);
                std::uint64_t skipped = 0;
                auto units = dftu_utils_test::ok(

                    trace::views::detail::gather_units(vp, vdef, skipped)
                        .get());
                dstd::Vector<std::uint64_t> ckpts;
                for (const auto& u : units) ckpts.push_back(u.checkpoint_idx);
                lines.push_back(
                    key + range_str(tr) + "|view|" +
                    dstd::to_string(static_cast<int>(!units.empty())) + "/" +
                    dstd::to_string(skipped) + "|" + join(ckpts));
            }
        }
    }
    return lines;
}

dstd::String recording_path() {
    return dstd::String((dstd::Path(DFTRACER_UTILS_TEST_SOURCE_DIR) / "index" /
                         "data" / "prune_oracle.txt")
                            .str());
}

std::size_t count_json(const dstd::String& trace, const dstd::String& index_dir,
                       const dstd::String& text, bool skip_pruning) {
    auto run = [&]() -> coro::CoroTask<std::size_t> {
        utilities::reader::TraceReader reader(
            {.file_path = dstd::String(trace),
             .index_dir = dstd::String(index_dir)});
        utilities::reader::ReadConfig cfg;
        cfg.query = text;
        cfg.skip_pruning = skip_pruning;
        cfg.buffer_size = SCAN_BUFFER_BYTES;
        auto gen = dftu_utils_test::ok_items(reader.read_json(cfg));
        std::size_t n = 0;
        while (auto line = co_await gen.next()) ++n;
        co_return n;
    };
    return dftu_utils_test::ok(run().get());
}

coro::CoroTask<std::uint64_t> scan_unit(
    const trace::views::detail::ScanUnit& u,
    const trace::views::ViewDefinition& vdef) {
    auto sin = trace::views::detail::make_scanner_input(u, vdef, vdef.query);
    sin.with_batch_size(SCAN_BUFFER_BYTES);
    trace::views::ViewScannerUtility scanner;
    auto gen = scanner(sin);
    std::uint64_t n = 0;
    while (auto b = co_await gen.next()) n += b->events_matched;
    co_return n;
}

std::uint64_t count_view(const dstd::String& trace,
                         const dstd::String& index_path,
                         const dstd::String& text) {
    trace::views::detail::ViewPlan vp;
    vp.files.push_back(
        {dstd::String(trace), dstd::String(index_path), 0, 0, 0});
    vp.query = dftu_utils_test::ok(duql::Query::from_string(text));
    const auto vdef = trace::views::detail::make_vdef(vp, false);
    auto run = [&]() -> coro::CoroTask<std::uint64_t> {
        std::uint64_t skipped = 0;
        const auto units =
            co_await trace::views::detail::gather_units(vp, vdef, skipped);
        std::uint64_t n = 0;
        for (const auto& u : units) n += co_await scan_unit(u, vdef);
        co_return n;
    };
    return dftu_utils_test::ok(run().get());
}

// The View scan without pruning: the whole file as one unit.
std::uint64_t count_view_unpruned(const dstd::String& trace,
                                  const dstd::String& index_path,
                                  const dstd::String& text) {
    trace::views::detail::ViewPlan vp;
    vp.query = dftu_utils_test::ok(duql::Query::from_string(text));
    const auto vdef = trace::views::detail::make_vdef(vp, false);
    const auto snapshot = dftu_utils_test::index_snapshot(index_path);
    const auto spans = dftu_utils_test::index_take(index::spans(
        snapshot, dftu_utils_test::require_indexed_file(snapshot, trace)));
    REQUIRE_FALSE(spans.empty());
    dstd::SharedPtr<const duql::Query> q;
    const trace::views::detail::ScanUnit u{
        dstd::String(trace),
        dstd::String(index_path),
        trace::views::detail::checkpoint_size_or_default(0),
        0,
        0,
        spans.back().uc_offset + spans.back().uc_size,
        0,
        q,
        nullptr};
    return dftu_utils_test::ok(scan_unit(u, vdef).get());
}

}  // namespace

TEST_CASE("prune decisions match the recording") {
    Corpus corpus;
    const auto lines = run_oracle(corpus);

    if (const char* out = std::getenv("DFTRACER_UTILS_PRUNE_ORACLE_RECORD")) {
        dstd::String f;
        for (const auto& l : lines) f += common::str_cat(l, "\n");
        MESSAGE("recorded " << lines.size() << " lines to "
                            << dstd::String(out));
        dstd::fs::write_file(dstd::PathView(out), f).discard();
        return;
    }

    const auto recorded = dstd::fs::read_file(dstd::PathView(recording_path()));
    REQUIRE_MESSAGE(recorded.has_value(), "missing " << recording_path());
    dstd::Vector<dstd::String> expected;
    dstd::StringView rest = *recorded;
    while (!rest.empty()) {
        const std::size_t eol = rest.find('\n');
        expected.push_back(dstd::String(rest.substr(0, eol)));
        rest = eol == dstd::StringView::npos ? dstd::StringView{}
                                             : rest.substr(eol + 1);
    }
    REQUIRE(expected.size() == lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        CAPTURE(i);
        CHECK(lines[i] == expected[i]);
    }
}

TEST_CASE("pruning never drops a matching event") {
    Corpus corpus;
    for (const auto& trace : corpus.traces) {
        for (const auto& text : QUERIES) {
            CAPTURE(trace);
            CAPTURE(text);
            const auto full = count_json(trace, corpus.index_dir, text, true);
            CHECK(count_json(trace, corpus.index_dir, text, false) == full);
            CHECK(count_view(trace, corpus.index_of(trace), text) ==
                  count_view_unpruned(trace, corpus.index_of(trace), text));
        }
    }
}

namespace {

dstd::Vector<std::uint64_t> view_chunks(const dstd::String& trace,
                                        const dstd::String& index_path,
                                        const dstd::String& text) {
    const auto snapshot = dftu_utils_test::index_snapshot(index_path);
    const auto file = dftu_utils_test::require_indexed_file(snapshot, trace);
    trace::views::detail::ViewPlan vp;
    vp.files.push_back(
        {dstd::String(trace), dstd::String(index_path),
         dftu_utils_test::index_take(index::file_metadata(snapshot, file))
             ->max_bytes,
         dftu_utils_test::index_take(index::members(snapshot, file)).size(),
         0});
    vp.query = dftu_utils_test::ok(duql::Query::from_string(text));
    const auto vdef = trace::views::detail::make_vdef(vp, false);
    std::uint64_t skipped = 0;
    dstd::Vector<std::uint64_t> out;
    const auto units = dftu_utils_test::ok(
        trace::views::detail::gather_units(vp, vdef, skipped).get());
    for (const auto& u : units) out.push_back(u.checkpoint_idx);
    return out;
}

using Manifest = dstd::Map<dstd::String, index::ExtensionEntry>;

Manifest manifest_of(const index::Indexer& ix, const dstd::String& trace) {
    Manifest out;
    for (const auto& f : ix.manifest())
        if (dstd::StringView(f.path) == dstd::StringView(trace))
            for (const auto& e : f.extensions) out[e.name] = e;
    return out;
}

// One query per pruning kind: postings, counts, zonemap, bloom, time.
const dstd::Vector<dstd::String> ROW_QUERIES = {
    R"(name == "read")", R"(cat == "COMPUTE")", R"(args.size > 8000)",
    R"(name == "target_op")", R"(ts >= 1200000)"};

dstd::Vector<std::uint64_t> rows(const Corpus& c) {
    dstd::Vector<std::uint64_t> out;
    for (const auto& trace : c.traces)
        for (const auto& text : ROW_QUERIES)
            out.push_back(count_view(trace, c.index_of(trace), text));
    return out;
}

index::IndexerOptions corpus_options(const Corpus& c) {
    index::IndexerOptions o;
    o.index_dir = c.index_dir;
    return o;
}

}  // namespace

TEST_CASE("explain reads the chunks a View scan reads") {
    Corpus corpus;
    auto ix = index::Indexer::open(dftu_utils_test::dstd_paths(corpus.traces),
                                   corpus_options(corpus));
    bool postings_removed = false;
    for (const auto& text : QUERIES) {
        const auto explained = ix.explain(text);
        REQUIRE(explained.size() == corpus.traces.size());
        for (const auto& f : explained) {
            CAPTURE(f.path);
            CAPTURE(text);
            CHECK(f.indexed);
            const auto view =
                view_chunks(dstd::String(f.path),
                            corpus.index_of(dstd::String(f.path)), text);
            CHECK(f.may_match == !view.empty());
            if (f.may_match) CHECK(f.read == view);
            for (const auto& e : f.extensions)
                if (e.name == "postings" && text == R"(name == "read")" &&
                    !e.removed.empty())
                    postings_removed = true;
        }
    }
    CHECK(postings_removed);
    CHECK_THROWS_AS(ix.explain("name =="), common::DFTUtilsException);
}

TEST_CASE("one extension is rebuilt or dropped without the others") {
    Corpus corpus;
    const auto trace = corpus.traces.front();
    const auto expected_rows = rows(corpus);
    auto ix = index::Indexer::open(dftu_utils_test::dstd_paths(corpus.traces),
                                   corpus_options(corpus));
    const auto before = manifest_of(ix, trace);
    for (const char* name : {"core.members", "core.rowset", "zonemap", "bloom",
                             "counts", "postings", "dft.stats"}) {
        CAPTURE(name);
        REQUIRE(before.count(name));
        CHECK(before.at(name).current);
    }

    ix.drop_extension("counts");
    auto dropped = manifest_of(ix, trace);
    CHECK_FALSE(dropped.count("counts"));
    CHECK(dropped.size() == before.size() - 1);
    CHECK(rows(corpus) == expected_rows);
    CHECK(ix.status().needs_work.size() == corpus.traces.size());
    CHECK(ix.build().indexed == corpus.traces.size());
    CHECK(manifest_of(ix, trace).count("counts"));
    CHECK(rows(corpus) == expected_rows);

    auto without = corpus_options(corpus);
    without.extensions = {"zonemap", "bloom", "postings"};
    auto subset = index::Indexer::open(
        dftu_utils_test::dstd_paths(corpus.traces), without);
    subset.drop_extension("counts");
    CHECK(subset.status().needs_work.empty());
    CHECK(subset.build().indexed == 0);
    subset.rebuild_extension("bloom");
    CHECK_FALSE(manifest_of(subset, trace).count("counts"));
    CHECK(rows(corpus) == expected_rows);

    auto other_rate = without;
    other_rate.bloom->false_positive_rate = 0.05;
    auto rate = index::Indexer::open(dftu_utils_test::dstd_paths(corpus.traces),
                                     other_rate);
    CHECK(rate.status().needs_work.size() == corpus.traces.size());
    rate.build();
    const auto after = manifest_of(rate, trace);
    CHECK(after.at("bloom").params_hash != before.at("bloom").params_hash);
    CHECK(after.at("bloom").current);
    for (const char* name : {"zonemap", "postings", "dft.stats"})
        CHECK(after.at(name).params_hash == before.at(name).params_hash);
    CHECK_FALSE(after.count("counts"));
    CHECK(rows(corpus) == expected_rows);

    for (const char* bad : {"core.members", "core.rowset", "nope"}) {
        CAPTURE(bad);
        try {
            ix.drop_extension(bad);
            FAIL("expected an error");
        } catch (const common::DFTUtilsException& e) {
            CHECK(e.error().is_code(common::ErrorCode::InvalidArgument));
        }
        CHECK_THROWS_AS(ix.rebuild_extension(bad), common::DFTUtilsException);
    }
    auto bad_options = corpus_options(corpus);
    bad_options.extensions = {"dft.stats"};
    CHECK_THROWS_AS(
        index::Indexer::open(dftu_utils_test::dstd_paths(corpus.traces),
                             bad_options),
        common::DFTUtilsException);
}

TEST_CASE("a spilled build indexes the corpus as an unbounded one") {
    Corpus corpus;
    auto spill = corpus_options(corpus);
    spill.index_dir = dstd::String(corpus.env.dir()) + "/idx_spill";
    spill.memory_budget = 64 * 1024;
    index::Indexer::open(dftu_utils_test::dstd_paths(corpus.traces), spill)
        .build();
    // The aggregation tier keys its rows by interned string ids that follow the
    // build's file order, and a memory budget changes that order. Its bytes may
    // differ, its answers may not.
    auto contents = [](const dstd::String& index_path) {
        dstd::Vector<dftu_utils_test::IndexRecord> out;
        for (auto& record : dftu_utils_test::dump_index(index_path))
            if (record.name != "dftu.idx.dftracer.agg")
                out.push_back(std::move(record));
        return out;
    };
    const auto& trace = corpus.traces.front();
    const auto spilled = dstd::String(
        trace::internal::determine_index_path(trace, spill.index_dir));
    const auto a = contents(corpus.index_of(trace));
    const auto b = contents(spilled);
    CHECK(a.size() == b.size());
    CHECK(a == b);

    const auto summary = [&](const dstd::String& index_path) {
        const auto df = dftu_utils_test::ok(
            trace::views::View::from_files(
                {{dstd::String(trace), dstd::String(index_path)}})
                .group_by({trace::views::GroupKey::cat(),
                           trace::views::GroupKey::name(),
                           trace::views::GroupKey::pid(),
                           trace::views::GroupKey::tid()})
                .agg({{trace::views::AggOp::Count, "", "n"},
                      {trace::views::AggOp::Sum, "dur", "dsum"},
                      {trace::views::AggOp::Min, "dur", "dmin"},
                      {trace::views::AggOp::Max, "dur", "dmax"},
                      {trace::views::AggOp::Min, "ts", "tmin"},
                      {trace::views::AggOp::Max, "ts", "tmax"}})
                .collect()
                .get());
        dstd::Map<dstd::String, dstd::Vector<double>> rows;
        for (std::int64_t r = 0; r < df.num_rows(); ++r) {
            dstd::String key;
            dstd::Vector<double> values;
            for (std::size_t i = 0; i < df.columns.size(); ++i) {
                const auto c = df.columns[i].materialize();
                double v = 0;
                switch (c.type()) {
                    case dataframe::TypeId::String:
                        key.append(c.string_at(r)).push_back('|');
                        continue;
                    case dataframe::TypeId::Int64:
                        v = static_cast<double>(c.data<std::int64_t>()[r]);
                        break;
                    case dataframe::TypeId::Uint64:
                        v = static_cast<double>(c.data<std::uint64_t>()[r]);
                        break;
                    default:
                        REQUIRE(c.type() == dataframe::TypeId::Float64);
                        v = c.data<double>()[r];
                }
                if (df.names[i] == "pid" || df.names[i] == "tid")
                    key.append(dstd::to_string(v)).push_back('|');
                else
                    values.push_back(v);
            }
            REQUIRE(rows.emplace(std::move(key), std::move(values)).second);
        }
        return rows;
    };
    for (const auto& t : corpus.traces) dstd::fs::remove(t).has_value();
    const auto from_full = summary(corpus.index_of(trace));
    CHECK_FALSE(from_full.empty());
    CHECK(from_full == summary(spilled));
}

TEST_CASE("a name two traces hash differently is an arrow to both hashes") {
    dftu_utils_test::TestEnvironment env{10};
    const auto index_dir = env.dir() + "/idx";
    dstd::Vector<dstd::String> traces;
    for (const char* hash : {"aaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbb"}) {
        dstd::String out;
        out += common::str_cat(
            R"({"name":"FH","ph":"M","pid":1,"tid":1,"args":{"name":"/x",)",
            R"("value":")", hash, "\"}}\n");
        for (int i = 0; i < 20; ++i)
            out += common::str_cat(
                R"({"id":)", i, R"(,"name":"read","cat":"POSIX","pid":1,)",
                R"("tid":1,"ph":"X","ts":)", 1000 + i,
                R"(,"dur":5,"args":{"fhash":")", hash, "\"}}\n");
        const auto plain =
            env.dir() + "/t" + dstd::to_string(traces.size()) + ".pfw";
        dstd::fs::write_file(dstd::PathView(plain), out).discard();
        const auto gz = plain + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                                   1024));
        dstd::fs::remove(plain).discard();
        traces.push_back(gz);
    }
    index::IndexerOptions opts;
    opts.index_dir = index_dir;
    index::Indexer::open(dftu_utils_test::dstd_paths(traces), opts).build();
    const auto index_path = dstd::String(
        trace::internal::determine_index_path(traces[0], index_dir));
    const auto rows = index::plan::stored_rowset(
        {{dstd::String(dstd::StringView(traces[0])),
          dstd::String(dstd::StringView(index_path))},
         {dstd::String(dstd::StringView(traces[1])),
          dstd::String(dstd::StringView(index_path))}},
        "files");
    REQUIRE(rows);
    CHECK(rows->num_rows() == 2);
    for (const auto& trace : traces) {
        CAPTURE(trace);
        const auto v = trace::views::View::from_file(dstd::String(trace),
                                                     dstd::String(index_path));
        const auto f = dftu_utils_test::ok(
            dftracer::utils::default_runtime()
                .submit(
                    v.duql(R"(where fhash -> files.path == "/x")").collect())
                .get());
        CHECK(f.num_rows() == 20);
    }
}
