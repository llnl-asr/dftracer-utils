#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/index/plan/resolved_field_rewriter.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/query/query.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <simdjson.h>
#include <testing_utilities.h>

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

using dftracer::utils::DFTUtilsException;
using dftracer::utils::ErrorCode;
using dftracer::utils::index::get_schema;
using dftracer::utils::index::store::IndexDatabase;
using dftracer::utils::query::Query;

namespace {

// Build an index DB seeded with a small FILE and HOST dictionary.
IndexDatabase make_db(const std::string& root) {
    fs::create_directories(root);
    IndexDatabase db((fs::path(root) / ".dftindex").string());
    auto writer = db.begin_write();
    writer->init_schema();
    dftu_utils_test::index_records::put_dict_row(
        *writer, "file", "fh_read", {{"path", "/scratch/data/train.h5"}});
    dftu_utils_test::index_records::put_dict_row(
        *writer, "file", "fh_write", {{"path", "/scratch/data/out.h5"}});
    dftu_utils_test::index_records::put_dict_row(
        *writer, "file", "fh_log", {{"path", "/var/log/run.txt"}});
    dftu_utils_test::index_records::put_dict_row(*writer, "host", "hh_a",
                                                 {{"name", "node01"}});
    dftu_utils_test::index_records::put_dict_row(*writer, "host", "hh_b",
                                                 {{"name", "login02"}});
    writer->commit();
    return db;
}

// Parse a rewritten query and confirm it is a `<dim> in [...]` over the
// expected set of keys, order-independent.
void check_in_clause(const Query& q, const std::string& dim,
                     std::vector<std::string> expected) {
    using namespace dftracer::utils::query;
    const auto& node = q.root();
    REQUIRE(std::holds_alternative<InNode>(node.data));
    const auto& in = std::get<InNode>(node.data);
    CHECK(in.field.path == dim);
    std::vector<std::string> got;
    for (const auto& e : in.values.elements)
        got.push_back(std::get<std::string>(e.value));
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    CHECK(got == expected);
}

Query rewrite(const std::string& dsl, const IndexDatabase& db) {
    auto q = Query::from_string(dsl);
    REQUIRE(q.has_value());
    auto rw = dftracer::utils::index::plan::rewrite_resolved_fields(
        *q, db, get_schema("dftracer"));
    REQUIRE_MESSAGE(rw.has_value(), dsl);
    return std::move(*rw);
}

}  // namespace

TEST_SUITE("ResolvedFieldRewriter") {
    TEST_CASE("detects virtual fields") {
        auto a = Query::from_string(R"(resolved.fhash.path ~ "train")");
        auto b = Query::from_string(R"(resolved.hhash.name == "node01")");
        auto c = Query::from_string(R"(cat == "POSIX")");
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        REQUIRE(c.has_value());
        CHECK(dftracer::utils::index::plan::has_resolved_fields(*a));
        CHECK(dftracer::utils::index::plan::has_resolved_fields(*b));
        CHECK_FALSE(dftracer::utils::index::plan::has_resolved_fields(*c));
    }

    TEST_CASE("no rewrite when no virtual fields") {
        auto root = dftu_utils_test::make_unique_test_path("rfr_none");
        auto db = make_db(root.string());
        auto q = Query::from_string(R"(cat == "POSIX" and dur > 10)");
        REQUIRE(q.has_value());
        CHECK_FALSE(dftracer::utils::index::plan::rewrite_resolved_fields(
                        *q, db, get_schema("dftracer"))
                        .has_value());
    }

    TEST_CASE("an old, no-longer-resolved name throws") {
        auto root = dftu_utils_test::make_unique_test_path("rfr_old_name");
        auto db = make_db(root.string());
        auto q = Query::from_string(R"(resolved.fpath == "/var/log/run.txt")");
        REQUIRE(q.has_value());
        try {
            dftracer::utils::index::plan::rewrite_resolved_fields(
                *q, db, get_schema("dftracer"));
            FAIL("expected DFTUtilsException");
        } catch (const DFTUtilsException& e) {
            CHECK(e.code() == ErrorCode::INVALID_ARGUMENT);
            CHECK(std::string(e.what()).find("resolved.fhash.path") !=
                  std::string::npos);
        }
    }

    TEST_CASE("exact fhash.path == resolves to single fhash") {
        auto root = dftu_utils_test::make_unique_test_path("rfr_eq");
        auto db = make_db(root.string());
        auto q =
            rewrite(R"(resolved.fhash.path == "/scratch/data/train.h5")", db);
        check_in_clause(q, "fhash", {"fh_read"});
    }

    TEST_CASE("fhash.path != resolves to the keys that still satisfy it") {
        auto root = dftu_utils_test::make_unique_test_path("rfr_ne");
        auto db = make_db(root.string());
        auto q = rewrite(R"(resolved.fhash.path != "/var/log/run.txt")", db);
        check_in_clause(q, "fhash", {"fh_read", "fh_write"});
    }

    TEST_CASE("regex over fhash.path collects all matching hashes") {
        auto root = dftu_utils_test::make_unique_test_path("rfr_regex");
        auto db = make_db(root.string());
        auto q = rewrite(R"(resolved.fhash.path ~ "/scratch/.*\.h5")", db);
        check_in_clause(q, "fhash", {"fh_read", "fh_write"});
    }

    TEST_CASE("substring 'x' in resolved.fhash.path is case-insensitive") {
        auto root = dftu_utils_test::make_unique_test_path("rfr_sub");
        auto db = make_db(root.string());
        auto q = rewrite(R"('TRAIN' in resolved.fhash.path)", db);
        check_in_clause(q, "fhash", {"fh_read"});
    }

    TEST_CASE("like over fhash.path") {
        auto root = dftu_utils_test::make_unique_test_path("rfr_like");
        auto db = make_db(root.string());
        auto q = rewrite(R"(resolved.fhash.path like "/scratch/%")", db);
        check_in_clause(q, "fhash", {"fh_read", "fh_write"});
    }

    TEST_CASE("negated regex over fhash.path keeps the non-matching keys") {
        auto root = dftu_utils_test::make_unique_test_path("rfr_nregex");
        auto db = make_db(root.string());
        auto q = rewrite(R"(resolved.fhash.path !~ "\.h5")", db);
        check_in_clause(q, "fhash", {"fh_log"});
    }

    TEST_CASE("hhash.name resolves against the host dictionary") {
        auto root = dftu_utils_test::make_unique_test_path("rfr_host");
        auto db = make_db(root.string());
        auto q = rewrite(R"(resolved.hhash.name ilike "NODE%")", db);
        check_in_clause(q, "hhash", {"hh_a"});
    }

    TEST_CASE("no matches yields empty in-clause") {
        auto root = dftu_utils_test::make_unique_test_path("rfr_empty");
        auto db = make_db(root.string());
        auto q = rewrite(R"(resolved.fhash.path ~ "nonexistent")", db);
        check_in_clause(q, "fhash", {});
    }

    TEST_CASE("rewrite + evaluate end-to-end against events") {
        using dftracer::utils::json::JsonValue;
        auto root = dftu_utils_test::make_unique_test_path("rfr_e2e");
        auto db = make_db(root.string());
        // resolved.fhash.path ~ "/scratch" -> fhash in [fh_read, fh_write]
        auto q = rewrite(R"('scratch' in resolved.fhash.path)", db);

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
        auto root = dftu_utils_test::make_unique_test_path("rfr_compose");
        auto db = make_db(root.string());
        auto base = Query::from_string(
            R"(cat == "POSIX" and 'train' in resolved.fhash.path)");
        REQUIRE(base.has_value());
        auto rw = dftracer::utils::index::plan::rewrite_resolved_fields(
            *base, db, get_schema("dftracer"));
        REQUIRE(rw.has_value());
        // Top level stays an AND; the right branch became fhash in [...].
        using namespace dftracer::utils::query;
        const auto& node = rw->root();
        REQUIRE(std::holds_alternative<AndNode>(node.data));
        const auto& an = std::get<AndNode>(node.data);
        CHECK(std::holds_alternative<CompareNode>(an.left->data));
        REQUIRE(std::holds_alternative<InNode>(an.right->data));
        CHECK(std::get<InNode>(an.right->data).field.path == "fhash");
    }

    TEST_CASE("an in-list past SEMI_JOIN_CAP still produces the full list") {
        constexpr std::size_t OVER_CAP = 4097;
        auto root = dftu_utils_test::make_unique_test_path("rfr_semi_join_cap");
        fs::create_directories(root);
        IndexDatabase db((fs::path(root) / ".dftindex").string());
        {
            auto writer = db.begin_write();
            writer->init_schema();
            for (std::size_t i = 0; i < OVER_CAP; ++i)
                dftu_utils_test::index_records::put_dict_row(
                    *writer, "file", "fh_" + std::to_string(i),
                    {{"path", "/scratch/data/shared.h5"}});
            writer->commit();
        }
        auto q =
            rewrite(R"(resolved.fhash.path == "/scratch/data/shared.h5")", db);
        using namespace dftracer::utils::query;
        const auto& node = q.root();
        REQUIRE(std::holds_alternative<InNode>(node.data));
        CHECK(std::get<InNode>(node.data).values.elements.size() == OVER_CAP);
    }
}
