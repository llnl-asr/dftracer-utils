#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/comparator/comparison_config.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::utilities::composites::dft::comparator;

TEST_SUITE("ComparisonConfig") {
    TEST_CASE("from_cli - default query and group_by") {
        auto cfg = ComparisonConfig::from_cli("base/", "var/", "", "");
        REQUIRE(cfg.nodes.size() == 1);
        const auto& node = cfg.nodes[0];
        // Default query covers POSIX and STDIO
        CHECK(node.query.find("POSIX") != std::string::npos);
        CHECK(node.query.find("STDIO") != std::string::npos);
        // Default group_by is cat,name
        REQUIRE(node.group_by.size() == 2);
        CHECK(node.group_by[0] == "cat");
        CHECK(node.group_by[1] == "name");
    }

    TEST_CASE("from_cli - custom query") {
        auto cfg = ComparisonConfig::from_cli("base/", "var/",
                                              R"(cat == "POSIX")", "");
        REQUIRE(cfg.nodes.size() == 1);
        CHECK(cfg.nodes[0].query == R"(cat == "POSIX")");
    }

    TEST_CASE("from_cli - custom group_by") {
        auto cfg =
            ComparisonConfig::from_cli("base/", "var/", "", "cat,name,pid");
        REQUIRE(cfg.nodes.size() == 1);
        const auto& gb = cfg.nodes[0].group_by;
        REQUIRE(gb.size() == 3);
        CHECK(gb[0] == "cat");
        CHECK(gb[1] == "name");
        CHECK(gb[2] == "pid");
    }

    TEST_CASE("resolve - inherits defaults") {
        ComparisonConfig cfg;
        cfg.defaults.metrics = {"count", "duration"};
        cfg.defaults.percentiles = {0.50, 0.99};
        cfg.defaults.threshold_pct = 5.0;
        cfg.defaults.sort_by = "regression";

        ComparisonNode node;
        node.name = "root";
        node.query = "cat == \"POSIX\"";
        cfg.nodes.push_back(node);

        cfg.resolve();

        const auto& resolved = cfg.nodes[0];
        REQUIRE(resolved.resolved_metrics.size() == 2);
        CHECK(resolved.resolved_metrics[0] == "count");
        CHECK(resolved.resolved_metrics[1] == "duration");
        REQUIRE(resolved.resolved_percentiles.size() == 2);
        CHECK(resolved.resolved_percentiles[0] ==
              doctest::Approx(0.50).epsilon(1e-9));
        CHECK(resolved.resolved_threshold_pct ==
              doctest::Approx(5.0).epsilon(1e-9));
        CHECK(resolved.resolved_sort_by == "regression");
    }

    TEST_CASE("resolve - node overrides") {
        ComparisonConfig cfg;
        cfg.defaults.metrics = {"count", "duration", "size"};

        ComparisonNode node;
        node.name = "root";
        node.query = "cat == \"POSIX\"";
        node.metrics = std::vector<std::string>{"count"};
        cfg.nodes.push_back(node);

        cfg.resolve();

        const auto& resolved = cfg.nodes[0];
        REQUIRE(resolved.resolved_metrics.size() == 1);
        CHECK(resolved.resolved_metrics[0] == "count");
    }

    TEST_CASE("resolve - composed query") {
        ComparisonConfig cfg;

        ComparisonNode parent;
        parent.name = "parent";
        parent.query = "cat == \"POSIX\"";

        ComparisonNode child;
        child.name = "child";
        child.query = "name == \"read\"";
        parent.children.push_back(child);

        cfg.nodes.push_back(parent);
        cfg.resolve();

        const auto& resolved_child = cfg.nodes[0].children[0];
        // composed = "(parent_query) AND (child_query)"
        CHECK(resolved_child.composed_query.find("POSIX") != std::string::npos);
        CHECK(resolved_child.composed_query.find("read") != std::string::npos);
        CHECK(resolved_child.composed_query.find("AND") != std::string::npos);
    }

    TEST_CASE("resolve - empty child query inherits parent") {
        ComparisonConfig cfg;

        ComparisonNode parent;
        parent.name = "parent";
        parent.query = "cat == \"POSIX\"";

        ComparisonNode child;
        child.name = "child";
        child.query = "";  // empty -> inherits parent's composed
        parent.children.push_back(child);

        cfg.nodes.push_back(parent);
        cfg.resolve();

        const auto& resolved_parent = cfg.nodes[0];
        const auto& resolved_child = resolved_parent.children[0];
        CHECK(resolved_child.composed_query == resolved_parent.composed_query);
    }

    TEST_CASE("defaults - default values") {
        ComparisonDefaults d;
        REQUIRE(d.metrics.size() == 5);
        CHECK(d.metrics[0] == "count");
        CHECK(d.metrics[1] == "duration");
        CHECK(d.metrics[2] == "size");
        CHECK(d.metrics[3] == "transfer_size");
        CHECK(d.metrics[4] == "bandwidth");

        REQUIRE(d.percentiles.size() == 3);
        CHECK(d.percentiles[0] == doctest::Approx(0.50).epsilon(1e-9));
        CHECK(d.percentiles[1] == doctest::Approx(0.95).epsilon(1e-9));
        CHECK(d.percentiles[2] == doctest::Approx(0.99).epsilon(1e-9));

        CHECK(d.threshold_pct == doctest::Approx(0.0).epsilon(1e-9));
        CHECK(d.time_interval_ms == doctest::Approx(5000.0).epsilon(1e-9));
    }
}

TEST_SUITE("ComparisonConfigPreset") {
    TEST_CASE("from_preset - unknown preset returns nullopt") {
        auto cfg = ComparisonConfig::from_preset("unknown", "base/", "var/");
        CHECK(!cfg.has_value());
    }

    TEST_CASE("from_preset - dlio has 8 top-level nodes") {
        auto cfg = ComparisonConfig::from_preset("dlio", "base/", "var/");
        REQUIRE(cfg.has_value());
        CHECK(cfg->baseline == "base/");
        CHECK(cfg->variant == "var/");
        CHECK(cfg->nodes.size() == 8);
    }

    TEST_CASE("from_preset - dlio node names") {
        auto cfg = ComparisonConfig::from_preset("dlio", "b", "v");
        REQUIRE(cfg.has_value());
        const auto& nodes = cfg->nodes;
        REQUIRE(nodes.size() == 8);
        CHECK(nodes[0].name == "Pipeline");
        CHECK(nodes[1].name == "Data Ingestion");
        CHECK(nodes[2].name == "Checkpointing");
        CHECK(nodes[3].name == "Compute");
        CHECK(nodes[4].name == "Storage");
        CHECK(nodes[5].name == "POSIX/STDIO I/O");
        CHECK(nodes[6].name == "Communication");
        CHECK(nodes[7].name == "Benchmark");
    }

    TEST_CASE("from_preset - dlio every node has a non-empty query") {
        auto cfg = ComparisonConfig::from_preset("dlio", "b", "v");
        REQUIRE(cfg.has_value());
        for (const auto& n : cfg->nodes) {
            CHECK_FALSE(n.query.empty());
        }
    }

    TEST_CASE("from_preset - dlio Pipeline has 4 children") {
        auto cfg = ComparisonConfig::from_preset("dlio", "b", "v");
        REQUIRE(cfg.has_value());
        const auto& pipeline = cfg->nodes[0];
        REQUIRE(pipeline.children.size() == 4);
        CHECK(pipeline.children[0].name == "Epoch");
        CHECK(pipeline.children[1].name == "Train");
        CHECK(pipeline.children[2].name == "Evaluate");
        CHECK(pipeline.children[3].name == "Test");
    }

    TEST_CASE("from_preset - dlio POSIX/STDIO I/O has 4 children") {
        auto cfg = ComparisonConfig::from_preset("dlio", "b", "v");
        REQUIRE(cfg.has_value());
        const auto& posix = cfg->nodes[5];
        REQUIRE(posix.children.size() == 4);
        CHECK(posix.children[0].name == "Read");
        CHECK(posix.children[1].name == "Write");
        CHECK(posix.children[2].name == "Metadata");
        CHECK(posix.children[3].name == "Sync");
    }

    TEST_CASE(
        "from_preset - dlio POSIX/STDIO I/O children have non-empty queries") {
        auto cfg = ComparisonConfig::from_preset("dlio", "b", "v");
        REQUIRE(cfg.has_value());
        for (const auto& child : cfg->nodes[5].children) {
            CHECK_FALSE(child.query.empty());
        }
    }

    TEST_CASE("from_preset - dlio POSIX/STDIO query covers both cats") {
        auto cfg = ComparisonConfig::from_preset("dlio", "b", "v");
        REQUIRE(cfg.has_value());
        const auto& q = cfg->nodes[5].query;
        CHECK(q.find("POSIX") != std::string::npos);
        CHECK(q.find("STDIO") != std::string::npos);
    }
}
