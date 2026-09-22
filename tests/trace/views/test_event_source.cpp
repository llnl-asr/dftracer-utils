#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/trace/views/event_source.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <map>
#include <optional>
#include <string>

using dftracer::utils::StringIntern;

// DomSource must reproduce a raw top-then-args simdjson extraction
// byte-for-byte (the same contract the old view_aggregate helpers gave), so the
// hot-path rewire is identical by construction.
using namespace dftracer::utils::trace::views::detail;

namespace {

std::string padded(std::string s) {
    s.resize(s.size() + simdjson::SIMDJSON_PADDING, '\0');
    return s;
}

simdjson::dom::element parse(simdjson::dom::parser& p, std::string& buf,
                             const char* json) {
    buf = padded(json);
    return p.parse(buf.data(), buf.size() - simdjson::SIMDJSON_PADDING, false)
        .value_unsafe();
}

// Independent references: a top-then-args string / number extraction straight
// over the simdjson element, mirroring the contract EventSource must satisfy.
std::string to_str_ref(simdjson::dom::element e) {
    std::string_view s;
    if (e.get_string().get(s) == simdjson::SUCCESS) return std::string(s);
    std::int64_t i;
    if (e.get_int64().get(i) == simdjson::SUCCESS) return std::to_string(i);
    std::uint64_t u;
    if (e.get_uint64().get(u) == simdjson::SUCCESS) return std::to_string(u);
    double d;
    if (e.get_double().get(d) == simdjson::SUCCESS) return std::to_string(d);
    bool b;
    if (e.get_bool().get(b) == simdjson::SUCCESS) return b ? "true" : "false";
    return {};
}

std::optional<double> to_number_ref(simdjson::dom::element e) {
    double d;
    if (e.get_double().get(d) == simdjson::SUCCESS) return d;
    std::int64_t i;
    if (e.get_int64().get(i) == simdjson::SUCCESS)
        return static_cast<double>(i);
    std::uint64_t u;
    if (e.get_uint64().get(u) == simdjson::SUCCESS)
        return static_cast<double>(u);
    return std::nullopt;
}

std::string top_or_args_str(simdjson::dom::element root,
                            const std::string& key) {
    auto r = root[key];
    if (!r.error()) return to_str_ref(r.value_unsafe());
    auto args = root["args"];
    if (!args.error() && args.is_object()) {
        auto rr = args[key];
        if (!rr.error()) return to_str_ref(rr.value_unsafe());
    }
    return {};
}

std::optional<double> agg_field(simdjson::dom::element root,
                                const std::string& field) {
    auto r = root[field];
    if (!r.error()) return to_number_ref(r.value_unsafe());
    auto args = root["args"];
    if (!args.error() && args.is_object()) {
        auto rr = args[field];
        if (!rr.error()) return to_number_ref(rr.value_unsafe());
    }
    return std::nullopt;
}

}  // namespace

TEST_SUITE("EventSource") {
    TEST_CASE("DomSource.value matches top_or_args_str across field shapes") {
        simdjson::dom::parser p;
        std::string buf;
        auto root = parse(
            p, buf,
            R"({"ph":"X","name":"read","cat":"POSIX","pid":42,"tid":7,"ts":100,"dur":9,"args":{"fhash":"fh1","ret":4096,"level":"warn"}})");
        DomSource src(root);

        // top-level string, top-level int, args string, args int, missing.
        for (const char* field : {"name", "cat", "pid", "tid", "ts", "dur",
                                  "fhash", "ret", "level", "absent"}) {
            CAPTURE(field);
            CHECK(src.value(field) == top_or_args_str(root, field));
        }
    }

    TEST_CASE("DomSource.number matches a raw top-then-args agg_field") {
        simdjson::dom::parser p;
        std::string buf;
        auto root = parse(
            p, buf,
            R"({"ph":"X","name":"read","pid":1,"ts":10,"dur":3,"args":{"ret":8192,"offset":512}})");
        DomSource src(root);

        // Non-derived fields: agg_field is a plain top-then-args number.
        for (const char* field : {"pid", "ts", "dur", "ret", "offset"}) {
            CAPTURE(field);
            auto got = src.number(field);
            auto want = agg_field(root, field);
            REQUIRE(got.has_value() == want.has_value());
            if (got) CHECK(*got == doctest::Approx(*want));
        }
        CHECK_FALSE(src.number("absent").has_value());
    }

    TEST_CASE("append_value writes the same bytes value returns") {
        simdjson::dom::parser p;
        std::string buf;
        auto root =
            parse(p, buf,
                  R"({"ph":"X","name":"n","pid":123,"args":{"k":"v","m":9}})");
        DomSource src(root);
        for (const char* field : {"name", "pid", "k", "m", "absent"}) {
            std::string out = "pre|";
            src.append_value(out, field);
            CHECK(out == "pre|" + src.value(field));
        }
    }

    // The payoff: an event read through PodSource (owned, interned) must yield
    // the same bytes as through DomSource (live element), so AggFold over the
    // POD equals scan_fold over the element.
    TEST_CASE("PodSource reproduces DomSource on the same event") {
        simdjson::dom::parser p;
        std::string buf;
        auto root = parse(
            p, buf,
            R"({"ph":"X","name":"read","cat":"POSIX","pid":42,"tid":7,"ts":100,"dur":9,"args":{"fhash":"fh1","hhash":"hh2","ret":4096,"rate":1.5,"path":"/x"}})");

        StringIntern intern;
        FoldEvent ev = extract_fold_event(root, intern, /*needs_args=*/true);
        DomSource dom(root);
        PodSource pod(ev, intern);

        for (const char* field :
             {"name", "cat", "pid", "tid", "ts", "dur", "fhash", "hhash", "ret",
              "rate", "path", "absent"}) {
            CAPTURE(field);
            CHECK(pod.value(field) == dom.value(field));
            auto pn = pod.number(field);
            auto dn = dom.number(field);
            CHECK(pn.has_value() == dn.has_value());
            if (pn) CHECK(*pn == doctest::Approx(*dn));
        }

        // Group dims read args-only (fhash/hhash/arg), which must also match.
        for (const char* key : {"fhash", "hhash", "ret", "path", "absent"}) {
            CAPTURE(key);
            std::string pd, dd;
            pod.append_arg(pd, key);
            dom.append_arg(dd, key);
            CHECK(pd == dd);
        }

        std::map<std::string, double> dom_args, pod_args;
        dom.for_each_numeric_arg([&](std::string_view k, double v) {
            dom_args[std::string(k)] = v;
        });
        pod.for_each_numeric_arg([&](std::string_view k, double v) {
            pod_args[std::string(k)] = v;
        });
        CHECK(dom_args == pod_args);
        CHECK(pod.phase() == dom.phase());
    }

    // An arg key that itself contains dots ("cqe.raw_ns") is one flat member,
    // not a nested object. It must resolve by its stored name, written bare or
    // with the explicit "args." prefix, identically through both sources.
    TEST_CASE("flat dotted arg key resolves bare and args.-prefixed") {
        simdjson::dom::parser p;
        std::string buf;
        auto root = parse(
            p, buf,
            R"({"ph":"X","name":"rdma_send","pid":9,"args":{"cqe.raw_ns":2850744041253792,"ibv.qp":4712,"mlx5.op":"SEND"}})");

        StringIntern intern;
        FoldEvent ev = extract_fold_event(root, intern, /*needs_args=*/true);
        DomSource dom(root);
        PodSource pod(ev, intern);

        for (const char* field : {"cqe.raw_ns", "args.cqe.raw_ns", "ibv.qp",
                                  "args.ibv.qp", "mlx5.op", "args.mlx5.op"}) {
            CAPTURE(field);
            CHECK(dom.value(field) == pod.value(field));
            auto dn = dom.number(field);
            auto pn = pod.number(field);
            CHECK(dn.has_value() == pn.has_value());
            if (dn) CHECK(*dn == doctest::Approx(*pn));
        }

        CHECK(dom.number("cqe.raw_ns").has_value());
        CHECK(dom.value("args.cqe.raw_ns") == "2850744041253792");
        CHECK(dom.value("mlx5.op") == "SEND");
    }

    TEST_CASE("for_each_numeric_arg visits every numeric arg once") {
        simdjson::dom::parser p;
        std::string buf;
        auto root = parse(
            p, buf,
            R"({"ph":"X","name":"n","args":{"ret":4096,"offset":512,"path":"/x","dur_d":1.5}})");
        DomSource src(root);

        std::map<std::string, double> seen;
        src.for_each_numeric_arg(
            [&](std::string_view k, double v) { seen[std::string(k)] = v; });

        CHECK(seen.size() == 3);  // ret, offset, dur_d -- not the string path
        CHECK(seen["ret"] == doctest::Approx(4096));
        CHECK(seen["offset"] == doctest::Approx(512));
        CHECK(seen["dur_d"] == doctest::Approx(1.5));
    }
}
