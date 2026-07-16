#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/indexing/resolved_field_rewriter.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <doctest/doctest.h>
#include <simdjson.h>
#include <testing_utilities.h>

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

using dftracer::utils::utilities::common::query::Query;
using dftracer::utils::utilities::indexer::IndexDatabase;
namespace indexing = dftracer::utils::utilities::composites::dft::indexing;

namespace {

// Build an index DB seeded with a small FILE and HOST hash table.
IndexDatabase make_db(const std::string& root) {
    fs::create_directories(root);
    IndexDatabase db((fs::path(root) / ".dftindex").string());
    auto writer = db.begin_write();
    writer->init_schema();
    // FILE (type 0): hash -> path
    writer->insert_hash_table_entry(0, "fh_read", "/scratch/data/train.h5");
    writer->insert_hash_table_entry(0, "fh_write", "/scratch/data/out.h5");
    writer->insert_hash_table_entry(0, "fh_log", "/var/log/run.txt");
    // HOST (type 1): hash -> hostname
    writer->insert_hash_table_entry(1, "hh_a", "node01");
    writer->insert_hash_table_entry(1, "hh_b", "login02");
    writer->commit();
    return db;
}

// Parse a rewritten query and confirm it is an `<dim> in [...]` (or not in)
// over the expected set of hashes, order-independent.
void check_in_clause(const Query& q, const std::string& dim, bool negated,
                     std::vector<std::string> expected) {
    using namespace dftracer::utils::utilities::common::query;
    const auto& node = q.root();
    std::vector<std::string> got;
    if (!negated) {
        REQUIRE(std::holds_alternative<InNode>(node.data));
        const auto& in = std::get<InNode>(node.data);
        CHECK(in.field.path == dim);
        for (const auto& e : in.values.elements)
            got.push_back(std::get<std::string>(e.value));
    } else {
        REQUIRE(std::holds_alternative<NotInNode>(node.data));
        const auto& in = std::get<NotInNode>(node.data);
        CHECK(in.field.path == dim);
        for (const auto& e : in.values.elements)
            got.push_back(std::get<std::string>(e.value));
    }
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    CHECK(got == expected);
}

Query rewrite(const std::string& dsl, const IndexDatabase& db) {
    auto q = Query::from_string(dsl);
    REQUIRE(q.has_value());
    auto rw = indexing::rewrite_resolved_fields(*q, db);
    REQUIRE_MESSAGE(rw.has_value(), dsl);
    return std::move(*rw);
}

}  // namespace

TEST_SUITE("ResolvedFieldRewriter") {
    TEST_CASE("detects virtual fields") {
        auto a = Query::from_string(R"(resolved.fpath ~ "train")");
        auto b = Query::from_string(R"(r.hostname == "node01")");
        auto c = Query::from_string(R"(cat == "POSIX")");
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        REQUIRE(c.has_value());
        CHECK(indexing::has_resolved_fields(*a));
        CHECK(indexing::has_resolved_fields(*b));
        CHECK_FALSE(indexing::has_resolved_fields(*c));
    }

    TEST_CASE("no rewrite when no virtual fields") {
        auto root = dft_utils_test::make_unique_test_path("rfr_none");
        auto db = make_db(root.string());
        auto q = Query::from_string(R"(cat == "POSIX" and dur > 10)");
        REQUIRE(q.has_value());
        CHECK_FALSE(indexing::rewrite_resolved_fields(*q, db).has_value());
    }

    TEST_CASE("exact fpath == resolves to single fhash") {
        auto root = dft_utils_test::make_unique_test_path("rfr_eq");
        auto db = make_db(root.string());
        auto q = rewrite(R"(resolved.fpath == "/scratch/data/train.h5")", db);
        check_in_clause(q, "fhash", false, {"fh_read"});
    }

    TEST_CASE("fpath != resolves to fhash not in") {
        auto root = dft_utils_test::make_unique_test_path("rfr_ne");
        auto db = make_db(root.string());
        auto q = rewrite(R"(r.fpath != "/var/log/run.txt")", db);
        check_in_clause(q, "fhash", true, {"fh_log"});
    }

    TEST_CASE("regex over fpath collects all matching hashes") {
        auto root = dft_utils_test::make_unique_test_path("rfr_regex");
        auto db = make_db(root.string());
        auto q = rewrite(R"(resolved.fpath ~ "/scratch/.*\.h5")", db);
        check_in_clause(q, "fhash", false, {"fh_read", "fh_write"});
    }

    TEST_CASE("substring 'x' in resolved.fpath is case-insensitive") {
        auto root = dft_utils_test::make_unique_test_path("rfr_sub");
        auto db = make_db(root.string());
        auto q = rewrite(R"('TRAIN' in resolved.fpath)", db);
        check_in_clause(q, "fhash", false, {"fh_read"});
    }

    TEST_CASE("like over fpath") {
        auto root = dft_utils_test::make_unique_test_path("rfr_like");
        auto db = make_db(root.string());
        auto q = rewrite(R"(r.fpath like "/scratch/%")", db);
        check_in_clause(q, "fhash", false, {"fh_read", "fh_write"});
    }

    TEST_CASE("negated regex over fpath yields not in") {
        auto root = dft_utils_test::make_unique_test_path("rfr_nregex");
        auto db = make_db(root.string());
        auto q = rewrite(R"(resolved.fpath !~ "\.h5")", db);
        check_in_clause(q, "fhash", true, {"fh_read", "fh_write"});
    }

    TEST_CASE("hostname resolves against HOST table to hhash") {
        auto root = dft_utils_test::make_unique_test_path("rfr_host");
        auto db = make_db(root.string());
        auto q = rewrite(R"(r.hostname ilike "NODE%")", db);
        check_in_clause(q, "hhash", false, {"hh_a"});
    }

    TEST_CASE("no matches yields empty in-clause") {
        auto root = dft_utils_test::make_unique_test_path("rfr_empty");
        auto db = make_db(root.string());
        auto q = rewrite(R"(resolved.fpath ~ "nonexistent")", db);
        check_in_clause(q, "fhash", false, {});
    }

    TEST_CASE("rewrite + evaluate end-to-end against events") {
        using dftracer::utils::utilities::common::json::JsonValue;
        auto root = dft_utils_test::make_unique_test_path("rfr_e2e");
        auto db = make_db(root.string());
        // resolved.fpath ~ "/scratch" -> fhash in [fh_read, fh_write]
        auto q = rewrite(R"('scratch' in resolved.fpath)", db);

        simdjson::dom::parser p1;
        simdjson::dom::parser p2;
        std::string j_match =
            R"({"cat":"POSIX","args":{"fhash":"fh_read","ret":1}})";
        std::string j_other =
            R"({"cat":"POSIX","args":{"fhash":"fh_log","ret":1}})";
        auto ev_match = p1.parse(j_match.data(), j_match.size());
        auto ev_other = p2.parse(j_other.data(), j_other.size());
        REQUIRE_FALSE(ev_match.error());
        REQUIRE_FALSE(ev_other.error());
        // Bare "fhash" resolves the nested args value in the evaluator.
        CHECK(q.evaluate(JsonValue(ev_match.value_unsafe())));
        CHECK_FALSE(q.evaluate(JsonValue(ev_other.value_unsafe())));
    }

    TEST_CASE("virtual field composes with real predicates") {
        auto root = dft_utils_test::make_unique_test_path("rfr_compose");
        auto db = make_db(root.string());
        auto base = Query::from_string(
            R"(cat == "POSIX" and 'train' in resolved.fpath)");
        REQUIRE(base.has_value());
        auto rw = indexing::rewrite_resolved_fields(*base, db);
        REQUIRE(rw.has_value());
        // Top level stays an AND; the right branch became fhash in [...].
        using namespace dftracer::utils::utilities::common::query;
        const auto& node = rw->root();
        REQUIRE(std::holds_alternative<AndNode>(node.data));
        const auto& an = std::get<AndNode>(node.data);
        CHECK(std::holds_alternative<CompareNode>(an.left->data));
        REQUIRE(std::holds_alternative<InNode>(an.right->data));
        CHECK(std::get<InNode>(an.right->data).field.path == "fhash");
    }
}
