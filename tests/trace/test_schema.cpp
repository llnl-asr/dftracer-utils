#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/trace/event.h>
#include <dftracer/utils/trace/schema.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <string>

using namespace dftracer::utils::trace;
using dftracer::utils::json::JsonValue;

namespace {

// Parse one NDJSON line into a DFTracerEvent via the DOM path.
DFTracerEvent parse_line(simdjson::dom::parser& p, const std::string& line) {
    std::string pad = line;
    pad.resize(line.size() + simdjson::SIMDJSON_PADDING);
    auto doc = p.parse(pad.data(), line.size(), false);
    DFTracerEvent ev;
    JsonValue jv(doc.value());
    DFTracerEvent::parse(jv, ev);
    return ev;
}

}  // namespace

TEST_SUITE("schema") {
    TEST_CASE("phase codec maps integers and legacy letters both ways") {
        CHECK(phase_from_int(1) == RecordPhase::Complete);
        CHECK(phase_from_int(2) == RecordPhase::Counter);
        CHECK(phase_from_int(3) == RecordPhase::Aggregated);
        CHECK(phase_from_int(4) == RecordPhase::Metadata);
        CHECK(phase_from_int(0) == RecordPhase::Unknown);
        CHECK(phase_from_int(99) == RecordPhase::Unknown);

        CHECK(phase_from_letter("X") == RecordPhase::Complete);
        CHECK(phase_from_letter("C") == RecordPhase::Counter);
        CHECK(phase_from_letter("A") == RecordPhase::Aggregated);
        CHECK(phase_from_letter("M") == RecordPhase::Metadata);
        CHECK(phase_from_letter("Z") == RecordPhase::Unknown);
        CHECK(phase_from_letter("") == RecordPhase::Unknown);

        CHECK(phase_to_int(RecordPhase::Aggregated) == 3);
        CHECK(phase_to_letter(RecordPhase::Complete) == 'X');
        CHECK(phase_to_letter(RecordPhase::Unknown) == '\0');
    }

    TEST_CASE("event type codec: integers, cat inference, unknown fallback") {
        CHECK(event_type_from_int(1) == EventType::Dftracer);
        CHECK(event_type_from_int(3) == EventType::LibcIo);
        CHECK(event_type_from_int(10) == EventType::Mpi);
        CHECK(event_type_from_int(0) == EventType::Unknown);
        CHECK(event_type_from_int(42) == EventType::Unknown);

        CHECK(event_type_from_cat("POSIX") == EventType::LibcIo);
        CHECK(event_type_from_cat("STDIO") == EventType::LibcIo);
        CHECK(event_type_from_cat("sys") == EventType::Psutil);
        CHECK(event_type_from_cat("MPI") == EventType::Mpi);
        CHECK(event_type_from_cat("whatever") == EventType::Unknown);

        CHECK(event_type_to_int(EventType::Psutil) == 7);
    }
}

TEST_SUITE("event-format") {
    TEST_CASE("new-format complete event: integer ph + type") {
        simdjson::dom::parser p;
        auto ev = parse_line(
            p,
            R"({"name":"read","cat":"POSIX","type":3,"pid":1,"tid":2,"ts":10,"dur":5,"ph":1,"args":{"hhash":"h"}})");
        CHECK(ev.is_complete());
        CHECK_FALSE(ev.is_metadata());
        CHECK_FALSE(ev.is_counter());
        CHECK(ev.phase == RecordPhase::Complete);
        CHECK(ev.type == EventType::LibcIo);
        CHECK(ev.dur == 5);
        CHECK(ev.has_dur);
    }

    TEST_CASE("counter (ph:2) and aggregated (ph:3) are distinct") {
        simdjson::dom::parser p;
        auto c = parse_line(
            p,
            R"({"name":"cpu","cat":"sys","type":7,"ph":2,"ts":1,"args":{"user_pct":40}})");
        CHECK(c.is_counter());
        CHECK_FALSE(c.is_aggregated());
        CHECK(c.is_system());

        auto a = parse_line(
            p,
            R"({"name":"openat","cat":"POSIX","type":3,"ph":3,"ts":1,"args":{"dftu_cnt":2,"dur_min":379,"dur_max":444,"dur_sum":1232}})");
        CHECK(a.is_aggregated());
        CHECK_FALSE(a.is_counter());
        CHECK(a.args["dftu_cnt"].get<std::uint64_t>() == 2);
        CHECK(a.args["dur_sum"].get<std::uint64_t>() == 1232);
    }

    TEST_CASE("legacy string-ph traces still parse (backward compatible)") {
        simdjson::dom::parser p;
        auto x = parse_line(
            p,
            R"({"name":"read","cat":"POSIX","ph":"X","pid":1,"tid":1,"ts":1,"dur":2,"args":{}})");
        CHECK(x.is_complete());
        CHECK(x.type == EventType::Unknown);  // legacy: no type column

        auto c = parse_line(
            p, R"({"name":"cpu","cat":"sys","ph":"C","ts":1,"args":{}})");
        CHECK(c.is_counter());

        auto m = parse_line(
            p, R"({"name":"FH","ph":"M","args":{"hhash":"h","value":"v"}})");
        CHECK(m.is_metadata());
    }

    TEST_CASE("nested args flatten to dotted keys at arbitrary depth") {
        simdjson::dom::parser p;
        auto ev = parse_line(
            p,
            R"({"name":"end","cat":"dftracer","type":1,"ph":1,"ts":1,"args":{"num_events":9,"used":{"MPI":1,"LIBC_IO":1},"app":{"model":"resnet50","batch":32},"tags":[{"model":{"size":42}}]}})");
        CHECK(ev.args["num_events"].get<std::uint64_t>() == 9);
        CHECK(ev.args["used.MPI"].get<std::uint64_t>() == 1);
        CHECK(ev.args["used.LIBC_IO"].get<std::uint64_t>() == 1);
        CHECK(ev.args["app.model"].get<std::string_view>() == "resnet50");
        CHECK(ev.args["app.batch"].get<std::uint64_t>() == 32);
        // Array index then object descent: unbounded depth.
        CHECK(ev.args["tags.0.model.size"].get<std::uint64_t>() == 42);
    }
}

TEST_SUITE("json-path") {
    TEST_CASE("JsonValue::at resolves deep object and array-index paths") {
        simdjson::dom::parser p;
        std::string s =
            R"({"args":{"used":{"MPI":1},"tags":[{"model":{"size":42}},{"model":{"size":7}}]}})";
        std::string pad = s;
        pad.resize(s.size() + simdjson::SIMDJSON_PADDING);
        auto doc = p.parse(pad.data(), s.size(), false);
        JsonValue jv(doc.value());

        CHECK(jv.at("args.used.MPI").get<std::uint64_t>() == 1);
        CHECK(jv.at("args.tags.0.model.size").get<std::uint64_t>() == 42);
        CHECK(jv.at("args.tags.1.model.size").get<std::uint64_t>() == 7);
        CHECK_FALSE(jv.at("args.tags.9.model").exists());  // out of range
        CHECK_FALSE(jv.at("args.nope").exists());
    }
}
