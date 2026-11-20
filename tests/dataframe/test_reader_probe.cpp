// SPDX-License-Identifier: MIT
// One reader probe for every entry point: the built-in trace reader scores like
// a plugin file format, the highest score wins, a tie is refused with both
// names, and a name forces a reader.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/internal/file_format_registry.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstdio>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

// A format that scores every file the same and opens none.
class FixedScore final : public FileFormatFactory {
   public:
    FixedScore(dstd::String name, int score)
        : name_(std::move(name)), score_(score) {}

    dstd::StringView name() const override { return name_; }
    dstd::StringView version() const override { return {}; }
    int probe(const std::uint8_t*, std::uint64_t,
              dstd::StringView) const override {
        return score_;
    }
    dstd::Result<dstd::SharedPtr<const FormatFileSource>> open(
        dstd::StringView, dstd::StringView) override {
        return dstd::SharedPtr<const FormatFileSource>();
    }

   private:
    dstd::String name_;
    int score_;
};

// Registers a FixedScore for the life of the object.
struct Registered {
    dstd::String name;
    Registered(dstd::String n, int score) : name(n) {
        REQUIRE(register_file_format(
                    dstd::make_shared<FixedScore>(std::move(n), score))
                    .has_value());
    }
    ~Registered() { unregister_file_format(name); }
};

dstd::String write_text(dstd::StringView name, dstd::StringView text) {
    const dstd::Path dir =
        dftu_utils_test::make_unique_test_path("reader_probe");
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String path = dstd::String((dir / dstd::PathView(name)).str());
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    REQUIRE(std::fwrite(text.data(), 1, text.size(), f) == text.size());
    std::fclose(f);
    return path;
}

const char* const JSONL =
    "[\n{\"id\":1,\"name\":\"a\",\"ph\":1},\n{\"id\":2,\"name\":\"b\"}\n";

dstd::String claimed_name(dstd::StringView path, dstd::StringView forced = {}) {
    auto reader = claim_reader(path, forced);
    REQUIRE(reader.has_value());
    return reader->name;
}

dstd::String claim_error(dstd::StringView path, dstd::StringView forced = {}) {
    auto reader = claim_reader(path, forced);
    REQUIRE(!reader.has_value());
    return dstd::String(reader.error().c_str());
}

}  // namespace

TEST_CASE("bulk readers pick the built-in reader without reading the file") {
    auto reader = claim_trace_reader("/no/such/file.pfw.gz");
    REQUIRE(reader.has_value());
    CHECK(reader->is_builtin());
    Registered weak("probe.weak", 5);
    CHECK(!claim_trace_reader("/no/such/file.pfw.gz").has_value());
}

TEST_CASE("one file is probed even with no plugin format registered") {
    const dstd::String other = write_text("h.csv", "id,name\n1,a\n");
    CHECK(claim_error(other).find("no format claims") != dstd::String::npos);
    CHECK(claimed_name(write_text("i.pfw", JSONL)) == BUILTIN_READER);
}

TEST_CASE("the built-in reader claims JSON lines, gzip and an empty file") {
    Registered weak("probe.weak", 5);
    const dstd::String text = write_text("a.pfw", JSONL);
    const dstd::String gz = write_text("b.pfw.gz", "\x1f\x8b\x08\x00 not read");
    const dstd::String empty = write_text("c.pfw", "");
    CHECK(claimed_name(text) == BUILTIN_READER);
    CHECK(claimed_name(gz) == BUILTIN_READER);
    CHECK(claimed_name(empty) == BUILTIN_READER);
}

TEST_CASE("a file that is not JSON lines goes to a format that claims it") {
    Registered weak("probe.weak", 5);
    const dstd::String other = write_text("d.csv", "id,name\n1,a\n");
    CHECK(claimed_name(other) == "probe.weak");
}

TEST_CASE("a file no reader claims is an error naming the path") {
    Registered none("probe.none", 0);
    const dstd::String other = write_text("e.csv", "id,name\n1,a\n");
    const dstd::String message = claim_error(other);
    CHECK(message.find("no format claims") != dstd::String::npos);
    CHECK(message.find(other.c_str()) != dstd::String::npos);
}

TEST_CASE("the highest score wins") {
    Registered high("probe.high", 90);
    Registered low("probe.low", 20);
    const dstd::String text = write_text("f.pfw", JSONL);
    CHECK(claimed_name(text) == "probe.high");
}

TEST_CASE("a tie with the built-in reader is refused with both names") {
    Registered rival("probe.rival", 50);
    const dstd::String text = write_text("g.pfw", JSONL);
    const dstd::String message = claim_error(text);
    CHECK(message.find("jsonl") != dstd::String::npos);
    CHECK(message.find("probe.rival") != dstd::String::npos);
    CHECK(message.find(text.c_str()) != dstd::String::npos);
}

TEST_CASE("a name forces a reader and nothing is probed") {
    Registered high("probe.high", 90);
    CHECK(claimed_name("/no/such/file", "jsonl") == BUILTIN_READER);
    CHECK(claimed_name("/no/such/file", "probe.high") == "probe.high");
    auto forced = claim_reader("/no/such/file", "jsonl");
    REQUIRE(forced.has_value());
    CHECK(forced->is_builtin());
}

TEST_CASE("an unknown name lists the known readers") {
    Registered high("probe.high", 90);
    const dstd::String message = claim_error("/x", "nope");
    CHECK(message.find("unknown format 'nope'") != dstd::String::npos);
    CHECK(message.find("jsonl, probe.high") != dstd::String::npos);
}

TEST_CASE(
    "a file that cannot be read is an error when formats are registered") {
    Registered weak("probe.weak", 5);
    const dstd::String message = claim_error("/no/such/dir/file.pfw");
    CHECK(message.find("cannot open") != dstd::String::npos);
}
