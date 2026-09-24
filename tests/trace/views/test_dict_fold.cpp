#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/trace/views/dict_fold.h>
#include <dftracer/utils/trace/views/fold.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <array>
#include <string>
#include <vector>

#include "test_view_common.h"

using namespace dftracer::utils::trace::views::detail;
using dftracer::utils::StringIntern;

namespace {

std::string padded(std::string s) {
    s.resize(s.size() + simdjson::SIMDJSON_PADDING, '\0');
    return s;
}

// FH + HH + SH definitions plus a data event the fold must ignore.
const std::vector<std::string>& metadata_lines() {
    static const std::vector<std::string> lines = {
        R"({"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"/data/a.bin","value":"fh1"}})",
        R"({"name":"HH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"node0","value":"hh1"}})",
        R"({"name":"SH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"s","value":"sh1"}})",
        R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1,"dur":2,"args":{}})",
    };
    return lines;
}

std::vector<FoldEvent> fold_events_of(const std::vector<std::string>& lines,
                                      StringIntern& intern) {
    std::vector<FoldEvent> out;
    out.reserve(lines.size());
    simdjson::dom::parser parser;
    for (const auto& l : lines) {
        std::string buf = padded(l);
        auto doc = parser.parse(buf.data(), l.size(), false);
        REQUIRE_FALSE(doc.error());
        out.push_back(extract_fold_event(doc.value_unsafe(), intern, true));
    }
    return out;
}

// A trace carrying FH/HH metadata for the fuse harvest path.
std::string create_metadata_trace(TestEnvironment& env, int n) {
    std::string pfw = env.get_dir() + "/dictmeta.pfw";
    {
        std::ofstream ofs(pfw);
        ofs << R"({"name":"HH","cat":"dftracer","pid":1,"tid":1,"ph":"M",)"
            << R"("args":{"name":"node0","value":"hh1"}})" << "\n";
        ofs << R"({"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":"M",)"
            << R"("args":{"name":"/data/a.bin","value":"fh1"}})" << "\n";
        for (int i = 0; i < n; ++i)
            ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
                << (1000 + i * 10) << R"(,"dur":2,"args":{"fhash":"fh1"}})"
                << "\n";
    }
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    return gz;
}

}  // namespace

TEST_SUITE("DictFold") {
    // The fold harvests from owned FoldEvents and drives HashTableDictionary's
    // commit. create_mixed_trace emits no metadata, so its index has no hash
    // tables for these entries and the commit is a genuine write.
    TEST_CASE(
        "harvests from FoldEvents and commits under whole-file coverage") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_mixed_trace(env, 4, 0);
        std::string idx = determine_index_path(gz, "");
        {
            StringSink s;
            View::from_file(gz, idx).sink_json(s).get();
        }

        ScanUnit unit;
        unit.file_path = gz;
        unit.index_path = idx;

        StringIntern intern;
        auto events = fold_events_of(metadata_lines(), intern);
        DictFold fold(intern);
        FoldBatch batch{std::span<const FoldEvent>(events), unit};
        fold.step(batch);
        CHECK(fold.entry_count() == 0);  // nothing published before sealing
        fold.seal_unit(unit);
        CHECK(fold.entry_count() == 3);  // FH + HH + SH, data event ignored

        CoverageSet member_only;
        member_only.add(gz, 0);          // a member, not the whole file
        CHECK_FALSE(fold.finalize(member_only).get());

        CoverageSet whole_file;
        whole_file.add_file(gz);
        CHECK(fold.finalize(whole_file).get());
    }

    TEST_CASE("a fold over budget abandons instead of growing") {
        ScanUnit unit;
        unit.file_path = "a.pfw.gz";
        StringIntern intern;
        auto events = fold_events_of(metadata_lines(), intern);
        DictFold fold(intern, /*budget_bytes=*/1);
        FoldBatch batch{std::span<const FoldEvent>(events), unit};
        fold.step(batch);
        fold.seal_unit(unit);

        CHECK(fold.abandoned());
        CHECK(fold.entry_count() == 0);
    }

    // End to end: fuse delivers metadata events and the fold harvests the
    // dictionary from the one pass the query runs, without re-parsing.
    TEST_CASE("fuse drives the fold to harvest the dictionary") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_metadata_trace(env, 6);
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
        vdef.include_metadata = true;
        vdef.emit_all_metadata = true;

        StringIntern intern;
        DictFold fold(intern);
        std::array<Fold*, 1> folds{&fold};
        fuse(plan, vdef, folds, intern).get();

        CHECK(fold.entry_count() == 2);  // FH + HH
    }
}
