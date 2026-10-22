#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/trace/parse_inflated.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using dftracer::utils::trace::EventRecord;
using dftracer::utils::trace::parse_buffer;

namespace {

// parse_buffer needs the buffer to carry simdjson's trailing padding.
std::shared_ptr<std::string> padded(const std::string& s) {
    auto b = std::make_shared<std::string>(s);
    b->resize(s.size() + simdjson::SIMDJSON_PADDING, '\0');
    return b;
}

// Parse one buffer and return the event names, plus the trailing byte count the
// caller would carry into the next chunk.
std::vector<std::string> parse_names(const std::string& data,
                                     std::size_t& carry) {
    simdjson::dom::parser parser;
    auto buf = padded(data);
    std::size_t line_no = 0;
    std::vector<std::string> names;
    carry = parse_buffer(
        parser, buf, data.size(), 0, line_no,
        [&](const EventRecord& r) { names.emplace_back(r.ev.name); });
    return names;
}

std::string event(const std::string& name) {
    return R"({"ph":"X","name":")" + name +
           R"(","cat":"POSIX","pid":1,"tid":1,"ts":1,"dur":1})";
}

bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

TEST_SUITE("parse_buffer") {
    TEST_CASE("parses every record in a clean buffer") {
        std::string data;
        for (int i = 0; i < 5; ++i)
            data += event("e" + std::to_string(i)) + "\n";
        std::size_t carry = 0;
        auto names = parse_names(data, carry);
        CHECK(names.size() == 5);
        CHECK(carry == 0);
    }

    // A record with a raw newline and unescaped quotes inside a string (an SH
    // record capturing a shell command is the real-world case) desyncs
    // parse_many over the whole batch. The neighbours must still be recovered.
    TEST_CASE("a malformed record drops only itself") {
        std::string data =
            event("before") + "\n" +
            R"({"ph":4,"name":"SH","args":{"name":"bash -c "echo )" + "\n" +
            R"(hi"","value":"h1"}})" + "\n" + event("after") + "\n";
        std::size_t carry = 0;
        auto names = parse_names(data, carry);
        CHECK(has(names, "before"));
        CHECK(has(names, "after"));
    }

    // A buffer cut mid-record (no closing newline) is carried whole into the
    // next chunk, where the completed record parses.
    TEST_CASE("a record split across chunks is carried and recovered") {
        std::string full = event("straddle") + "\n";
        std::size_t cut = full.size() - 15;  // mid-record, before any newline
        std::string first = full.substr(0, cut);

        std::size_t carry = 0;
        auto n1 = parse_names(first, carry);
        CHECK(n1.empty());
        CHECK(carry == first.size());  // no complete line: carry it all

        std::string second = first + full.substr(cut);
        auto n2 = parse_names(second, carry);
        CHECK(has(n2, "straddle"));
    }

    // A boundary that lands inside a string leaves a trailing partial line;
    // only the complete records are parsed and the remainder is reported to
    // carry.
    TEST_CASE("a trailing partial line is not parsed but is carried") {
        std::string data = event("complete") + "\n" +
                           R"({"ph":"X","name":"partial","cat":"POS)";  // no \n
        std::size_t carry = 0;
        auto names = parse_names(data, carry);
        CHECK(has(names, "complete"));
        CHECK_FALSE(has(names, "partial"));
        CHECK(carry == data.size() - (event("complete").size() + 1));
    }
}
