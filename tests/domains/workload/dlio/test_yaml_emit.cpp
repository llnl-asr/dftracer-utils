// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/domains/workload/dlio/yaml_emit.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/yaml/document.h>
#include <doctest/doctest.h>

#include <cmath>
#include <limits>

using namespace dftracer::utils;
using namespace dftracer::utils::domains::workload::dlio;
using yaml::Document;
using yaml::Value;

namespace {

DlioTimingBlock normal_block(double mean, double stdev, double max_bound) {
    FittedDistribution f;
    f.kind = DistributionKind::Normal;
    f.params = {mean, stdev, 0.0};
    return DlioTimingBlock{BestModel(f), max_bound};
}

DlioTimingBlock single_block(DistributionKind kind, double a, double b,
                             double max_bound) {
    FittedDistribution f;
    f.kind = kind;
    f.params = {a, b, 0.0};
    return DlioTimingBlock{BestModel(f), max_bound};
}

DlioTimingBlock mixture_block(double max_bound) {
    FittedMixture m;
    m.weights = {0.25, 0.75};
    m.components = {{1.5, 0.5}, {10.0, 2.25}};
    return DlioTimingBlock{BestModel(m), max_bound};
}

dstd::String max_bound_text(double v) {
    const DlioTimingBlock block = normal_block(1.0, 1.0, v);
    const dstd::String text = render_dlio_yaml(&block, nullptr);
    const dstd::String key = "    max_bound: ";
    const std::size_t at = text.find(key);
    REQUIRE(at != dstd::String::npos);
    return text.substr(at + key.size());
}

}  // namespace

TEST_CASE("yaml_emit: both blocks of a single distribution") {
    const DlioTimingBlock comp =
        single_block(DistributionKind::Normal, 0.37, 2.5, 9.75);
    const DlioTimingBlock prep =
        single_block(DistributionKind::Normal, 0.0, 1.0, 1.0);
    CHECK(render_dlio_yaml(&comp, &prep) ==
          "train:\n"
          "  computation_time:\n"
          "    type: normal\n"
          "    mean: 0.37\n"
          "    stdev: 2.5\n"
          "    max_bound: 9.75\n"
          "reader:\n"
          "  preprocess_time:\n"
          "    type: normal\n"
          "    mean: 0.0\n"
          "    stdev: 1.0\n"
          "    max_bound: 1.0");
}

TEST_CASE("yaml_emit: every distribution kind") {
    const DlioTimingBlock lognormal =
        single_block(DistributionKind::Lognormal, 0.37, 2.5, 9.75);
    CHECK(render_dlio_yaml(&lognormal, nullptr) ==
          "train:\n"
          "  computation_time:\n"
          "    type: lognormal\n"
          "    mean: 0.37\n"
          "    sigma: 2.5\n"
          "    max_bound: 9.75");
    const DlioTimingBlock gamma =
        single_block(DistributionKind::Gamma, 0.37, 2.5, 9.75);
    CHECK(render_dlio_yaml(&gamma, nullptr) ==
          "train:\n"
          "  computation_time:\n"
          "    type: gamma\n"
          "    shape: 0.37\n"
          "    scale: 2.5\n"
          "    max_bound: 9.75");
    const DlioTimingBlock weibull =
        single_block(DistributionKind::Weibull, 0.37, 2.5, 9.75);
    CHECK(render_dlio_yaml(&weibull, nullptr) ==
          "train:\n"
          "  computation_time:\n"
          "    type: weibull\n"
          "    shape: 0.37\n"
          "    scale: 2.5\n"
          "    max_bound: 9.75");
    const DlioTimingBlock exponential =
        single_block(DistributionKind::Exponential, 0.37, 0.0, 9.75);
    CHECK(render_dlio_yaml(&exponential, nullptr) ==
          "train:\n"
          "  computation_time:\n"
          "    type: exponential\n"
          "    scale: 2.7027027027027026\n"
          "    max_bound: 9.75");
    const DlioTimingBlock no_rate =
        single_block(DistributionKind::Exponential, 0.0, 0.0, 1.0);
    CHECK(render_dlio_yaml(&no_rate, nullptr) ==
          "train:\n"
          "  computation_time:\n"
          "    type: exponential\n"
          "    scale: 0.0\n"
          "    max_bound: 1.0");
}

TEST_CASE("yaml_emit: a mixture and a lone reader block") {
    const DlioTimingBlock block = mixture_block(42.0);
    CHECK(render_dlio_yaml(nullptr, &block) ==
          "reader:\n"
          "  preprocess_time:\n"
          "    type: mixture\n"
          "    n_components: 2\n"
          "    components:\n"
          "      - weight: 0.25\n"
          "        params:\n"
          "          type: normal\n"
          "          mean: 1.5\n"
          "          stdev: 0.5\n"
          "      - weight: 0.75\n"
          "        params:\n"
          "          type: normal\n"
          "          mean: 10.0\n"
          "          stdev: 2.25\n"
          "    max_bound: 42.0");
}

TEST_CASE("yaml_emit: no block gives empty text") {
    CHECK(render_dlio_yaml(nullptr, nullptr).empty());
}

TEST_CASE("yaml_emit: numbers print as the previous writer printed them") {
    struct Case {
        double value;
        const char* text;
    };
    const Case cases[] = {
        {0.0, "0.0"},
        {-0.0, "-0.0"},
        {1.0, "1.0"},
        {0.1, "0.1"},
        {1.0 / 3.0, "0.3333333333333333"},
        {0.30000000000000004, "0.30000000000000004"},
        {123456.789, "123456.789"},
        {99999.5, "99999.5"},
        {1234567.0, "1234567.0"},
        {100.0, "100.0"},
        {-2.5, "-2.5"},
        {0.0001, "1.0e-04"},
        {0.000123, "0.000123"},
        {1e-5, "1.0e-05"},
        {1.5e-5, "1.5e-05"},
        {1e-7, "1.0e-07"},
        {5e-324, "5.0e-324"},
        {1e16, "1.0e+16"},
        {1e17, "1.0e+17"},
        {1.234e20, "1.234e+20"},
        {1e300, "1.0e+300"},
        {1.7976931348623157e308, "1.7976931348623157e+308"},
        {std::numeric_limits<double>::infinity(), ".inf"},
        {-std::numeric_limits<double>::infinity(), "-.inf"},
        {std::numeric_limits<double>::quiet_NaN(), ".nan"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.text);
        CHECK(max_bound_text(c.value) == c.text);
    }
}

TEST_CASE("yaml_emit: the text parses back to the same values") {
    const DlioTimingBlock comp = mixture_block(12.125);
    const DlioTimingBlock prep =
        single_block(DistributionKind::Weibull, 1.0 / 3.0, 2e-7, 0.1);
    const dstd::String text = render_dlio_yaml(&comp, &prep);
    auto document = Document::parse(text, "");
    REQUIRE(document.has_value());
    const Value root = document->root();

    const auto computation = root.find("train")->find("computation_time");
    REQUIRE(computation);
    CHECK(computation->find("type")->raw() == "mixture");
    CHECK(computation->find("n_components")->as_uint() == 2);
    CHECK(computation->find("max_bound")->as_double() == 12.125);
    dstd::Vector<Value> items;
    computation->find("components")->for_each_element([&](Value v) {
        items.push_back(v);
    });
    REQUIRE(items.size() == 2);
    CHECK(items[0].find("weight")->as_double() == 0.25);
    CHECK(items[0].find("params")->find("mean")->as_double() == 1.5);
    CHECK(items[0].find("params")->find("stdev")->as_double() == 0.5);
    CHECK(items[1].find("weight")->as_double() == 0.75);
    CHECK(items[1].find("params")->find("mean")->as_double() == 10.0);
    CHECK(items[1].find("params")->find("stdev")->as_double() == 2.25);

    const auto preprocess = root.find("reader")->find("preprocess_time");
    REQUIRE(preprocess);
    CHECK(preprocess->find("type")->raw() == "weibull");
    CHECK(preprocess->find("shape")->as_double() == 1.0 / 3.0);
    CHECK(preprocess->find("scale")->as_double() == 2e-7);
    CHECK(preprocess->find("max_bound")->as_double() == 0.1);
}
