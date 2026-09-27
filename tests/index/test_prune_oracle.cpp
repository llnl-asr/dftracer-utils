// Prune oracle: file verdicts and chunk sets of every pruning consumer over a
// fixed, generated corpus and query list, compared with a recording. The View
// consumer is `gather_units`, the path every View scan plans through. Dynamic
// narrowing is not recorded: it races the running scan, and it prunes through
// ChunkPruner, which is. Set
// DFTRACER_PRUNE_ORACLE_RECORD=<path> to write a new recording instead.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/index/plan/rowsets.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <rocksdb/iterator.h>
#include <testing_utilities.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace dftracer::utils;
namespace plan = dftracer::utils::index::plan;
namespace store = dftracer::utils::index::store;

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

// Deterministic linear congruential generator; the corpus must not change
// between the recording and the check.
struct Lcg {
    std::uint64_t s;
    std::uint64_t next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
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
std::string write_trace(const std::string& dir, int seed, int events) {
    Lcg rng{static_cast<std::uint64_t>(seed) * 7919};
    std::ostringstream out;
    out << "[\n";
    for (int f = 0; f < 3; ++f)
        out << R"({"name":"FH","ph":"M","pid":1,"tid":1,"args":{"name":")"
            << FILES[f] << R"(","value":")" << FHASH[f] << "\"}}\n";
    for (int h = 0; h < 2; ++h)
        out << R"({"name":"HH","ph":"M","pid":1,"tid":1,"args":{"name":")"
            << HOSTS[h] << R"(","value":")" << HHASH[h] << "\"}}\n";
    for (int x = 0; x < 2; ++x)
        out << R"({"name":"SH","ph":"M","pid":1,"tid":1,"args":{"name":")"
            << EXECS[x] << R"(","value":")" << SHASH[x] << "\"}}\n";
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
                out << R"({"name":"thread_name","ph":"M","pid":1,"tid":)"
                    << 1000 + t << R"(,"args":{"name":"worker-)" << t
                    << "\"}}\n";
        ts += 5 + rng.next() % 50;
        if (i == events / 2) ts += 400000;
        const auto dur = 10 + (rng.next() % 1000);
        const auto size = (rng.next() % 64) * 128;
        std::string op = NAMES[name];
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
        out << R"({"id":)" << i << R"(,"name":")" << op << R"(","cat":")"
            << CATS[cat] << R"(","pid":)" << pid << R"(,"tid":)" << pid * 10
            << R"(,"ph":"X","ts":)" << ts << R"(,"dur":)" << dur
            << R"(,"args":{"size":)" << size << R"(,"fhash":")" << FHASH[file]
            << R"(","hhash":")" << HHASH[host] << R"(","shash":")"
            << SHASH[pid % 2] << "\"}}\n";
    }
    out << "]\n";
    const auto plain = dir + "/trace" + std::to_string(seed) + ".pfw";
    std::ofstream(plain) << out.str();
    const auto gz = plain + ".gz";
    const std::size_t member = seed == 3 ? 256 * 1024 : 32 * 1024;
    REQUIRE(
        dftu_utils_test::compress_file_to_gzip_multimember(plain, gz, member));
    fs::remove(plain);
    return gz;
}

const std::vector<std::string> QUERIES = {
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

using TimeRange = std::optional<std::pair<double, double>>;
const std::vector<TimeRange> TIME_RANGES = {
    std::nullopt,
    std::make_pair(1050000.0, 1150000.0),
    std::make_pair(0.0, 1020000.0),
    std::make_pair(1300000.0, 0.0),
};

std::string join(const std::vector<std::uint64_t>& v) {
    std::string s;
    for (auto x : v) {
        if (!s.empty()) s += ',';
        s += std::to_string(x);
    }
    return s;
}

std::string range_str(const TimeRange& r) {
    if (!r) return "-";
    return std::to_string(static_cast<long long>(r->first)) + ".." +
           std::to_string(static_cast<long long>(r->second));
}

struct Corpus {
    dftu_utils_test::TestEnvironment env{10};
    std::string index_dir;
    std::vector<std::string> traces;

    Corpus() {
        index_dir = env.get_dir() + "/idx";
        for (int seed : {1, 2, 3})
            traces.push_back(write_trace(env.get_dir(), seed, 6000));
        index::IndexerOptions opts;
        opts.index_dir = index_dir;
        index::Indexer::open(traces, opts).build();
    }
    std::string index_of(const std::string& trace) const {
        return trace::internal::determine_index_path(trace, index_dir);
    }
};

std::vector<std::string> run_oracle(const Corpus& c) {
    std::vector<std::string> lines;
    for (const auto& trace : c.traces) {
        const auto base = fs::path(trace).filename().string();
        const auto index_path = c.index_of(trace);
        store::IndexDatabase db(index_path, store::IndexOpenMode::ReadOnly);
        const int fid =
            db.get_file_info_id(store::internal::get_logical_path(trace));
        REQUIRE(fid >= 0);
        const auto members = db.query_gzip_members(fid).size();
        const auto uc = db.query_file_metadata_batch({fid})[fid].max_bytes;

        for (const auto& text : QUERIES) {
            const auto q = duql::parse_or_throw(text);
            const std::string key = base + "|" + text + "|";

            plan::ChunkPruner pruner;
            auto one =
                pruner(plan::ChunkPrunerInput{index_path, trace, q}).get();
            lines.push_back(key + "-|pruner|" + std::to_string(one.success) +
                            "/" + std::to_string(one.file_may_match) + "/" +
                            std::to_string(one.total_checkpoints) + "|" +
                            join(one.candidate_checkpoints));

            plan::ChunkPrunerBatchInput batch;
            batch.index_path = index_path;
            batch.items.push_back({trace, q});
            auto many = pruner.process_batch(batch);
            REQUIRE(many);
            const auto& b = many->outputs.at(0);
            lines.push_back(key + "-|batch|" + std::to_string(b.success) + "/" +
                            std::to_string(b.file_may_match) + "/" +
                            std::to_string(b.total_checkpoints) + "|" +
                            join(b.candidate_checkpoints));

            for (const auto& tr : TIME_RANGES) {
                trace::views::detail::ViewPlan vp;
                vp.files.push_back({trace, index_path, uc, members, 0});
                vp.query = q;
                vp.time_range = tr;
                const auto vdef = trace::views::detail::make_vdef(vp, false);
                std::uint64_t skipped = 0;
                auto units =
                    trace::views::detail::gather_units(vp, vdef, skipped).get();
                std::vector<std::uint64_t> ckpts;
                for (const auto& u : units) ckpts.push_back(u.checkpoint_idx);
                lines.push_back(key + range_str(tr) + "|view|" +
                                std::to_string(!units.empty()) + "/" +
                                std::to_string(skipped) + "|" + join(ckpts));
            }
        }
    }
    return lines;
}

std::string recording_path() {
    return (fs::path(DFTRACER_UTILS_TEST_SOURCE_DIR) / "index" / "data" /
            "prune_oracle.txt")
        .string();
}

std::size_t count_json(const std::string& trace, const std::string& index_dir,
                       const std::string& text, bool skip_pruning) {
    auto run = [&]() -> coro::CoroTask<std::size_t> {
        utilities::reader::TraceReader reader(
            {.file_path = trace, .index_dir = index_dir});
        utilities::reader::ReadConfig cfg;
        cfg.query = text;
        cfg.skip_pruning = skip_pruning;
        auto gen = reader.read_json(cfg);
        std::size_t n = 0;
        while (auto line = co_await gen.next()) ++n;
        co_return n;
    };
    return run().get();
}

std::uint64_t count_view(const std::string& trace,
                         const std::string& index_path,
                         const std::string& text) {
    trace::views::detail::ViewPlan vp;
    vp.files.push_back({trace, index_path, 0, 0, 0});
    vp.query = duql::parse_or_throw(text);
    const auto vdef = trace::views::detail::make_vdef(vp, false);
    return trace::views::detail::for_each_scanned_batch(
               vp, vdef, 1, 0,
               [](std::size_t, const std::vector<std::string_view>&) {})
        .get()
        .events_matched;
}

// The View scan without pruning: the whole file as one unit.
std::uint64_t count_view_unpruned(const std::string& trace,
                                  const std::string& index_path,
                                  const std::string& text) {
    trace::views::detail::ViewPlan vp;
    vp.query = duql::parse_or_throw(text);
    const auto vdef = trace::views::detail::make_vdef(vp, false);
    store::IndexDatabase db(index_path, store::IndexOpenMode::ReadOnly);
    const auto spans = db.query_chunk_spans(
        db.get_file_info_id(store::internal::get_logical_path(trace)));
    REQUIRE_FALSE(spans.empty());
    std::shared_ptr<const duql::Query> q;
    const trace::views::detail::ScanUnit u{
        trace, index_path, trace::views::detail::checkpoint_size_or_default(0),
        0,     0,          spans.back().uc_offset + spans.back().uc_size,
        0,     q};
    auto run = [&]() -> coro::CoroTask<std::uint64_t> {
        const auto sin =
            trace::views::detail::make_scanner_input(u, vdef, vdef.query);
        trace::views::ViewScannerUtility scanner;
        auto gen = scanner(sin);
        std::uint64_t n = 0;
        while (auto b = co_await gen.next()) n += b->events_matched;
        co_return n;
    };
    return run().get();
}

}  // namespace

TEST_CASE("prune decisions match the recording") {
    Corpus corpus;
    const auto lines = run_oracle(corpus);

    if (const char* out = std::getenv("DFTRACER_PRUNE_ORACLE_RECORD")) {
        std::ofstream f(out);
        for (const auto& l : lines) f << l << "\n";
        MESSAGE("recorded " << lines.size() << " lines to "
                            << std::string(out));
        return;
    }

    std::ifstream f(recording_path());
    REQUIRE_MESSAGE(f.good(), "missing " << recording_path());
    std::vector<std::string> expected;
    for (std::string l; std::getline(f, l);) expected.push_back(l);
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

std::vector<std::uint64_t> view_chunks(const std::string& trace,
                                       const std::string& index_path,
                                       const std::string& text) {
    store::IndexDatabase db(index_path, store::IndexOpenMode::ReadOnly);
    const int fid =
        db.get_file_info_id(store::internal::get_logical_path(trace));
    trace::views::detail::ViewPlan vp;
    vp.files.push_back({trace, index_path,
                        db.query_file_metadata_batch({fid})[fid].max_bytes,
                        db.query_gzip_members(fid).size(), 0});
    vp.query = duql::parse_or_throw(text);
    const auto vdef = trace::views::detail::make_vdef(vp, false);
    std::uint64_t skipped = 0;
    std::vector<std::uint64_t> out;
    for (const auto& u :
         trace::views::detail::gather_units(vp, vdef, skipped).get())
        out.push_back(u.checkpoint_idx);
    return out;
}

using Manifest = std::map<std::string, index::ExtensionEntry>;

Manifest manifest_of(const index::Indexer& ix, const std::string& trace) {
    Manifest out;
    for (const auto& f : ix.manifest())
        if (f.path == trace)
            for (const auto& e : f.extensions) out[e.name] = e;
    return out;
}

// One query per pruning kind: postings, counts, zonemap, bloom, time.
const std::vector<std::string> ROW_QUERIES = {
    R"(name == "read")", R"(cat == "COMPUTE")", R"(args.size > 8000)",
    R"(name == "target_op")", R"(ts >= 1200000)"};

std::vector<std::uint64_t> rows(const Corpus& c) {
    std::vector<std::uint64_t> out;
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
    auto ix = index::Indexer::open(corpus.traces, corpus_options(corpus));
    bool postings_removed = false;
    for (const auto& text : QUERIES) {
        const auto explained = ix.explain(text);
        REQUIRE(explained.size() == corpus.traces.size());
        for (const auto& f : explained) {
            CAPTURE(f.path);
            CAPTURE(text);
            CHECK(f.indexed);
            const auto view =
                view_chunks(f.path, corpus.index_of(f.path), text);
            CHECK(f.may_match == !view.empty());
            if (f.may_match) CHECK(f.read == view);
            for (const auto& e : f.extensions)
                if (e.name == "postings" && text == R"(name == "read")" &&
                    !e.removed.empty())
                    postings_removed = true;
        }
    }
    CHECK(postings_removed);
    CHECK_THROWS_AS(ix.explain("name =="), DFTUtilsException);
}

TEST_CASE("one extension is rebuilt or dropped without the others") {
    Corpus corpus;
    const auto trace = corpus.traces.front();
    const auto expected_rows = rows(corpus);
    auto ix = index::Indexer::open(corpus.traces, corpus_options(corpus));
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
    auto subset = index::Indexer::open(corpus.traces, without);
    subset.drop_extension("counts");
    CHECK(subset.status().needs_work.empty());
    CHECK(subset.build().indexed == 0);
    subset.rebuild_extension("bloom");
    CHECK_FALSE(manifest_of(subset, trace).count("counts"));
    CHECK(rows(corpus) == expected_rows);

    auto other_rate = without;
    other_rate.bloom->false_positive_rate = 0.05;
    auto rate = index::Indexer::open(corpus.traces, other_rate);
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
        } catch (const DFTUtilsException& e) {
            CHECK(e.code() == ErrorCode::INVALID_ARGUMENT);
        }
        CHECK_THROWS_AS(ix.rebuild_extension(bad), DFTUtilsException);
    }
    auto bad_options = corpus_options(corpus);
    bad_options.extensions = {"dft.stats"};
    CHECK_THROWS_AS(index::Indexer::open(corpus.traces, bad_options),
                    DFTUtilsException);
}

TEST_CASE("a spilled build indexes the corpus as an unbounded one") {
    Corpus corpus;
    auto spill = corpus_options(corpus);
    spill.index_dir = corpus.env.get_dir() + "/idx_spill";
    spill.memory_budget = 64 * 1024;
    index::Indexer::open(corpus.traces, spill).build();
    auto contents = [](const std::string& index_path) {
        store::RocksDBManager::instance().reset(index_path);
        store::IndexDatabase db(index_path, store::IndexOpenMode::ReadOnly);
        std::vector<std::pair<std::string, std::string>> out;
        for (std::size_t f = 0; f < store::layout::FAMILY_COUNT; ++f) {
            const auto name = store::layout::family_name(
                static_cast<store::layout::Family>(f));
            auto it = db.db()->new_iterator(name);
            if (!it) continue;
            for (it->SeekToFirst(); it->Valid(); it->Next())
                out.emplace_back(std::string(name) + "|" + it->key().ToString(),
                                 it->value().ToString());
        }
        return out;
    };
    const auto& trace = corpus.traces.front();
    const auto a = contents(corpus.index_of(trace));
    const auto b =
        contents(trace::internal::determine_index_path(trace, spill.index_dir));
    CHECK(a.size() == b.size());
    CHECK(a == b);
}

TEST_CASE("a name two traces hash differently is an arrow to both hashes") {
    dftu_utils_test::TestEnvironment env{10};
    const auto index_dir = env.get_dir() + "/idx";
    std::vector<std::string> traces;
    for (const char* hash : {"aaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbb"}) {
        std::ostringstream out;
        out << R"({"name":"FH","ph":"M","pid":1,"tid":1,"args":{"name":"/x",)"
            << R"("value":")" << hash << "\"}}\n";
        for (int i = 0; i < 20; ++i)
            out << R"({"id":)" << i
                << R"(,"name":"read","cat":"POSIX","pid":1,)"
                << R"("tid":1,"ph":"X","ts":)" << 1000 + i
                << R"(,"dur":5,"args":{"fhash":")" << hash << "\"}}\n";
        const auto plain =
            env.get_dir() + "/t" + std::to_string(traces.size()) + ".pfw";
        std::ofstream(plain) << out.str();
        const auto gz = plain + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                                   1024));
        fs::remove(plain);
        traces.push_back(gz);
    }
    index::IndexerOptions opts;
    opts.index_dir = index_dir;
    index::Indexer::open(traces, opts).build();
    const auto index_path =
        trace::internal::determine_index_path(traces[0], index_dir);
    const auto rows = index::plan::stored_rowset(
        {{traces[0], index_path}, {traces[1], index_path}}, "files");
    REQUIRE(rows);
    CHECK(rows->num_rows() == 2);
    for (const auto& trace : traces) {
        CAPTURE(trace);
        const auto v = trace::views::View::from_file(trace, index_path);
        const auto f =
            dftracer::utils::default_runtime()
                .submit(
                    v.duql(R"(where fhash -> files.path == "/x")").collect())
                .get();
        CHECK(f.num_rows() == 20);
    }
}
