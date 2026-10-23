#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/trace/parse_inflated.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using dftracer::utils::trace::EventRecord;
using dftracer::utils::trace::PARSE_BATCH_BYTES;
using dftracer::utils::trace::parse_buffer;
using dftracer::utils::trace::strip_array_delimiters;

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

std::string long_event(const std::string& name, std::size_t line_bytes) {
    const std::string head = R"({"ph":"X","name":")" + name +
                             R"(","cat":"POSIX","pid":1,"tid":1,"ts":1,)" +
                             R"("dur":1,"args":{"blob":")";
    const std::string tail = R"("}})";
    return head + std::string(line_bytes - head.size() - tail.size(), 'x') +
           tail;
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

    // A buffer of many batches is read batch by batch, so the parser holds
    // one batch whatever the buffer size.
    TEST_CASE("a large buffer grows the parser by one batch, not the buffer") {
        std::string data;
        std::size_t count = 0;
        while (data.size() < 8 * PARSE_BATCH_BYTES)
            data += event("e" + std::to_string(count++)) + "\n";
        simdjson::dom::parser parser;
        auto buf = padded(data);
        std::size_t line_no = 0;
        std::size_t seen = 0;
        bool in_order = true;
        parse_buffer(parser, buf, data.size(), 0, line_no,
                     [&](const EventRecord& r) {
                         in_order &= r.ev.name == "e" + std::to_string(seen);
                         ++seen;
                     });
        CHECK(seen == count);
        CHECK(in_order);
        CHECK(parser.capacity() <= 2 * PARSE_BATCH_BYTES);
    }

    // A record longer than a batch is parsed alone; the records on both
    // sides of it still come through in order.
    TEST_CASE("records longer than a batch do not drop their neighbours") {
        const std::string blob(2 * PARSE_BATCH_BYTES, 'x');
        std::string data;
        std::vector<std::string> want;
        for (int round = 0; round < 3; ++round) {
            for (int i = 0; i < 2000; ++i) {
                want.push_back("r" + std::to_string(round) + "_" +
                               std::to_string(i));
                data += event(want.back()) + "\n";
            }
            want.push_back("long" + std::to_string(round));
            data += R"({"ph":"X","name":"long)" + std::to_string(round) +
                    R"(","cat":"POSIX","pid":1,"tid":1,"ts":1,"dur":1,)" +
                    R"("args":{"blob":")" + blob + R"("}})" + "\n";
        }
        want.push_back("last");
        data += event("last") + "\n";
        std::size_t carry = 0;
        const auto names = parse_names(data, carry);
        CHECK(carry == 0);
        CHECK(names == want);
    }

    TEST_CASE("a bad record in the third batch drops only itself") {
        std::string data;
        std::vector<std::string> want;
        std::size_t count = 0;
        const auto add = [&] {
            want.push_back("e" + std::to_string(count++));
            data += event(want.back()) + "\n";
        };
        while (data.size() < 2 * PARSE_BATCH_BYTES + PARSE_BATCH_BYTES / 2)
            add();
        data += std::string(
                    R"({"ph":4,"name":"SH","args":{"name":"bash -c "echo )") +
                "\n" + R"(hi"","value":"h1"}})" + "\n";
        while (data.size() < 5 * PARSE_BATCH_BYTES) add();
        std::size_t carry = 0;
        const auto names = parse_names(data, carry);
        CHECK(carry == 0);
        CHECK(names == want);
    }

    TEST_CASE("CRLF line ends parse and the carry counts the bytes after \\n") {
        std::string data = "[\r\n";
        std::vector<std::string> want;
        for (int i = 0; i < 50; ++i) {
            want.push_back("e" + std::to_string(i));
            data += event(want.back()) + "\r\n";
        }
        data += "]\r\n";
        const std::string partial = R"({"ph":"X","name":"cut","ca)";
        data += partial;
        strip_array_delimiters(data.data(), data.size());
        std::size_t carry = 0;
        const auto names = parse_names(data, carry);
        CHECK(names == want);
        CHECK(carry == partial.size());
    }

    TEST_CASE("lines around the batch size keep their neighbours in order") {
        for (const std::size_t bytes :
             {PARSE_BATCH_BYTES / 2, PARSE_BATCH_BYTES / 2 + 1,
              PARSE_BATCH_BYTES - 1, PARSE_BATCH_BYTES,
              PARSE_BATCH_BYTES + 1}) {
            INFO("line bytes: ", bytes);
            std::string data;
            std::vector<std::string> want;
            for (int i = 0; i < 1500; ++i) {
                want.push_back("a" + std::to_string(i));
                data += event(want.back()) + "\n";
            }
            want.push_back("exact");
            data += long_event("exact", bytes) + "\n";
            for (int i = 0; i < 1500; ++i) {
                want.push_back("b" + std::to_string(i));
                data += event(want.back()) + "\n";
            }
            std::size_t carry = 0;
            const auto names = parse_names(data, carry);
            CHECK(carry == 0);
            CHECK(names == want);
        }
    }

    TEST_CASE("a long line that is itself bad drops only itself") {
        std::string bad = long_event("bad", 2 * PARSE_BATCH_BYTES);
        bad.erase(bad.size() - 3);
        std::string data;
        std::vector<std::string> want;
        for (int i = 0; i < 2500; ++i) {
            want.push_back("a" + std::to_string(i));
            data += event(want.back()) + "\n";
        }
        data += bad + "\n";
        for (int i = 0; i < 2500; ++i) {
            want.push_back("b" + std::to_string(i));
            data += event(want.back()) + "\n";
        }
        std::size_t carry = 0;
        const auto names = parse_names(data, carry);
        CHECK(carry == 0);
        CHECK(names == want);
    }
}
