#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/trace/views/fold.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <array>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "test_view_common.h"

using namespace dftracer::utils::trace::views::detail;
using dftracer::utils::StringIntern;

namespace {

std::string padded(std::string s) {
    s.resize(s.size() + simdjson::SIMDJSON_PADDING, '\0');
    return s;
}

// A minimal fold: counts events and sums ts, to validate the traversal
// (slice/step/merge, coverage, fan-out) independent of any artifact logic.
struct CountingFold : Fold {
    std::uint64_t count = 0;
    std::uint64_t ts_sum = 0;
    std::uint64_t sealed_units = 0;

    bool accepts(const ScanShape&) const override { return true; }
    std::unique_ptr<Fold> slice() const override {
        return std::make_unique<CountingFold>();
    }
    void step(const FoldBatch& b) override {
        for (const auto& e : b.events) {
            ++count;
            ts_sum += e.ts;
        }
    }
    void seal_unit(const ScanUnit&) override { ++sealed_units; }
    void drop_unit(const ScanUnit&) override {}
    void merge(Fold& other) override {
        auto& o = static_cast<CountingFold&>(other);
        count += o.count;
        ts_sum += o.ts_sum;
        sealed_units += o.sealed_units;
    }
    coro::CoroTask<bool> finalize(const CoverageSet&) override {
        co_return true;
    }
};

}  // namespace

TEST_SUITE("Fold") {
    // The reason FoldEvent exists: a batch of them must survive the parser that
    // produced them. Extract from A, then reuse the SAME parser on B (which
    // invalidates A's buffer), and A must still resolve correctly.
    TEST_CASE("an extracted event outlives the parser buffer") {
        StringIntern intern;
        simdjson::dom::parser parser;

        std::string a = padded(
            R"({"ph":"X","name":"read","cat":"POSIX","pid":7,"tid":9,"ts":100,"dur":5,"args":{"fhash":"fhA"}})");
        std::size_t a_len = a.size() - simdjson::SIMDJSON_PADDING;
        auto ra = parser.parse(a.data(), a_len, false);
        REQUIRE_FALSE(ra.error());
        FoldEvent ea = extract_fold_event(ra.value_unsafe(), intern, false);

        // Reuse the parser on a different document: this is what invalidates a
        // string_view held into the parser buffer.
        std::string b = padded(
            R"({"ph":"X","name":"write","cat":"STDIO","pid":1,"tid":1,"ts":200,"dur":3,"args":{"fhash":"fhB"}})");
        auto rb = parser.parse(b.data(), b.size() - simdjson::SIMDJSON_PADDING,
                               false);
        REQUIRE_FALSE(rb.error());
        FoldEvent eb = extract_fold_event(rb.value_unsafe(), intern, false);

        // ea must be unchanged despite the parser having moved on.
        CHECK(ea.pid == 7);
        CHECK(ea.tid == 9);
        CHECK(ea.ts == 100);
        CHECK(ea.dur == 5);
        CHECK(ea.has_dur);
        CHECK(intern.resolve(ea.name_id) == "read");
        CHECK(intern.resolve(ea.cat_id) == "POSIX");
        CHECK(intern.resolve(ea.fhash_id) == "fhA");

        CHECK(intern.resolve(eb.name_id) == "write");
        CHECK(intern.resolve(eb.fhash_id) == "fhB");
        // Distinct strings interned to distinct ids.
        CHECK(ea.name_id != eb.name_id);
    }

    TEST_CASE("args are captured only when asked, fhash always") {
        StringIntern intern;
        simdjson::dom::parser parser;
        std::string line = padded(
            R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1,"dur":1,"args":{"fhash":"fh1","size":4096,"mode":"r"}})");
        auto r = parser.parse(line.data(),
                              line.size() - simdjson::SIMDJSON_PADDING, false);
        REQUIRE_FALSE(r.error());

        SUBCASE("needs_args=false drops args but keeps fhash") {
            FoldEvent e = extract_fold_event(r.value_unsafe(), intern, false);
            CHECK(e.args.empty());
            CHECK(intern.resolve(e.fhash_id) == "fh1");
        }

        SUBCASE("needs_args=true captures numeric and string args") {
            FoldEvent e = extract_fold_event(r.value_unsafe(), intern, true);
            CHECK(e.args.size() == 2);  // size + mode, not fhash
            bool saw_size = false, saw_mode = false;
            const auto size_id = intern.get_or_insert("size");
            const auto mode_id = intern.get_or_insert("mode");
            for (const auto& [k, v] : e.args) {
                if (k == size_id) {
                    saw_size = true;
                    CHECK(std::get<std::int64_t>(v) == 4096);
                } else if (k == mode_id) {
                    saw_mode = true;
                    CHECK(intern.resolve(std::get<std::uint32_t>(v)) == "r");
                }
            }
            CHECK(saw_size);
            CHECK(saw_mode);
        }
    }

    // Integer args are kept as int64, not double, so a value above 2^53
    // survives exactly; a fractional value stays a double.
    TEST_CASE("integer args wider than 2^53 keep exact precision") {
        StringIntern intern;
        simdjson::dom::parser parser;
        // 2^53 + 1 is the smallest positive integer a double cannot represent.
        std::string line = padded(
            R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1,"dur":1,"args":{"offset":9007199254740993,"ratio":0.5}})");
        auto r = parser.parse(line.data(),
                              line.size() - simdjson::SIMDJSON_PADDING, false);
        REQUIRE_FALSE(r.error());

        FoldEvent e = extract_fold_event(r.value_unsafe(), intern, true);
        const auto offset_id = intern.get_or_insert("offset");
        const auto ratio_id = intern.get_or_insert("ratio");
        bool saw_offset = false, saw_ratio = false;
        for (const auto& [k, v] : e.args) {
            if (k == offset_id) {
                saw_offset = true;
                CHECK(std::get<std::int64_t>(v) == 9007199254740993LL);
            } else if (k == ratio_id) {
                saw_ratio = true;
                CHECK(std::get<double>(v) == doctest::Approx(0.5));
            }
        }
        CHECK(saw_offset);
        CHECK(saw_ratio);
    }

    // The traversal drives a fold over a real trace: every event reaches the
    // fold exactly once, and per-worker slices merge into one count.
    TEST_CASE("fuse drives a fold over every event exactly once") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_mixed_trace(env, 30, 20);  // 50 events
        std::string idx = determine_index_path(gz, "");
        {
            StringSink s;
            View::from_file(gz, idx).sink_json(s).get();
        }

        ViewPlan plan;
        ViewFile f;
        f.file_path = gz;
        f.index_path = idx;
        plan.files.push_back(f);
        ViewDefinition vdef;
        vdef.include_metadata = false;

        StringIntern intern;
        CountingFold cf;
        std::array<Fold*, 1> folds{&cf};
        auto stats = fuse(plan, vdef, folds, intern).get();

        CHECK(cf.count == 50);
        CHECK(stats.events_matched == 50);
        CHECK(cf.sealed_units >= 1);
    }

    // Step 2 of lazy indexing: the scanner in fold mode must deliver metadata
    // events (ph="M") to the fold under emit_all_metadata, so a dictionary fold
    // can harvest the hash table without re-parsing.
    TEST_CASE(
        "fuse delivers metadata events to a fold under emit_all_metadata") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        // One FH and one HH metadata record, then data events.
        std::string pfw = env.get_dir() + "/meta.pfw";
        {
            std::ofstream ofs(pfw);
            ofs << R"({"name":"HH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"node0","value":"hh1"}})"
                << "\n";
            ofs << R"({"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"/data/a.bin","value":"fh1"}})"
                << "\n";
            for (int i = 0; i < 5; ++i)
                ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
                    << (100 + i) << R"(,"dur":2,"args":{"fhash":"fh1"}})"
                    << "\n";
        }
        std::string gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        fs::remove(pfw);
        std::string idx = determine_index_path(gz, "");
        {
            StringSink s;
            View::from_file(gz, idx).sink_json(s).get();
        }

        // A minimal dictionary fold: collects (type -> {name -> value}) from
        // metadata FoldEvents.
        struct DictProbe : Fold {
            const StringIntern* in;
            std::map<std::string, std::pair<std::string, std::string>> got;
            explicit DictProbe(const StringIntern& i) : in(&i) {}
            bool accepts(const ScanShape&) const override { return true; }
            bool needs_args() const override { return true; }
            std::unique_ptr<Fold> slice() const override {
                return std::make_unique<DictProbe>(*in);
            }
            void step(const FoldBatch& b) override {
                for (const auto& e : b.events) {
                    if (e.phase !=
                        dftracer::utils::trace::RecordPhase::METADATA)
                        continue;
                    std::string type(in->resolve(e.name_id));
                    std::string name, value;
                    for (const auto& [k, v] : e.args) {
                        auto key = in->resolve(k);
                        if (const auto* id = std::get_if<std::uint32_t>(&v)) {
                            if (key == "name")
                                name = std::string(in->resolve(*id));
                            else if (key == "value")
                                value = std::string(in->resolve(*id));
                        }
                    }
                    if (!name.empty()) got[type] = {name, value};
                }
            }
            void seal_unit(const ScanUnit&) override {}
            void drop_unit(const ScanUnit&) override {}
            void merge(Fold& o) override {
                for (auto& [t, nv] : static_cast<DictProbe&>(o).got)
                    got.insert({t, nv});
            }
            coro::CoroTask<bool> finalize(const CoverageSet&) override {
                co_return true;
            }
        };

        ViewPlan plan;
        ViewFile f;
        f.file_path = gz;
        f.index_path = idx;
        plan.files.push_back(f);
        ViewDefinition vdef;
        vdef.include_metadata = true;
        vdef.emit_all_metadata = true;

        StringIntern intern;
        DictProbe probe(intern);
        std::array<Fold*, 1> folds{&probe};
        fuse(plan, vdef, folds, intern).get();

        REQUIRE(probe.got.count("FH") == 1);
        CHECK(probe.got["FH"].first == "/data/a.bin");
        CHECK(probe.got["FH"].second == "fh1");
        REQUIRE(probe.got.count("HH") == 1);
        CHECK(probe.got["HH"].first == "node0");
        CHECK(probe.got["HH"].second == "hh1");
    }

    // The dictionary fold reads metadata (FH/HH/SH/PR) from the same POD: the
    // record type is name_id, and args carries the registered name and its hash
    // value. Confirm they survive extraction so the fold need not re-parse.
    TEST_CASE("a metadata event's type, name and value are captured") {
        StringIntern intern;
        simdjson::dom::parser parser;
        std::string line = padded(
            R"({"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"hhash":"hh9","name":"/data/x.bin","value":"fh7"}})");
        auto r = parser.parse(line.data(),
                              line.size() - simdjson::SIMDJSON_PADDING, false);
        REQUIRE_FALSE(r.error());
        FoldEvent e = extract_fold_event(r.value_unsafe(), intern, true);

        CHECK(e.phase == dftracer::utils::trace::RecordPhase::METADATA);
        CHECK(intern.resolve(e.name_id) == "FH");
        CHECK(intern.resolve(e.hhash_id) == "hh9");  // hhash always harvested

        const auto name_id = intern.get_or_insert("name");
        const auto value_id = intern.get_or_insert("value");
        bool saw_name = false, saw_value = false;
        for (const auto& [k, v] : e.args) {
            if (k == name_id) {
                saw_name = true;
                CHECK(intern.resolve(std::get<std::uint32_t>(v)) ==
                      "/data/x.bin");
            } else if (k == value_id) {
                saw_value = true;
                CHECK(intern.resolve(std::get<std::uint32_t>(v)) == "fh7");
            }
        }
        CHECK(saw_name);
        CHECK(saw_value);
    }

    TEST_CASE("absent fields stay at their sentinels") {
        StringIntern intern;
        simdjson::dom::parser parser;
        // No cat, no args.
        std::string line =
            padded(R"({"ph":"X","name":"n","pid":1,"tid":1,"ts":1})");
        auto r = parser.parse(line.data(),
                              line.size() - simdjson::SIMDJSON_PADDING, false);
        REQUIRE_FALSE(r.error());
        FoldEvent e = extract_fold_event(r.value_unsafe(), intern, true);

        CHECK(e.cat_id == StringIntern::NO_ID);
        CHECK(e.fhash_id == StringIntern::NO_ID);
        CHECK(e.hhash_id == StringIntern::NO_ID);
        CHECK_FALSE(e.has_dur);
        CHECK(e.args.empty());
    }
}
