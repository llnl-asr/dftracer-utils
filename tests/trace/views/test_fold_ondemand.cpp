// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/intern_cache.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/value_access.h>
#include <doctest/doctest.h>
#include <duql/filter_corpus.h>
#include <trace/views/fold_shapes.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace fold_shapes;

namespace {

using dftracer::utils::trace::views::detail::decode_record_into;
using dftracer::utils::trace::views::detail::DecodeHints;
using dftracer::utils::trace::views::detail::extract_fold_event_into;
using json::detail::ValueAccess;

String describe_reset() {
    StringIntern intern;
    return describe(FoldEvent{}, intern);
}

struct OnDemandRead {
    String text;
    bool parsed = false;
    bool broken = false;
};

OnDemandRead read_on_demand(json::Document& doc, StringView line,
                            bool needs_args, bool with_cache = false) {
    OnDemandRead out;
    auto value = doc.parse(line);
    if (!value) return out;
    out.parsed = true;
    StringIntern intern;
    dstd::InternCache cache(intern);
    FoldEvent ev;
    extract_fold_event_into(ev, *value, intern, needs_args,
                            with_cache ? &cache : nullptr);
    out.broken = ValueAccess::broken(*value);
    out.text = describe(ev, intern);
    return out;
}

// The two readers agree on `line`. A line the DOM parser rejects is one the
// on-demand reader drops as broken when it reaches the bad part, which the
// walk with args always does; without args, a bad part inside args is not
// reached.
void check_line(json::Document& doc, StringView line, const char* expected) {
    CAPTURE(line);
    for (const bool needs_args : {true, false}) {
        for (const bool with_cache : {false, true}) {
            CAPTURE(needs_args);
            CAPTURE(with_cache);
            StringIntern dom_intern;
            FoldEvent dom_ev;
            const bool dom_ok = read_dom(line, dom_ev, dom_intern, needs_args);
            const OnDemandRead got =
                read_on_demand(doc, line, needs_args, with_cache);
            if (!got.parsed) {
                CHECK_FALSE(dom_ok);
                continue;
            }
            if (dom_ok) {
                CHECK_FALSE(got.broken);
                CHECK(got.text == describe(dom_ev, dom_intern));
                if (needs_args && expected) CHECK(got.text == expected);
            } else {
                if (needs_args) CHECK(got.broken);
                if (got.broken) CHECK(got.text == describe_reset());
            }
        }
    }
}

std::uint64_t next_random(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

// Lines shaped like the ones dftracer_gen_fake_trace writes, with the extra
// args a real trace carries.
dstd::Vector<String> generated_lines(std::size_t count) {
    static const char* const NAMES[] = {"read",  "write",    "open64",
                                        "fsync", "H5Dwrite", "MPI_Send"};
    static const char* const CATS[] = {"POSIX", "STDIO", "MPI", "HDF5"};
    static const char* const EXTRAS[] = {
        R"("fname":"/p/lustre/data/a.h5","size":4096)",
        R"("ret":-1,"errno":2)",
        R"("offset":9223372036854775807,"flags":[1,2,3])",
        R"("io":{"off":1.5,"len":[1,{"k":"v"}]},"e":{},"f":[],"n":null)",
        R"("msg":"a\"bé","ok":true)",
        R"("big":18446744073709551615,"huge":123456789012345678901234567890)",
        R"("a.b":1,"a":{"b":2})",
    };
    dstd::Vector<String> lines;
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t r = next_random(state);
        String line = common::str_cat(
            R"({"id":)", i, R"(,"name":")", NAMES[r % 6], R"(","cat":")",
            CATS[(r >> 8) % 4], R"(","pid":)", 1000 + (r >> 16) % 8,
            R"(,"tid":)", 10000 + (r >> 20) % 64, R"(,"ts":)",
            1000000000 + i * 100, R"(,"dur":)", (r >> 24) % 5000, R"(,"ph":)",
            1 + (r >> 40) % 3, R"(,"type":)", 1 + (r >> 44) % 9,
            R"(,"args":{"hhash":"h)", (r >> 48) % 4, R"(","level":)",
            (r >> 52) % 4);
        if ((r >> 56) & 1)
            line += common::str_cat(R"(,"fhash":"f)", r % 16, "\"");
        if ((r >> 57) & 1) line += common::str_cat(",", EXTRAS[(r >> 58) % 7]);
        if ((r >> 61) & 1) line += common::str_cat(",", EXTRAS[(r >> 3) % 7]);
        line += "}}";
        lines.push_back(std::move(line));
    }
    return lines;
}

String describe_path_reset() {
    StringIntern intern;
    return describe_path(FoldEvent{}, intern);
}

// The on-demand path reader on `line` with `o`. `parsed` is false when the
// document refuses the line.
struct PathRead {
    String text;
    bool parsed = false;
    bool broken = false;
};

PathRead read_path_on_demand(json::Document& doc, StringView line,
                             const PathOptions& o, FoldEvent& ev,
                             StringIntern& intern, DecodeHints& hints,
                             dstd::InternCache* cache) {
    PathRead out;
    auto value = doc.parse(line);
    if (!value) return out;
    out.parsed = true;
    const auto* schema = o.typed ? &path_schema() : nullptr;
    dstd::Vector<const index::FieldSpec*> fields;
    for (const String& path : o.paths)
        fields.push_back(schema ? schema->field_at(path) : nullptr);
    decode_record_into(ev, *value, intern, o.capture_schema,
                       o.paths.empty() ? nullptr : &o.paths, schema,
                       o.paths.empty() || !schema ? nullptr : &fields,
                       o.max_children, o.with_hints ? &hints : nullptr,
                       o.skip_never, cache);
    out.broken = ValueAccess::broken(*value);
    out.text = describe_path(ev, intern);
    return out;
}

// `ev` as the path readers write it, cut to the keys at or under `paths` when
// there are any. The count of unconverted values is left out then, since the
// reference counts the ones outside `paths` too.
String describe_under_paths(const FoldEvent& ev, const StringIntern& intern,
                            const dstd::Vector<String>& paths) {
    if (paths.empty()) return describe_path(ev, intern);
    const auto wanted = [&](std::uint32_t id) {
        const StringView key = intern.resolve(id);
        for (const String& path : paths) {
            if (key == StringView(path)) return true;
            if (key.size() > path.size() && key[path.size()] == '.' &&
                key.substr(0, path.size()) == StringView(path))
                return true;
        }
        return false;
    };
    FoldEvent cut;
    for (const auto& a : ev.args)
        if (wanted(a.first)) cut.args.push_back(a);
    for (const auto& a : ev.top_fields)
        if (wanted(a.first)) cut.top_fields.push_back(a);
    for (const auto& sp : ev.specials)
        if (wanted(sp.key)) cut.specials.push_back(sp);
    for (const auto& leaf : ev.schema_leaves)
        if (wanted(leaf.first)) cut.schema_leaves.push_back(leaf);
    cut.by_path = ev.by_path;
    return describe_path(cut, intern);
}

// The two path readers agree on every line, for every way to call them, with
// and without an intern cache. Hints and the intern table live across the
// lines of a run, as in a scan. A line the DOM parser rejects is not compared:
// the explicit shapes below pin what the on-demand reader does with those.
void check_path_lines(const dstd::Vector<StringView>& lines) {
    for (const PathOptions& o : path_option_sets()) {
        for (const bool with_cache : {false, true}) {
            CAPTURE(o.name);
            CAPTURE(with_cache);
            json::Document doc;
            StringIntern dom_intern;
            StringIntern od_intern;
            dstd::InternCache cache(od_intern);
            DecodeHints dom_hints;
            DecodeHints od_hints;
            FoldEvent dom_ev;
            FoldEvent od_ev;
            for (StringView line : lines) {
                CAPTURE(line);
                const bool dom_ok =
                    read_dom_path(line, dom_ev, dom_intern, o, dom_hints);
                const PathRead got = read_path_on_demand(
                    doc, line, o, od_ev, od_intern, od_hints,
                    with_cache ? &cache : nullptr);
                if (!got.parsed) {
                    CHECK_FALSE(dom_ok);
                    continue;
                }
                if (dom_ok) {
                    CHECK_FALSE(got.broken);
                    CHECK(got.text == describe_path(dom_ev, dom_intern));
                } else if (got.broken) {
                    CHECK(got.text == describe_path_reset());
                }
            }
        }
    }
}

dstd::Vector<StringView> views_of(const dstd::Vector<String>& lines) {
    dstd::Vector<StringView> out;
    for (const String& line : lines) out.emplace_back(line);
    return out;
}

dstd::Vector<String> capture_names(std::size_t from, std::size_t count) {
    dstd::Vector<String> names;
    const std::size_t all = sizeof(CAPTURE_NAMES) / sizeof(CAPTURE_NAMES[0]);
    for (std::size_t i = from; i < from + count && i < all; ++i)
        names.emplace_back(CAPTURE_NAMES[i]);
    return names;
}

// The captures of the DOM form and the on-demand form agree on `line` for the
// given names, with and without args.
void check_captures(json::Document& doc, StringView line,
                    const dstd::Vector<String>& names) {
    for (const bool needs_args : {true, false}) {
        for (const bool with_cache : {false, true}) {
            CAPTURE(line);
            CAPTURE(needs_args);
            CAPTURE(with_cache);
            StringIntern dom_intern;
            FoldEvent dom_ev;
            const bool dom_ok =
                read_dom_captures(line, dom_ev, dom_intern, needs_args, names);
            auto value = doc.parse(line);
            if (!value) {
                CHECK_FALSE(dom_ok);
                continue;
            }
            StringIntern intern;
            dstd::InternCache cache(intern);
            FoldEvent ev;
            extract_fold_event_into(ev, *value, intern, needs_args,
                                    with_cache ? &cache : nullptr, &names);
            const bool broken = ValueAccess::broken(*value);
            if (dom_ok) {
                CHECK_FALSE(broken);
                CHECK(describe(ev, intern) == describe(dom_ev, dom_intern));
            } else if (broken) {
                CHECK(describe(ev, intern) == describe_reset());
            }
        }
    }
}

}  // namespace

TEST_SUITE("FoldOnDemand") {
    TEST_CASE("the on-demand reader equals the DOM reader on every shape") {
        json::Document doc;
        for (const Shape& shape : SHAPES) {
            CAPTURE(shape.name);
            check_line(doc, shape.line, shape.expected);
        }
    }

    TEST_CASE("the on-demand reader equals the DOM reader on the corpus") {
        json::Document doc;
        for (const char* line : EVENTS) check_line(doc, line, nullptr);
    }

    TEST_CASE("whitespace after a value matches the DOM reader") {
        json::Document doc;
        const char* const lines[] = {
            "{\"ph\":\"X\"  ,\"name\":\"a\" ,\"cat\" : \"b\"\t,\"ts\":1 "
            ",\"dur\":2\n}",
            "{ \"ph\" : 1 , \"args\" : { \"s\" : \"v\" , \"fhash\" : "
            "\"f\"\r\n, \"n\" : 3 } }",
            "{\"ph\":\"X\"   }",
            "{\"ph\":1,\"args\":{\"esc\":\"a\\\"b\"  ,\"tail\":\"c\\\\\" }}",
        };
        for (const char* line : lines) check_line(doc, line, nullptr);
    }

    TEST_CASE("the on-demand reader equals the DOM reader on trace lines") {
        json::Document doc;
        for (const String& line : generated_lines(3000))
            check_line(doc, line, nullptr);
    }

    TEST_CASE("a part that is not reached is not checked") {
        json::Document doc;
        // The DOM parser rejects these lines. The reader does not read `id`,
        // an atom in a field it only type-checks, or args when it wants none.
        const OnDemandRead id =
            read_on_demand(doc, R"({"ph":1,"id":tru,"ts":5})", true);
        CHECK(id.parsed);
        CHECK_FALSE(id.broken);
        CHECK(id.text.find("ts=5") != String::npos);
        const OnDemandRead args =
            read_on_demand(doc, R"({"ph":1,"args":{"a":01},"ts":5})", false);
        CHECK_FALSE(args.broken);
        CHECK(args.text.find("ts=5") != String::npos);
        const OnDemandRead atom =
            read_on_demand(doc, R"({"ph":1,"cat":nul,"ts":5})", true);
        CHECK_FALSE(atom.broken);
        CHECK(atom.text.find("ts=5") != String::npos);
        StringIntern intern;
        FoldEvent ev;
        CHECK_FALSE(read_dom(R"({"ph":1,"id":tru,"ts":5})", ev, intern, true));
        CHECK_FALSE(read_dom(R"({"ph":1,"cat":nul,"ts":5})", ev, intern, true));
        CHECK_FALSE(
            read_dom(R"({"ph":1,"args":{"a":01},"ts":5})", ev, intern, false));
    }

    TEST_CASE("a broken line leaves the event reset and the next line clean") {
        json::Document doc;
        auto bad = doc.parse(R"({"ph":1,"ts":5,"args":{"a":01}})");
        REQUIRE(bad.has_value());
        StringIntern intern;
        FoldEvent ev;
        extract_fold_event_into(ev, *bad, intern, true);
        CHECK(ValueAccess::broken(*bad));
        CHECK(describe(ev, intern) == describe_reset());

        auto good = doc.parse(R"({"ph":1,"ts":6,"args":{"a":2}})");
        REQUIRE(good.has_value());
        extract_fold_event_into(ev, *good, intern, true);
        CHECK_FALSE(ValueAccess::broken(*good));
        CHECK(describe(ev, intern) ==
              "ph=1 pid=0 tid=0 ts=6 dur=- cat=- name=- fh=- hh=- "
              "args{\"a\"=i2} top{} sp{}");
    }

    TEST_CASE("a value that is not an object reads as a reset event") {
        json::Document doc;
        auto list = doc.parse(R"([{"ph":1}])");
        REQUIRE(list.has_value());
        StringIntern intern;
        FoldEvent ev;
        extract_fold_event_into(ev, *list, intern, true);
        CHECK_FALSE(ValueAccess::broken(*list));
        CHECK(describe(ev, intern) == describe_reset());
    }

    TEST_CASE("the reader leaves the line readable for the next consumer") {
        json::Document doc;
        const char* line =
            R"({"name":"read","ph":1,"ts":7,"args":{"fhash":"f","a":2}})";
        auto value = doc.parse(line);
        REQUIRE(value.has_value());
        const duql::Query query =
            *duql::Query::from_string(R"(name == "read")");
        StringIntern intern;
        FoldEvent first;
        FoldEvent second;
        FoldEvent third;
        // As the branch driver runs a line: a predicate, then each consumer
        // from the start of the event.
        CHECK(query.matches(*value));
        ValueAccess::rewind(*value);
        extract_fold_event_into(first, *value, intern, true);
        ValueAccess::rewind(*value);
        extract_fold_event_into(second, *value, intern, true);
        CHECK(query.matches(*value));
        ValueAccess::rewind(*value);
        extract_fold_event_into(third, *value, intern, false);
        CHECK_FALSE(ValueAccess::broken(*value));
        CHECK(describe(first, intern) == describe(second, intern));
        CHECK(describe(first, intern) ==
              "ph=1 pid=0 tid=0 ts=7 dur=- cat=- name=\"read\" fh=\"f\" hh=- "
              "args{\"a\"=i2} top{} sp{}");
        CHECK(describe(third, intern) ==
              "ph=1 pid=0 tid=0 ts=7 dur=- cat=- name=\"read\" fh=\"f\" hh=- "
              "args{} top{} sp{}");
    }

    TEST_CASE("a reused event slot carries nothing from the last line") {
        json::Document doc;
        StringIntern intern;
        FoldEvent ev;
        auto first = doc.parse(
            R"({"ph":1,"name":"n","ts":9,"args":{"fhash":"f","a":1,"b":[]}})");
        REQUIRE(first.has_value());
        extract_fold_event_into(ev, *first, intern, true);
        auto second = doc.parse(R"({"ph":2,"args":{"c":3}})");
        REQUIRE(second.has_value());
        extract_fold_event_into(ev, *second, intern, true);
        CHECK(describe(ev, intern) ==
              "ph=2 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
              "args{\"c\"=i3} top{} sp{}");
    }

    TEST_CASE(
        "a number too large for a double and a string with an invalid "
        "escape keep the line") {
        // The DOM parser rejects each of these lines, so the expected event is
        // written out here.
        struct OddShape {
            const char* name;
            const char* line;
            const char* expected;
        };
        const OddShape shapes[] = {
            {"args_plus_overflow", R"j({"ph":1,"args":{"a":1e400,"x":1}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=dinf \"x\"=i1} top{} sp{}"},
            {"args_minus_overflow", R"j({"ph":1,"args":{"a":-1e400,"x":1}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=d-inf \"x\"=i1} top{} sp{}"},
            {"args_big_exponent", R"j({"ph":1,"args":{"a":1.5E+999,"x":1}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=dinf \"x\"=i1} top{} sp{}"},
            {"args_underflow_still_reads",
             R"j({"ph":1,"args":{"a":1e-400,"x":1}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=d0 \"x\"=i1} top{} sp{}"},
            {"args_overflow_nested",
             R"j({"ph":1,"args":{"l":[1,1e400,-1e400],"o":{"k":1e400}}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"l.0\"=i1 \"l.1\"=dinf \"l.2\"=d-inf \"o.k\"=dinf} top{} "
             "sp{}"},
            {"ts_overflow", R"j({"ph":1,"ts":1e400,"name":"n"})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} "
             "top{} sp{}"},
            {"dur_overflow", R"j({"ph":1,"dur":-1e400,"ts":5})j",
             "ph=1 pid=0 tid=0 ts=5 dur=- cat=- name=- fh=- hh=- args{} top{} "
             "sp{}"},
            {"ph_overflow", R"j({"ph":1e400,"ts":5})j",
             "ph=0 pid=0 tid=0 ts=5 dur=- cat=- name=- fh=- hh=- args{} top{} "
             "sp{}"},
            {"bad_escape_name", R"j({"ph":1,"name":"a\qb"})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"a\\\\qb\" fh=- hh=- "
             "args{} top{} sp{}"},
            {"bad_escape_cat", R"j({"ph":1,"cat":"\q"})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=\"\\\\q\" name=- fh=- hh=- "
             "args{} top{} sp{}"},
            {"bad_escape_arg", R"j({"ph":1,"args":{"a":"x\qy","b":2}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=s\"x\\\\qy\" \"b\"=i2} top{} sp{}"},
            {"lone_high_surrogate", R"j({"ph":1,"args":{"a":"\ud800"}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=s\"\\\\ud800\"} top{} sp{}"},
            {"lone_low_surrogate", R"j({"ph":1,"args":{"a":"\udc00"}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=s\"\\\\udc00\"} top{} sp{}"},
            {"short_unicode_escape", R"j({"ph":1,"args":{"a":"\u12"}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=s\"\\\\u12\"} top{} sp{}"},
            {"bad_escape_key", R"j({"ph":1,"args":{"k\q":1,"b":2}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"k\\\\q\"=i1 \"b\"=i2} top{} sp{}"},
            {"bad_escape_nested_key", R"j({"ph":1,"args":{"o":{"k\q":1}}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"o.k\\\\q\"=i1} top{} sp{}"},
            {"bad_escape_top_key", R"j({"k\q":1,"ph":1,"ts":5})j",
             "ph=1 pid=0 tid=0 ts=5 dur=- cat=- name=- fh=- hh=- args{} top{} "
             "sp{}"},
            {"bad_escape_fhash", R"j({"ph":1,"args":{"fhash":"a\qb"}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=\"a\\\\qb\" hh=- "
             "args{} top{} sp{}"},
            {"ts_above_uint64", R"j({"ph":1,"ts":18446744073709551616})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} "
             "sp{}"},
            {"ts_20_nines",
             R"j({"ph":1,"ts":99999999999999999999,"name":"n"})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} "
             "top{} sp{}"},
            {"args_bigint_20_over",
             R"j({"ph":1,"args":{"a":18446744073709551616,"x":1}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=s\"18446744073709551616\" \"x\"=i1} top{} sp{}"},
            {"args_bigint_20_nines",
             R"j({"ph":1,"args":{"a":99999999999999999999,"x":1}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=s\"99999999999999999999\" \"x\"=i1} top{} sp{}"},
            {"args_bigint_20_nested",
             R"j({"ph":1,"args":{"l":[18446744073709551616,1],"o":{"k":99999999999999999999}}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"l.0\"=s\"18446744073709551616\" \"l.1\"=i1 "
             "\"o.k\"=s\"99999999999999999999\"} top{} sp{}"},
            {"bad_escape_ph", R"j({"ph":"\q"})j",
             "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} "
             "sp{}"},
        };
        json::Document doc;
        for (const OddShape& shape : shapes) {
            INFO(String(shape.name) << " " << shape.line);
            StringIntern dom_intern;
            FoldEvent dom_ev;
            const bool dom_ok = read_dom(shape.line, dom_ev, dom_intern, true);
            if (StringView(shape.name) != "args_underflow_still_reads")
                CHECK_FALSE(dom_ok);
            for (const bool needs_args : {true, false}) {
                CAPTURE(needs_args);
                const OnDemandRead got =
                    read_on_demand(doc, shape.line, needs_args);
                REQUIRE(got.parsed);
                CHECK_FALSE(got.broken);
                if (needs_args ||
                    StringView(shape.line).find("\"args\"") == StringView::npos)
                    CHECK(got.text == shape.expected);
                const OnDemandRead next = read_on_demand(
                    doc, R"j({"ph":1,"ts":9,"name":"z"})j", needs_args);
                CHECK_FALSE(next.broken);
                CHECK(next.text ==
                      "ph=1 pid=0 tid=0 ts=9 dur=- cat=- name=\"z\" fh=- "
                      "hh=- args{} top{} sp{}");
            }
        }
    }

    TEST_CASE(
        "text that is not a number or a closed string is still an error") {
        json::Document doc;
        const char* const bad_args[] = {"tru", "01",  "1x",  "-",
                                        "1.",  "+1",  ".5",  "NaN",
                                        "1e",  "1e+", "0x1", "1e400x"};
        for (const char* token : bad_args) {
            INFO(String("token ") << token);
            const String line = common::str_cat(
                R"({"ph":1,"ts":5,"args":{"a":)", token, R"(,"b":2}})");
            const OnDemandRead got = read_on_demand(doc, line, true);
            CHECK_FALSE((got.parsed && !got.broken));
            // A field the reader only type-checks never looks at an atom that
            // starts like no number.
            if (token[0] < '0' || token[0] > '9') continue;
            const String top =
                common::str_cat(R"({"ph":1,"ts":)", token, R"(,"name":"n"})");
            const OnDemandRead top_got = read_on_demand(doc, top, true);
            CHECK_FALSE((top_got.parsed && !top_got.broken));
        }
        const char* const cropped[] = {
            R"j({"ph":1,"args":{"a":"x)j", R"j({"ph":1,"args":{"a":"x\q)j",
            R"j({"ph":1,"args":{"a":"x\)j", R"j({"ph":1,"args":{"a":)j",
            R"j({"ph":1,"args":{"k\q":)j"};
        for (const char* line : cropped) {
            INFO(String("line ") << line);
            const OnDemandRead got = read_on_demand(doc, line, true);
            CHECK_FALSE((got.parsed && !got.broken));
        }
    }

    TEST_CASE("bad UTF-8 and raw control bytes in a string keep the line") {
        struct Repaired {
            String line;
            const char* expected;
        };
        const Repaired shapes[] = {
            {String("{\"ph\":1,\"name\":\"a\xff"
                    "b\"}"),
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"a?b\" fh=- hh=- args{} "
             "top{} sp{}"},
            {String("{\"ph\":1,\"cat\":\"\xe2\x82\",\"ts\":5}"),
             "ph=1 pid=0 tid=0 ts=5 dur=- cat=\"??\" name=- fh=- hh=- args{} "
             "top{} sp{}"},
            {String("{\"ph\":1,\"args\":{\"a\":\"x\ty\",\"b\":2}}"),
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=s\"x?y\" \"b\"=i2} top{} sp{}"},
            {String("{\"ph\":1,\"args\":{\"k\tq\":1,\"b\":2}}"),
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"k?q\"=i1 \"b\"=i2} top{} sp{}"},
            {String("{\"ph\":1,\"args\":{\"fhash\":\"f\x01\"}}"),
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=\"f?\" hh=- args{} "
             "top{} sp{}"},
            {String("{\"\xff\":1,\"ph\":1,\"ts\":7}"),
             "ph=1 pid=0 tid=0 ts=7 dur=- cat=- name=- fh=- hh=- args{} top{} "
             "sp{}"},
        };
        json::Document doc;
        for (const Repaired& shape : shapes) {
            INFO(shape.line);
            StringIntern dom_intern;
            FoldEvent dom_ev;
            CHECK_FALSE(read_dom(shape.line, dom_ev, dom_intern, true));
            for (const bool needs_args : {true, false}) {
                const OnDemandRead got =
                    read_on_demand(doc, shape.line, needs_args);
                REQUIRE(got.parsed);
                CHECK_FALSE(got.broken);
                if (needs_args ||
                    StringView(shape.line).find("\"args\"") == StringView::npos)
                    CHECK(got.text == shape.expected);
                const OnDemandRead next = read_on_demand(
                    doc, R"j({"ph":1,"ts":9,"name":"z"})j", needs_args);
                CHECK_FALSE(next.broken);
            }
        }
        for (const String& broken : {String("{\"ph\":1,\x01\"ts\":5}"),
                                     String("{\"ph\":1,\xff\"ts\":5}"),
                                     String("{\"ph\":1,\"name\":\"a\tb"),
                                     String("{\"ph\":1,\"ts\":\x01 5}")}) {
            INFO(broken);
            const OnDemandRead got = read_on_demand(doc, broken, true);
            CHECK_FALSE((got.parsed && !got.broken));
        }
    }

    TEST_CASE(
        "the on-demand path reader equals the DOM reader on every shape") {
        dstd::Vector<StringView> lines;
        for (const Shape& shape : SHAPES) lines.emplace_back(shape.line);
        for (const char* line : EVENTS) lines.emplace_back(line);
        for (const char* line : PATH_LINES) lines.emplace_back(line);
        for (const char* line : CAPTURE_LINES) lines.emplace_back(line);
        check_path_lines(lines);
    }

    TEST_CASE(
        "the on-demand path reader equals the DOM reader on trace lines") {
        const dstd::Vector<String> generated = generated_lines(300);
        check_path_lines(views_of(generated));
    }

    TEST_CASE(
        "the path readers agree when the lines come in a different order") {
        dstd::Vector<StringView> lines;
        for (const char* line : PATH_LINES) lines.emplace_back(line);
        for (const Shape& shape : SHAPES) lines.emplace_back(shape.line);
        std::reverse(lines.begin(), lines.end());
        check_path_lines(lines);
    }

    TEST_CASE("the on-demand captures equal the DOM captures on every shape") {
        json::Document doc;
        const std::size_t all =
            sizeof(CAPTURE_NAMES) / sizeof(CAPTURE_NAMES[0]);
        const dstd::Vector<String> every = capture_names(0, all);
        dstd::Vector<String> reversed = every;
        std::reverse(reversed.begin(), reversed.end());
        dstd::Vector<StringView> lines;
        for (const char* line : CAPTURE_LINES) lines.emplace_back(line);
        for (const Shape& shape : SHAPES) lines.emplace_back(shape.line);
        for (const char* line : EVENTS) lines.emplace_back(line);
        for (StringView line : lines) {
            check_captures(doc, line, every);
            check_captures(doc, line, reversed);
        }
    }

    TEST_CASE("more captures than one walk holds equal the DOM captures") {
        json::Document doc;
        const dstd::Vector<String> names = many_capture_names();
        for (const char* line : CAPTURE_LINES) check_captures(doc, line, names);
        for (const String& line : generated_lines(50))
            check_captures(doc, line, names);
    }

    TEST_CASE("each capture alone equals the DOM capture") {
        json::Document doc;
        const std::size_t all =
            sizeof(CAPTURE_NAMES) / sizeof(CAPTURE_NAMES[0]);
        for (std::size_t i = 0; i < all; ++i) {
            CAPTURE(CAPTURE_NAMES[i]);
            const dstd::Vector<String> one = capture_names(i, 1);
            const dstd::Vector<String> twice{one[0], one[0]};
            for (const char* line : CAPTURE_LINES) {
                check_captures(doc, line, one);
                check_captures(doc, line, twice);
            }
        }
    }

    TEST_CASE("the on-demand captures equal the DOM captures on trace lines") {
        json::Document doc;
        const dstd::Vector<String> names = capture_names(0, 12);
        for (const String& line : generated_lines(300))
            check_captures(doc, line, names);
    }

    TEST_CASE("a capture leaves the line readable for the next consumer") {
        json::Document doc;
        const char* line =
            R"({"name":"read","ph":1,"ts":7,"args":{"fhash":"f","io":{"off":3}}})";
        auto value = doc.parse(line);
        REQUIRE(value.has_value());
        StringIntern intern;
        FoldEvent first;
        FoldEvent second;
        const dstd::Vector<String> names{"io.off", "args.io.off", "type"};
        extract_fold_event_into(first, *value, intern, true, nullptr, &names);
        ValueAccess::rewind(*value);
        extract_fold_event_into(second, *value, intern, true, nullptr, &names);
        CHECK_FALSE(ValueAccess::broken(*value));
        CHECK(describe(first, intern) == describe(second, intern));
        CHECK(describe(first, intern) ==
              "ph=1 pid=0 tid=0 ts=7 dur=- cat=- name=\"read\" fh=\"f\" hh=- "
              "args{\"io.off\"=i3 \"args.io.off\"=i3} top{} sp{}");
    }

    TEST_CASE("a capture of a broken part marks the line broken") {
        json::Document doc;
        auto value = doc.parse(R"({"ph":1,"args":{"a":1},"x":{"y":01}})");
        REQUIRE(value.has_value());
        StringIntern intern;
        FoldEvent ev;
        const dstd::Vector<String> names{"x.y"};
        extract_fold_event_into(ev, *value, intern, true, nullptr, &names);
        CHECK(ValueAccess::broken(*value));
        CHECK(describe(ev, intern) == describe_reset());
    }

    TEST_CASE("the on-demand path reader keeps what the DOM parser refuses") {
        struct Odd {
            const char* name;
            String line;
            String expected;
            bool dom_reads;
        };
        // A path event has the Complete phase and no fixed fields.
        const auto event = [](const char* tail) {
            return String(
                       "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- ") +
                   tail + " unc=0 bp=1";
        };
        const Odd shapes[] = {
            {"plus_overflow", R"j({"k":1,"x":1e400})j",
             event("args{\"k\"=i1 \"x\"=dinf} top{} sp{} lv{k:2 x:4}"), false},
            {"minus_overflow", R"j({"x":-1e400,"k":2})j",
             event("args{\"x\"=d-inf \"k\"=i2} top{} sp{} lv{x:4 k:2}"), false},
            {"exponent_overflow", R"j({"o":{"a":1.5E+999}})j",
             event("args{\"o.a\"=dinf} top{} sp{} lv{o.a:4}"), false},
            {"overflow_in_array", R"j({"l":[1,1e400,-1e400]})j",
             event("args{\"l.0\"=i1 \"l.1\"=dinf \"l.2\"=d-inf} top{} sp{} "
                   "lv{l.0:2 l.1:4 l.2:4}"),
             false},
            {"bad_escape_value", R"j({"s":"a\qb","k":1})j",
             event("args{\"s\"=s\"a\\\\qb\" \"k\"=i1} top{} sp{} "
                   "lv{s:5 k:2}"),
             false},
            {"bad_escape_key", R"j({"k\q":1,"o":{"p\q":2}})j",
             event("args{\"k\\\\q\"=i1 \"o.p\\\\q\"=i2} top{} sp{} "
                   "lv{k\\q:2 o.p\\q:2}"),
             false},
            {"lone_surrogate", R"j({"s":"\ud800"})j",
             event("args{\"s\"=s\"\\\\ud800\"} top{} sp{} lv{s:5}"), false},
            {"bigint_30", R"j({"b":123456789012345678901234567890,"k":1})j",
             event("args{\"b\"=s\"123456789012345678901234567890\" \"k\"=i1} "
                   "top{} sp{} lv{b:0 k:2}"),
             true},
            {"bigint_neg_20", R"j({"l":[-18446744073709551616]})j",
             event("args{\"l.0\"=s\"-18446744073709551616\"} top{} sp{} "
                   "lv{l.0:0}"),
             true},
            {"uint64_20_over", R"j({"b":18446744073709551616})j",
             event("args{\"b\"=s\"18446744073709551616\"} top{} sp{} lv{b:0}"),
             false},
            {"bad_utf8_value",
             String("{\"s\":\"a\xff"
                    "b\",\"k\":1}"),
             event("args{\"s\"=s\"a?b\" \"k\"=i1} top{} sp{} lv{s:5 k:2}"),
             false},
            {"bad_utf8_key", String("{\"\xff\":1,\"k\":2}"),
             event("args{\"?\"=i1 \"k\"=i2} top{} sp{} lv{?:2 k:2}"), false},
            {"control_byte", String("{\"s\":\"x\ty\",\"o\":{\"k\t\":1}}"),
             event("args{\"s\"=s\"x?y\" \"o.k?\"=i1} top{} sp{} "
                   "lv{s:5 o.k?:2}"),
             false},
            {"root_array", R"j([1e400,"a\q"])j",
             event("args{\"0\"=dinf \"1\"=s\"a\\\\q\"} top{} sp{} "
                   "lv{0:4 1:5}"),
             false},
        };
        json::Document doc;
        const PathOptions plain("plain");
        const StringView next_line = R"j({"k":9})j";
        for (const Odd& shape : shapes) {
            INFO(shape.name << " " << shape.line);
            StringIntern dom_intern;
            FoldEvent dom_ev;
            DecodeHints dom_hints;
            CHECK(read_dom_path(shape.line, dom_ev, dom_intern, plain,
                                dom_hints) == shape.dom_reads);
            for (const PathOptions& o : path_option_sets()) {
                // The reference reads the same lines with the same options
                // and no paths.
                PathOptions whole = o;
                whole.paths.clear();
                for (const bool with_cache : {false, true}) {
                    CAPTURE(o.name);
                    CAPTURE(with_cache);
                    StringIntern ref_intern;
                    dstd::InternCache ref_cache(ref_intern);
                    FoldEvent ref_ev;
                    DecodeHints ref_hints;
                    StringIntern intern;
                    dstd::InternCache cache(intern);
                    FoldEvent ev;
                    DecodeHints hints;
                    const StringView lines[] = {shape.line, next_line};
                    for (const StringView line : lines) {
                        CAPTURE(line);
                        const PathRead ref = read_path_on_demand(
                            doc, line, whole, ref_ev, ref_intern, ref_hints,
                            with_cache ? &ref_cache : nullptr);
                        const PathRead got =
                            read_path_on_demand(doc, line, o, ev, intern, hints,
                                                with_cache ? &cache : nullptr);
                        REQUIRE(got.parsed);
                        CHECK_FALSE(got.broken);
                        CHECK_FALSE(ref.broken);
                        CHECK(
                            describe_under_paths(ev, intern, o.paths) ==
                            describe_under_paths(ref_ev, ref_intern, o.paths));
                        if (line.data() == shape.line.data() && !o.typed &&
                            !o.skip_never && !o.max_children &&
                            o.capture_schema && o.paths.empty())
                            CHECK(got.text == shape.expected);
                    }
                }
            }
        }
    }

    TEST_CASE("the path readers keep a refused line in a declared field") {
        // The DOM parser refuses each line; the typed reader still binds the
        // declared fields it reads and keeps the rest by path.
        const PathOptions typed = [] {
            PathOptions o("typed");
            o.typed = true;
            return o;
        }();
        json::Document doc;
        StringIntern intern;
        FoldEvent ev;
        DecodeHints hints;
        const PathRead got =
            read_path_on_demand(doc, R"j({"k":1,"f":1e400,"s":"a\qb","ts":5})j",
                                typed, ev, intern, hints, nullptr);
        REQUIRE(got.parsed);
        CHECK_FALSE(got.broken);
        CHECK(got.text ==
              "ph=1 pid=0 tid=0 ts=5 dur=- cat=- name=- fh=- hh=- "
              "args{\"k\"=i1 \"f\"=dinf \"s\"=s\"a\\\\qb\" \"ts\"=i5} top{} "
              "sp{} lv{k:2 f:4 s:5 ts:2} unc=0 bp=1");
    }

    TEST_CASE("a scalar overflow keeps its written token in a text field") {
        const PathOptions typed = [] {
            PathOptions o("typed");
            o.typed = true;
            return o;
        }();
        json::Document doc;
        StringIntern intern;
        FoldEvent ev;
        DecodeHints hints;
        const PathRead got =
            read_path_on_demand(doc, R"j({"s":1e400,"o":-1E+999,"l":[2e400]})j",
                                typed, ev, intern, hints, nullptr);
        REQUIRE(got.parsed);
        CHECK_FALSE(got.broken);
        CHECK(got.text ==
              "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
              "args{\"s\"=s\"1e400\" \"o\"=s\"-1E+999\" \"l\"=s\"[2e400]\" "
              "\"l.0\"=dinf} top{} sp{\"l\":5} lv{s:4 o:4 l.0:4} unc=0 bp=1");
    }

    TEST_CASE("the on-demand captures keep what the DOM parser refuses") {
        json::Document doc;
        StringIntern intern;
        FoldEvent ev;
        const dstd::Vector<String> names{"x.y", "x.z", "x.w"};
        const String line =
            R"j({"ph":1,"x":{"y":1e400,"z":"a\qb","w":123456789012345678901234567890}})j";
        auto value = doc.parse(line);
        REQUIRE(value.has_value());
        extract_fold_event_into(ev, *value, intern, true, nullptr, &names);
        CHECK_FALSE(ValueAccess::broken(*value));
        CHECK(describe(ev, intern) ==
              "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
              "args{\"x.y\"=dinf \"x.z\"=s\"a\\\\qb\" "
              "\"x.w\"=s\"123456789012345678901234567890\"} top{} sp{}");
        const String repaired = String(
            "{\"ph\":1,\"x\":{\"y\":\"p\xff"
            "q\"}}");
        value = doc.parse(repaired);
        REQUIRE(value.has_value());
        const dstd::Vector<String> one{"x.y"};
        extract_fold_event_into(ev, *value, intern, true, nullptr, &one);
        CHECK_FALSE(ValueAccess::broken(*value));
        CHECK(describe(ev, intern) ==
              "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
              "args{\"x.y\"=s\"p?q\"} top{} sp{}");
    }
}
