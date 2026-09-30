#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/duql/decorrelate.h>
#include <dftracer/utils/duql/syntax/parser.h>
#include <doctest/doctest.h>

#include <string>

using namespace dftracer::utils::duql;

namespace {

std::optional<Decorrelated> run(const std::string& text, bool scalar) {
    auto tree = syntax::parse(text);
    REQUIRE(tree.has_value());
    return decorrelate(*tree->pipeline, scalar);
}

std::string refusal(const std::string& text, bool scalar) {
    try {
        (void)run(text, scalar);
    } catch (const Refusal& r) {
        return r.message;
    }
    return {};
}

bool has(const std::string& text, std::initializer_list<const char*> parts) {
    for (const char* p : parts)
        if (text.find(p) == std::string::npos) {
            MESSAGE(text);
            return false;
        }
    return true;
}

}  // namespace

TEST_SUITE("duql decorrelate") {
    TEST_CASE("a sub-query that reads no enclosing row is left alone") {
        CHECK(!run("from data | where name == \"a\" | agg { m = mean(dur) }",
                   true));
        CHECK(!run("where any(tags, . == ^.cat) | select x", false));
    }

    TEST_CASE("an equality term becomes a group key and an outer key") {
        const auto d =
            run("from data | where name == ^.name and dur > 1 | agg { m = "
                "mean(dur) }",
                true);
        REQUIRE(d);
        REQUIRE(d->outer.size() == 1);
        CHECK(syntax::to_text(*d->outer[0]) == "name");
        CHECK(d->names == std::vector<std::string>{"__ck0"});
        CHECK(has(syntax::to_text(*d->side),
                  {"dur > 1", "group __ck0 = name", "__correlated_value"}));
        REQUIRE(d->empty);
        CHECK(has(syntax::to_text(*d->empty), {"where false", "agg"}));
    }

    TEST_CASE("either side may hold the enclosing row, several keys") {
        const auto d =
            run("from data | where ^.run * 2 == run + 1 and ^.name == name | "
                "select dur",
                false);
        REQUIRE(d);
        REQUIRE(d->outer.size() == 2);
        CHECK(syntax::to_text(*d->outer[0]) == "run * 2");
        CHECK(syntax::to_text(*d->outer[1]) == "name");
        CHECK(!d->empty);
        CHECK(has(syntax::to_text(*d->side), {"select dur", "__ck0", "__ck1"}));
    }

    TEST_CASE("a derive after the where keeps the keys in a derive") {
        const auto d =
            run("from data | where run == ^.run | derive run = 0 | agg { c = "
                "count() }",
                true);
        REQUIRE(d);
        CHECK(has(syntax::to_text(*d->side), {"derive __ck0 = run"}));
    }

    TEST_CASE("the quantified record is not the enclosing row") {
        const auto d =
            run("from data | where run == ^.run and any(tags, . == ^.cat) | "
                "select dur",
                false);
        REQUIRE(d);
        CHECK(d->outer.size() == 1);
        CHECK(has(syntax::to_text(*d->side), {"any(tags, . == ^.cat)"}));
    }

    TEST_CASE("a bound becomes the range column and an outer bound") {
        const auto d =
            run("from data | where ts < ^.ts and dur > 1 | agg { m = "
                "max(dur) }",
                true);
        REQUIRE(d);
        CHECK(d->outer.empty());
        CHECK(!d->low);
        REQUIRE(d->high);
        CHECK(d->high->open);
        CHECK(syntax::to_text(*d->high->outer) == "ts");
        CHECK(d->read == RangeRead::MAX);
        CHECK(!d->empty);
        CHECK(has(syntax::to_text(*d->side),
                  {"dur > 1", "select __correlated_value = dur",
                   "__correlated_range = ts"}));
    }

    TEST_CASE("a flipped bound, a between band and keys") {
        const auto d =
            run("from data | where ^.pid == pid | where ts + 1 between ^.lo "
                "and ^.hi * 2 | agg { c = count() }",
                true);
        REQUIRE(d);
        REQUIRE(d->outer.size() == 1);
        CHECK(syntax::to_text(*d->outer[0]) == "pid");
        REQUIRE(d->low);
        REQUIRE(d->high);
        CHECK(!d->low->open);
        CHECK(syntax::to_text(*d->low->outer) == "lo");
        CHECK(syntax::to_text(*d->high->outer) == "hi * 2");
        CHECK(d->read == RangeRead::COUNT);
        CHECK(has(syntax::to_text(*d->side),
                  {"__ck0 = pid", "__correlated_range = ts + 1"}));
        const auto f =
            run("from data | where ^.ts > ts | agg { c = count() }", true);
        REQUIRE(f);
        REQUIRE(f->high);
        CHECK(f->high->open);
        CHECK(!f->low);
    }

    TEST_CASE("what a range sub-query gives") {
        auto read = [](const std::string& agg) {
            return run("from data | where ts < ^.ts | agg { v = " + agg + " }",
                       true)
                ->read;
        };
        CHECK(read("count()") == RangeRead::COUNT);
        CHECK(read("count(dur)") == RangeRead::COUNT_VALUES);
        CHECK(read("count_if(dur > 2)") == RangeRead::COUNT_IF);
        CHECK(read("sum(dur)") == RangeRead::SUM);
        CHECK(read("min(dur)") == RangeRead::MIN);
        CHECK(read("mean(dur)") == RangeRead::MEAN);
        CHECK(run("from data | where ts < ^.ts | select dur", true)->read ==
              RangeRead::ROWS);
        const auto g = run("from data | where ts < ^.ts | group fd {}", false);
        REQUIRE(g);
        CHECK(g->read == RangeRead::ROWS);
        CHECK(has(syntax::to_text(*g->side),
                  {"select fd", "__correlated_range = ts"}));
        const auto b =
            run("from data | where ts between 0 and ^.ts | select dur", true);
        REQUIRE(b);
        CHECK(!b->low);
        CHECK(has(syntax::to_text(*b->side), {"ts >= 0"}));
    }

    TEST_CASE("what a correlated sub-query may not hold") {
        CHECK(has(refusal("from data | where ts + ^.dur < end | agg { m = "
                          "max(dur) }",
                          true),
                  {"ts + ^.dur < end", "mixes"}));
        CHECK(has(refusal("from data | where x + ^.y == z | select a", false),
                  {"x + ^.y == z", "mixes"}));
        CHECK(has(refusal("from data | where ts < ^.ts and end > ^.ts | agg "
                          "{ m = max(dur) }",
                          true),
                  {"end > ^.ts", "bounds 'end'", "overlap"}));
        CHECK(has(refusal("from data | where ts < ^.ts and ts <= ^.end | agg "
                          "{ m = max(dur) }",
                          true),
                  {"second upper bound"}));
        CHECK(has(refusal("from data | where ts not between ^.a and ^.b | agg "
                          "{ c = count() }",
                          true),
                  {"not between", "'=='"}));
        CHECK(has(
            refusal("from data | where ts != ^.ts | agg { c = count() }", true),
            {"ts != ^.ts"}));
        CHECK(has(
            refusal("from data | where ts < ^.ts | agg { m = var(dur) }", true),
            {"'var'", "count, count_if"}));
        CHECK(has(refusal("from data | where ts < ^.ts | agg { m = max(dur) }",
                          false),
                  {"'in' over a correlated range"}));
        CHECK(has(refusal("from data | where ts < ^.ts | group fd { c = "
                          "count() }",
                          false),
                  {"'in' over a correlated range"}));
        CHECK(has(
            refusal("from data | where ^.a == ^.b | agg { c = count() }", true),
            {"^.a"}));
        CHECK(has(refusal("from data | where run == ^.run | take 3 | agg { c "
                          "= count() }",
                          true),
                  {"take", "correlated"}));
        CHECK(has(refusal("from data | where run == ^.run | agg { c = "
                          "count() } | sort c",
                          true),
                  {"'agg'", "last stage"}));
        CHECK(has(refusal("from data | where run == ^.run | sort dur | agg "
                          "{ c = count() }",
                          true),
                  {"'sort'", "correlated"}));
        CHECK(has(refusal("from data | where run == ^.run | parse name ~ "
                          "\"(?<op>[a-z]+)\" | agg { c = count() }",
                          true),
                  {"'parse'", "correlated"}));
        CHECK(has(refusal("from data | derive z = ^.x | where run == ^.run | "
                          "agg { c = count() }",
                          true),
                  {"^.x", "other than 'where'"}));
        CHECK(has(refusal("from data | where run == ^.run", false),
                  {"select", "group", "agg"}));
        CHECK(has(refusal("from data | where run == ^.run | group name { c = "
                          "count() }",
                          true),
                  {"not 'group'"}));
        CHECK(has(refusal("from data | where run == ^.run | select a, b", true),
                  {"one column", "2"}));
    }
}
