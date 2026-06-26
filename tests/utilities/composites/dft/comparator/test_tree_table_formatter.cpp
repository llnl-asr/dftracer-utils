#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/comparator/comparison_result.h>
#include <dftracer/utils/utilities/composites/dft/comparator/tree_table_formatter.h>
#include <doctest/doctest.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace dftracer::utils::utilities::composites::dft::comparator;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static MetricComparison make_mc(std::string name, double base, double var) {
    MetricComparison mc;
    mc.metric_name = std::move(name);
    mc.baseline_value = base;
    mc.variant_value = var;
    mc.delta = var - base;
    mc.pct_change = base != 0.0 ? (var - base) / base * 100.0 : 100.0;
    return mc;
}

static NodeResult make_node(std::string name,
                            std::vector<MetricComparison> summary = {},
                            std::vector<NodeResult> children = {}) {
    NodeResult n;
    n.name = std::move(name);
    n.summary.metrics = std::move(summary);
    n.children = std::move(children);
    return n;
}

// Render output with colors and unicode disabled for predictable string
// matching.
static std::string render_to_string(const ComparisonOutput& output) {
    FormatterOptions opts;
    opts.use_color = false;
    opts.use_unicode = false;
    TreeTableFormatter fmt(opts);

    auto* f = tmpfile();
    REQUIRE(f != nullptr);
    fmt.render(f, output);
    rewind(f);

    std::string result;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        result.append(buf, n);
    }
    std::fclose(f);
    return result;
}

static std::size_t count_occurrences(const std::string& haystack,
                                     const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// ---------------------------------------------------------------------------
// Empty node display
// ---------------------------------------------------------------------------

TEST_SUITE("TreeTableFormatter_EmptyNode") {
    TEST_CASE("empty leaf node shows (no data) and no SUMMARY") {
        ComparisonOutput out;
        out.nodes.push_back(make_node("Lifecycle"));

        auto s = render_to_string(out);
        CHECK(s.find("(no data)") != std::string::npos);
        CHECK(s.find("SUMMARY") == std::string::npos);
    }

    TEST_CASE("multiple empty top-level nodes each get (no data)") {
        ComparisonOutput out;
        out.nodes.push_back(make_node("Lifecycle"));
        out.nodes.push_back(make_node("Checkpoint I/O"));

        auto s = render_to_string(out);
        CHECK(count_occurrences(s, "(no data)") == 2);
        CHECK(s.find("SUMMARY") == std::string::npos);
    }

    TEST_CASE(
        "empty child node shows (no data), parent SUMMARY still renders") {
        ComparisonOutput out;
        auto child = make_node("Read");
        auto parent =
            make_node("POSIX", {make_mc("count", 50.0, 60.0)}, {child});
        out.nodes.push_back(parent);

        auto s = render_to_string(out);
        CHECK(s.find("SUMMARY") != std::string::npos);
        CHECK(s.find("(no data)") != std::string::npos);
        CHECK(s.find("Read") != std::string::npos);
    }

    TEST_CASE(
        "parent with empty summary but child with data omits parent SUMMARY") {
        ComparisonOutput out;
        auto child = make_node("Read", {make_mc("count", 100.0, 150.0)});
        auto parent = make_node("POSIX/STDIO", {}, {child});
        out.nodes.push_back(parent);

        auto s = render_to_string(out);
        // Parent has no summary data so its SUMMARY header should be absent.
        // Child has data so SUMMARY appears exactly once (for the child).
        CHECK(count_occurrences(s, "SUMMARY") == 1);
        CHECK(s.find("(no data)") == std::string::npos);
    }

    TEST_CASE(
        "all children empty: parent with data still renders, children get (no "
        "data)") {
        ComparisonOutput out;
        auto child1 = make_node("Save");
        auto child2 = make_node("Load");
        auto parent = make_node("Checkpointing", {make_mc("count", 10.0, 12.0)},
                                {child1, child2});
        out.nodes.push_back(parent);

        auto s = render_to_string(out);
        CHECK(s.find("SUMMARY") != std::string::npos);
        CHECK(count_occurrences(s, "(no data)") == 2);
        CHECK(s.find("Save") != std::string::npos);
        CHECK(s.find("Load") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Non-empty node display
// ---------------------------------------------------------------------------

TEST_SUITE("TreeTableFormatter_NonEmptyNode") {
    TEST_CASE("non-empty leaf node shows SUMMARY and no (no data)") {
        ComparisonOutput out;
        out.nodes.push_back(
            make_node("Read", {make_mc("count", 100.0, 150.0)}));

        auto s = render_to_string(out);
        CHECK(s.find("SUMMARY") != std::string::npos);
        CHECK(s.find("(no data)") == std::string::npos);
    }

    TEST_CASE("non-empty node metric name appears in output") {
        ComparisonOutput out;
        out.nodes.push_back(
            make_node("Read", {make_mc("count", 100.0, 150.0)}));

        auto s = render_to_string(out);
        CHECK(s.find("count") != std::string::npos);
    }

    TEST_CASE("non-empty nested node renders all levels") {
        ComparisonOutput out;
        auto child = make_node("Read", {make_mc("count", 10.0, 20.0)});
        auto parent =
            make_node("POSIX", {make_mc("count", 50.0, 60.0)}, {child});
        out.nodes.push_back(parent);

        auto s = render_to_string(out);
        // Both POSIX and Read appear
        CHECK(s.find("POSIX") != std::string::npos);
        CHECK(s.find("Read") != std::string::npos);
        // Two SUMMARY blocks (parent + child)
        CHECK(count_occurrences(s, "SUMMARY") == 2);
        CHECK(s.find("(no data)") == std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// JSON output for empty nodes
// ---------------------------------------------------------------------------

TEST_SUITE("TreeTableFormatter_Json") {
    TEST_CASE("empty node still appears in JSON with empty metrics array") {
        ComparisonOutput out;
        out.nodes.push_back(make_node("Lifecycle"));

        FormatterOptions opts;
        opts.use_color = false;
        opts.use_unicode = false;
        TreeTableFormatter fmt(opts);
        auto json = fmt.render_json(out);

        CHECK(json.find("\"Lifecycle\"") != std::string::npos);
        CHECK(json.find("\"metrics\":[]") != std::string::npos);
    }

    TEST_CASE("non-empty node metrics appear in JSON") {
        ComparisonOutput out;
        out.nodes.push_back(
            make_node("Read", {make_mc("count", 100.0, 150.0)}));

        FormatterOptions opts;
        opts.use_color = false;
        TreeTableFormatter fmt(opts);
        auto json = fmt.render_json(out);

        CHECK(json.find("\"count\"") != std::string::npos);
        CHECK(json.find("\"baseline\":100") != std::string::npos);
        CHECK(json.find("\"variant\":150") != std::string::npos);
    }
}
