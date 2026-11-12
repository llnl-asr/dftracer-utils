// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/yaml/document.h>
#include <dftracer/utils/yaml/records.h>
#include <dftracer/utils/yaml/writer.h>
#include <doctest/doctest.h>
#include <yaml/yaml_gen.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace dftracer::utils;
using dftu_yaml_test::Gen;
using dftu_yaml_test::matches;
using dftu_yaml_test::mutate;
using dftu_yaml_test::Rng;
using dftu_yaml_test::Tree;
using yaml::Document;
using yaml::Records;
using yaml::RecordsOptions;
using yaml::Value;

namespace {

std::size_t seed_count() {
    if (const char* more = std::getenv("YAML_FUZZ_SEEDS"))
        return static_cast<std::size_t>(std::strtoull(more, nullptr, 10));
    return 3000;
}

// The text of the records that a text of a generated tree has.
void expected_records(const dstd::String& text, const Tree& tree,
                      dstd::Vector<const Tree*>& out) {
    dstd::StringView rest = text;
    if (rest.substr(0, 3) == "\xEF\xBB\xBF") rest.remove_prefix(3);
    while (!rest.empty()) {
        const std::size_t end = rest.find('\n');
        const dstd::StringView line = rest.substr(0, end);
        rest = end == dstd::StringView::npos ? dstd::StringView{}
                                             : rest.substr(end + 1);
        if (line.find_first_not_of(" \t\r") == dstd::StringView::npos ||
            line[line.find_first_not_of(" \t\r")] == '#')
            continue;
        if (tree.kind == Tree::Kind::Seq && line[0] == '-' &&
            (line.size() == 1 || line[1] == ' ') &&
            line.substr(0, 3) != "---") {
            for (const Tree& item : tree.items) out.push_back(&item);
            return;
        }
        break;
    }
    out.push_back(&tree);
}

// Walks every value, so that a broken index shows as a crash or a report of a
// sanitizer.
std::size_t touch(Value value, std::size_t& misses, int depth = 0) {
    std::size_t n = 1;
    if (depth > 300) return n;
    if (value.is_object()) {
        value.for_each_member([&](dstd::StringView key, Value member) {
            n += key.size() + touch(member, misses, depth + 1);
            if (!value.find(key)) ++misses;
        });
    } else if (value.is_array()) {
        value.for_each_element(
            [&](Value element) { n += touch(element, misses, depth + 1); });
    } else if (value.is_string()) {
        n += value.raw().size();
        n += value.as_int() ? 1 : 0;
        n += value.as_double() ? 1 : 0;
        n += value.as_bool() ? 1 : 0;
    }
    return n;
}

// Whether `message` is `name:line:col: reason`.
bool is_well_formed(dstd::StringView message, dstd::StringView name,
                    std::size_t line_count) {
    if (message.substr(0, name.size()) != name) return false;
    message.remove_prefix(name.size());
    if (message.empty() || message[0] != ':') return false;
    message.remove_prefix(1);
    std::size_t numbers = 0;
    std::size_t line = 0;
    while (numbers < 2) {
        std::size_t digits = 0;
        std::size_t value = 0;
        while (!message.empty() && message[0] >= '0' && message[0] <= '9') {
            value = value * 10 + static_cast<std::size_t>(message[0] - '0');
            message.remove_prefix(1);
            ++digits;
        }
        if (digits == 0 || value == 0) return false;
        if (numbers == 0) line = value;
        ++numbers;
        if (message.empty() || message[0] != ':') return false;
        message.remove_prefix(1);
    }
    return line <= line_count + 1 && message.size() > 2 && message[0] == ' ';
}

}  // namespace

TEST_CASE("yaml fuzz: a generated text reads as its tree") {
    const std::size_t seeds = seed_count();
    std::size_t failures = 0;
    std::size_t compared = 0;
    Gen::Stats forms;
    for (std::size_t seed = 1; seed <= seeds && failures < 5; ++seed) {
        Gen gen(seed);
        const Tree tree = gen.tree();
        const dstd::String text = gen.text(tree);
        forms.add(gen.stats());
        auto doc = Document::parse(text, "fuzz");
        if (!doc) {
            ++failures;
            std::fprintf(stderr, "seed %zu: %s\n---\n%s\n---\n", seed,
                         dstd::String(doc.error().message()).c_str(),
                         text.c_str());
            continue;
        }
        dstd::String why;
        ++compared;
        if (!matches(tree, doc->root(), why)) {
            ++failures;
            std::fprintf(stderr, "seed %zu: %s\n---\n%s\n---\n", seed,
                         why.c_str(), text.c_str());
        }
    }
    CHECK(failures == 0);
    CHECK(compared > 0);
    MESSAGE("trees compared ", compared, ", flow collections ",
            forms.flow_collections, ", flow lines broken ",
            forms.flow_multi_line, ", block scalars ", forms.block_scalars,
            ", multi-line plain ", forms.multi_line_plain,
            ", multi-line quoted ", forms.multi_line_quoted, ", gap scalars ",
            forms.gap_scalars, ", gap flows ", forms.gap_flows,
            ", gap block scalars ", forms.gap_block_scalars, ", gap lines ",
            forms.gap_lines, ", texts with no final line feed ",
            forms.unterminated_texts);
    if (seeds >= 1000) {
        CHECK(forms.flow_collections > 0);
        CHECK(forms.flow_multi_line > 0);
        CHECK(forms.block_scalars > 0);
        CHECK(forms.multi_line_plain > 0);
        CHECK(forms.multi_line_quoted > 0);
        CHECK(forms.gap_scalars > 0);
        CHECK(forms.gap_flows > 0);
        CHECK(forms.gap_block_scalars > 0);
        CHECK(forms.unterminated_texts > 0);
    }
}

TEST_CASE("yaml fuzz: records of a generated text, cut in small chunks") {
    const std::size_t seeds = seed_count();
    std::size_t failures = 0;
    for (std::size_t seed = 1; seed <= seeds && failures < 5; ++seed) {
        Gen gen(seed);
        const Tree tree = gen.tree();
        const dstd::String text = gen.text(tree);
        dstd::Vector<const Tree*> expected;
        expected_records(text, tree, expected);
        RecordsOptions options;
        options.chunk_bytes = 1 + seed % 40;
        Records records = Records::from_text(text, "fuzz", options);
        std::size_t index = 0;
        dstd::String why;
        bool ok = true;
        for (;;) {
            auto next = records.next();
            if (!next) {
                ok = false;
                why = dstd::String(next.error().message());
                break;
            }
            if (!next->has_value()) break;
            if (index >= expected.size()) {
                ok = false;
                why = "more records than expected";
                break;
            }
            if (!matches(*expected[index], **next, why)) {
                ok = false;
                break;
            }
            ++index;
        }
        if (ok && index != expected.size()) {
            ok = false;
            why = "fewer records than expected";
        }
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "records seed %zu: %s\n---\n%s\n---\n", seed,
                         why.c_str(), text.c_str());
        }
    }
    CHECK(failures == 0);
}

TEST_CASE("yaml fuzz: a mutated text is a value or a well formed error") {
    const std::size_t seeds = seed_count();
    std::size_t failures = 0;
    std::size_t accepted = 0;
    std::size_t refused = 0;
    std::size_t sink = 0;
    std::size_t misses = 0;
    for (std::size_t seed = 1; seed <= seeds && failures < 5; ++seed) {
        Gen gen(seed);
        const Tree tree = gen.tree();
        Rng rng{seed * 7919 + 13};
        const dstd::String text = mutate(gen.text(tree), rng);
        std::size_t lines = 1;
        for (char c : text) lines += c == '\n' ? 1 : 0;
        auto doc = Document::parse(text, "fuzz");
        if (doc) {
            ++accepted;
            sink += touch(doc->root(), misses);
        } else {
            ++refused;
            if (!is_well_formed(doc.error().message(), "fuzz", lines) ||
                !doc.error().is_code(common::ErrorCode::InvalidArgument)) {
                ++failures;
                std::fprintf(stderr, "seed %zu: a malformed error '%s'\n", seed,
                             dstd::String(doc.error().message()).c_str());
            }
        }
        RecordsOptions options;
        options.chunk_bytes = 1 + seed % 50;
        Records records = Records::from_text(text, "fuzz", options);
        for (;;) {
            auto next = records.next();
            if (!next) {
                if (!is_well_formed(next.error().message(), "fuzz",
                                    lines + 1) ||
                    !next.error().is_code(common::ErrorCode::InvalidArgument)) {
                    ++failures;
                    std::fprintf(
                        stderr, "seed %zu: a malformed records error '%s'\n",
                        seed, dstd::String(next.error().message()).c_str());
                }
                break;
            }
            if (!next->has_value()) break;
            sink += touch(**next, misses);
        }
    }
    CHECK(failures == 0);
    CHECK(misses == 0);
    MESSAGE("accepted ", accepted, ", refused ", refused, ", sink ", sink);
}

TEST_CASE("yaml fuzz: the block writer's text reads as its tree") {
    const std::size_t seeds = seed_count();
    std::size_t failures = 0;
    for (std::size_t seed = 1; seed <= seeds && failures < 5; ++seed) {
        Gen gen(seed);
        const Tree tree = gen.tree();
        dstd::String text;
        yaml::Writer writer(text);
        struct Emit {
            yaml::Writer& w;
            void operator()(const Tree& t) const {
                switch (t.kind) {
                    case Tree::Kind::Map:
                        w.begin_object();
                        for (std::size_t i = 0; i < t.items.size(); ++i) {
                            w.key(t.keys[i]);
                            (*this)(t.items[i]);
                        }
                        w.end_object();
                        break;
                    case Tree::Kind::Seq:
                        w.begin_array();
                        for (const Tree& item : t.items) (*this)(item);
                        w.end_array();
                        break;
                    case Tree::Kind::String:
                        w.value(dstd::StringView(t.text));
                        break;
                    default:
                        w.null();
                        break;
                }
            }
        };
        Emit{writer}(tree);
        text += '\n';
        auto doc = Document::parse(text, "fuzz");
        dstd::String why;
        if (!doc) {
            ++failures;
            why = dstd::String(doc.error().message());
        } else if (!matches(tree, doc->root(), why)) {
            ++failures;
        } else {
            continue;
        }
        std::fprintf(stderr, "writer seed %zu: %s\n---\n%s\n---\n", seed,
                     why.c_str(), text.c_str());
    }
    CHECK(failures == 0);
}
