// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_TRACE_VIEWS_FOLD_SHAPES_H
#define DFTRACER_UTILS_TESTS_TRACE_VIEWS_FOLD_SHAPES_H

#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/trace/views/fold.h>
#include <simdjson.h>

#include <cstdint>
#include <variant>

// Event lines where a forward, single-pass reader could differ from a reader
// that sees the whole line first, with the text of the FoldEvent that the DOM
// reader gives when it reads args. The text resolves every intern id, so the
// numbers the intern table hands out do not matter.

namespace fold_shapes {

using FoldEvent = dftracer::utils::trace::views::detail::FoldEvent;
using StringIntern = dftracer::utils::dstd::StringIntern;
using String = dftracer::utils::dstd::String;
using StringView = dftracer::utils::dstd::StringView;

struct Shape {
    const char* name;
    const char* line;
    const char* expected;
};

inline const Shape SHAPES[] = {
    {"dup_ts", R"j({"ph":1,"ts":1,"ts":2})j",
     "ph=1 pid=0 tid=0 ts=2 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"dup_ts_bad_later", R"j({"ph":1,"ts":1,"ts":"x"})j",
     "ph=1 pid=0 tid=0 ts=1 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"dup_ts_neg_later", R"j({"ph":1,"ts":1,"ts":-4})j",
     "ph=1 pid=0 tid=0 ts=1 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"dup_ts_double_later", R"j({"ph":1,"ts":1,"ts":2.5})j",
     "ph=1 pid=0 tid=0 ts=1 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"dup_pid_tid", R"j({"ph":1,"pid":1,"pid":2,"tid":3,"tid":4})j",
     "ph=1 pid=2 tid=4 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"dup_ph", R"j({"ph":1,"ph":2})j",
     "ph=2 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"dup_ph_letter", R"j({"ph":"X","ph":"C"})j",
     "ph=2 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"dup_ph_bad_later", R"j({"ph":1,"ph":"zz"})j",
     "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"dup_ph_after_fields", R"j({"ph":1,"ts":5,"ph":3})j",
     "ph=3 pid=0 tid=0 ts=5 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"dup_name", R"j({"ph":1,"name":"a","name":"b"})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"b\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"dup_name_bad_later", R"j({"ph":1,"name":"a","name":3})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"a\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"dup_name_empty_later", R"j({"ph":1,"name":"a","name":""})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"dup_cat", R"j({"ph":1,"cat":"a","cat":"b","cat":null})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=\"b\" name=- fh=- hh=- args{} top{} "
     "sp{}"},
    {"dup_dur", R"j({"ph":1,"dur":5,"dur":7})j",
     "ph=1 pid=0 tid=0 ts=0 dur=7 cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"dup_dur_bad_later", R"j({"ph":1,"dur":5,"dur":-1})j",
     "ph=1 pid=0 tid=0 ts=0 dur=5 cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"dup_dur_null_later", R"j({"ph":1,"dur":5,"dur":null})j",
     "ph=1 pid=0 tid=0 ts=0 dur=5 cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"dup_args_two_objects", R"j({"ph":1,"args":{"a":1},"args":{"b":2}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"b\"=i2} top{} "
     "sp{}"},
    {"dup_args_scalar_later", R"j({"ph":1,"args":{"a":1},"args":3})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"a\"=i1} top{} "
     "sp{}"},
    {"dup_args_scalar_first", R"j({"ph":1,"args":3,"args":{"a":1}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"a\"=i1} top{} "
     "sp{}"},
    {"dup_args_array_later", R"j({"ph":1,"args":{"a":1},"args":[1,2]})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"a\"=i1} top{} "
     "sp{}"},
    {"dup_args_hash_in_first",
     R"j({"ph":1,"args":{"fhash":"x","hhash":"y","a":1},"args":{"b":2}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"b\"=i2} top{} "
     "sp{}"},
    {"dup_args_hash_in_second",
     R"j({"ph":1,"args":{"a":1},"args":{"fhash":"x","b":2}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=\"x\" hh=- args{\"b\"=i2} "
     "top{} sp{}"},
    {"dup_args_empty_later", R"j({"ph":1,"args":{"a":1},"args":{}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"args_before_scalars", R"j({"args":{"a":1},"ph":1,"name":"n","ts":3})j",
     "ph=1 pid=0 tid=0 ts=3 dur=- cat=- name=\"n\" fh=- hh=- args{\"a\"=i1} "
     "top{} sp{}"},
    {"args_after_scalars", R"j({"ph":1,"name":"n","ts":3,"args":{"a":1}})j",
     "ph=1 pid=0 tid=0 ts=3 dur=- cat=- name=\"n\" fh=- hh=- args{\"a\"=i1} "
     "top{} sp{}"},
    {"args_between", R"j({"ph":1,"args":{"a":1},"name":"n","dur":4})j",
     "ph=1 pid=0 tid=0 ts=0 dur=4 cat=- name=\"n\" fh=- hh=- args{\"a\"=i1} "
     "top{} sp{}"},
    {"no_ph", R"j({"name":"n","ts":3,"args":{"a":1}})j",
     "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"no_ph_empty", R"j({})j",
     "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"ph_int_1", R"j({"ph":1})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"ph_int_4", R"j({"ph":4,"name":"process_name"})j",
     "ph=4 pid=0 tid=0 ts=0 dur=- cat=- name=\"process_name\" fh=- hh=- args{} "
     "top{} sp{}"},
    {"ph_int_5", R"j({"ph":5,"name":"n"})j",
     "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"ph_letter_x", R"j({"ph":"X","name":"n"})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"ph_letter_m", R"j({"ph":"M","name":"n"})j",
     "ph=4 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"ph_letter_unknown", R"j({"ph":"Q","name":"n"})j",
     "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"ph_letter_long", R"j({"ph":"XX","name":"n"})j",
     "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"ph_negative", R"j({"ph":-1,"name":"n"})j",
     "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"ph_double", R"j({"ph":1.0,"name":"n"})j",
     "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"ph_null", R"j({"ph":null,"name":"n"})j",
     "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"ph_bool", R"j({"ph":true,"name":"n"})j",
     "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"ph_array", R"j({"ph":[1],"name":"n"})j",
     "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"ph_object", R"j({"ph":{"a":1},"name":"n"})j",
     "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"ph_uint64_max", R"j({"ph":18446744073709551615,"name":"n"})j",
     "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"ph_bigint", R"j({"ph":123456789012345678901234567890,"name":"n"})j",
     "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"ph_escaped_key", R"j({"ph":1,"name":"n"})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"ph_escaped_letter", R"j({"ph":"X","name":"n"})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"ts_int64_max", R"j({"ph":1,"ts":9223372036854775807})j",
     "ph=1 pid=0 tid=0 ts=9223372036854775807 dur=- cat=- name=- fh=- hh=- "
     "args{} top{} sp{}"},
    {"ts_above_int64", R"j({"ph":1,"ts":9223372036854775808})j",
     "ph=1 pid=0 tid=0 ts=9223372036854775808 dur=- cat=- name=- fh=- hh=- "
     "args{} top{} sp{}"},
    {"ts_uint64_max", R"j({"ph":1,"ts":18446744073709551615})j",
     "ph=1 pid=0 tid=0 ts=18446744073709551615 dur=- cat=- name=- fh=- hh=- "
     "args{} top{} sp{}"},
    {"ts_bigint", R"j({"ph":1,"ts":123456789012345678901234567890})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"ts_negative", R"j({"ph":1,"ts":-1})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"ts_int64_min", R"j({"ph":1,"ts":-9223372036854775808})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"ts_double_integral", R"j({"ph":1,"ts":1.0})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"ts_exponent", R"j({"ph":1,"ts":1e3})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"ts_double", R"j({"ph":1,"ts":1.5})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"ts_string", R"j({"ph":1,"ts":"5"})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"ts_bool", R"j({"ph":1,"ts":true})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"ts_null", R"j({"ph":1,"ts":null})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"ts_zero", R"j({"ph":1,"ts":0})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"ts_minus_zero", R"j({"ph":1,"ts":-0})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"pid_negative", R"j({"ph":1,"pid":-5,"tid":-6})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"pid_max",
     R"j({"ph":1,"pid":18446744073709551615,"tid":9223372036854775807})j",
     "ph=1 pid=18446744073709551615 tid=9223372036854775807 ts=0 dur=- cat=- "
     "name=- fh=- hh=- args{} top{} sp{}"},
    {"dur_int64_max", R"j({"ph":1,"dur":9223372036854775807})j",
     "ph=1 pid=0 tid=0 ts=0 dur=9223372036854775807 cat=- name=- fh=- hh=- "
     "args{} top{} sp{}"},
    {"dur_above_int64", R"j({"ph":1,"dur":9223372036854775808})j",
     "ph=1 pid=0 tid=0 ts=0 dur=9223372036854775808 cat=- name=- fh=- hh=- "
     "args{} top{} sp{}"},
    {"dur_zero", R"j({"ph":1,"dur":0})j",
     "ph=1 pid=0 tid=0 ts=0 dur=0 cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"dur_double", R"j({"ph":1,"dur":1.5})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"dur_string", R"j({"ph":1,"dur":"7"})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"dur_negative", R"j({"ph":1,"dur":-5})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"id_and_type_ignored",
     R"j({"ph":1,"id":5,"type":3,"extra":{"a":1},"name":"n"})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"other_top_keys", R"j({"ph":1,"phx":2,"p":3,"n":4,"names":"z","t":5})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"cat_name_escaped", R"j({"ph":1,"cat":"a\"b","name":"c\\d\ne"})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=\"a\\\"b\" name=\"c\\\\d\\ne\" fh=- hh=- "
     "args{} top{} sp{}"},
    {"cat_name_unicode",
     R"j({"ph":1,"cat":")j"
     "\xC3\xA9\xF0\x9F\x98\x80"
     R"j(","name":")j"
     "\xC3\xA9\xF0\x9F\x98\x80"
     R"j("})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=\""
     "\xC3\xA9\xF0\x9F\x98\x80"
     "\" name=\""
     "\xC3\xA9\xF0\x9F\x98\x80"
     "\" fh=- hh=- args{} top{} sp{}"},
    {"cat_name_empty", R"j({"ph":1,"cat":"","name":""})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"name_escaped_key", R"j({"ph":1,"name":"x"})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"x\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"cat_name_not_string", R"j({"ph":1,"cat":5,"name":[1]})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"args_ints",
     R"j({"ph":1,"args":{"a":1,"b":-2,"c":0,"d":9223372036854775807,"e":-9223372036854775808}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"a\"=i1 "
     "\"b\"=i-2 \"c\"=i0 \"d\"=i9223372036854775807 "
     "\"e\"=i-9223372036854775808} top{} sp{}"},
    {"args_uint64",
     R"j({"ph":1,"args":{"a":9223372036854775808,"b":18446744073709551615}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
     "args{\"a\"=u9223372036854775808 \"b\"=u18446744073709551615} top{} sp{}"},
    {"args_bigint_30",
     R"j({"ph":1,"args":{"a":123456789012345678901234567890,"b":-123456789012345678901234567890}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
     "args{\"a\"=s\"123456789012345678901234567890\" "
     "\"b\"=s\"-123456789012345678901234567890\"} top{} sp{}"},
    {"args_bigint_21", R"j({"ph":1,"args":{"a":123456789012345678901,"x":1}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
     "args{\"a\"=s\"123456789012345678901\" \"x\"=i1} top{} sp{}"},
    {"args_bigint_neg_19",
     R"j({"ph":1,"args":{"a":-9223372036854775809,"x":1}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
     "args{\"a\"=s\"-9223372036854775809\" \"x\"=i1} top{} sp{}"},
    {"args_bigint_neg_20",
     R"j({"ph":1,"args":{"a":-18446744073709551616,"x":1}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
     "args{\"a\"=s\"-18446744073709551616\" \"x\"=i1} top{} sp{}"},
    {"args_bigint_in_array",
     R"j({"ph":1,"args":{"l":[1,123456789012345678901234567890,2]}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"l.0\"=i1 "
     "\"l.1\"=s\"123456789012345678901234567890\" \"l.2\"=i2} top{} sp{}"},
    {"args_double_big",
     R"j({"ph":1,"args":{"a":1.5e308,"b":123456789012345678901234567890.5}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"a\"=d1.5e+308 "
     "\"b\"=d1.2345678901234568e+29} top{} sp{}"},
    {"bad_number_leading_zero", R"j({"ph":1,"args":{"a":01}})j", "rejected"},
    {"bad_number_trailing_dot", R"j({"ph":1,"args":{"a":1.}})j", "rejected"},
    {"bad_number_lone_minus", R"j({"ph":1,"args":{"a":-}})j", "rejected"},
    {"bad_number_top_level", R"j({"ph":1,"ts":01,"name":"n"})j", "rejected"},
    {"bad_number_trailing_garbage", R"j({"ph":1,"args":{"a":1x}})j",
     "rejected"},
    {"bad_number_plus", R"j({"ph":1,"args":{"a":+1}})j", "rejected"},
    {"bad_number_leading_dot", R"j({"ph":1,"args":{"a":.5}})j", "rejected"},
    {"bad_number_nan", R"j({"ph":1,"args":{"a":NaN}})j", "rejected"},
    {"bad_atom_true", R"j({"ph":1,"args":{"a":tru}})j", "rejected"},
    {"bad_atom_null", R"j({"ph":1,"args":{"a":nul}})j", "rejected"},
    {"bad_value_unknown_char", R"j({"ph":1,"args":{"a":x}})j", "rejected"},
    {"args_doubles",
     R"j({"ph":1,"args":{"a":1.5,"b":-1.5e3,"c":1e2,"d":0.0,"e":-0.0,"f":1E-2,"g":2.5e+3}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"a\"=d1.5 "
     "\"b\"=d-1500 \"c\"=d100 \"d\"=d0 \"e\"=d-0 \"f\"=d0.01 \"g\"=d2500} "
     "top{} sp{}"},
    {"args_minus_zero", R"j({"ph":1,"args":{"a":-0}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"a\"=i0} top{} "
     "sp{}"},
    {"args_integral_double", R"j({"ph":1,"args":{"a":1.0,"b":100.0}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"a\"=d1 "
     "\"b\"=d100} top{} sp{}"},
    {"args_strings",
     R"j({"ph":1,"args":{"a":"x","b":"","c":"a\"b","d":")j"
     "\xC3\xA9"
     R"j(","e":"l1\nl2","f":")j"
     "\xF0\x9F\x98\x80"
     R"j(","g":")j"
     "\xC3\xA9\xF0\x9F\x98\x80"
     R"j(","h":"tab\there"}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"a\"=s\"x\" "
     "\"b\"=s\"\" \"c\"=s\"a\\\"b\" \"d\"=s\""
     "\xC3\xA9"
     "\" \"e\"=s\"l1\\nl2\" \"f\"=s\""
     "\xF0\x9F\x98\x80"
     "\" \"g\"=s\""
     "\xC3\xA9\xF0\x9F\x98\x80"
     "\" \"h\"=s\"tab\\there\"} top{} sp{}"},
    {"args_bool_null", R"j({"ph":1,"args":{"a":true,"b":false,"c":null}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"a\"=i1 "
     "\"b\"=i0} top{} sp{\"a\":2 \"b\":1 \"c\":0}"},
    {"args_escaped_keys",
     R"j({"ph":1,"args":{"kA":1,"k\"q":2,"k\\b":3,")j"
     "\xC3\xA9"
     R"j(":4,")j"
     "\xC3\xA9"
     R"j(x":5}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"kA\"=i1 "
     "\"k\\\"q\"=i2 \"k\\\\b\"=i3 \""
     "\xC3\xA9"
     "\"=i4 \""
     "\xC3\xA9"
     "x\"=i5} top{} sp{}"},
    {"args_dup_keys", R"j({"ph":1,"args":{"a":1,"a":2,"a":"x"}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"a\"=i1 "
     "\"a\"=i2 \"a\"=s\"x\"} top{} sp{}"},
    {"args_nested_object", R"j({"ph":1,"args":{"o":{"a":{"b":1},"c":2}}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"o.a.b\"=i1 "
     "\"o.c\"=i2} top{} sp{}"},
    {"args_nested_array", R"j({"ph":1,"args":{"l":[1,"x",null,true,2.5]}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"l.0\"=i1 "
     "\"l.1\"=s\"x\" \"l.3\"=i1 \"l.4\"=d2.5} top{} sp{\"l.2\":0 \"l.3\":2}"},
    {"args_nested_mixed",
     R"j({"ph":1,"args":{"o":{"a":[1,{"b":2},[3,4]],"c":{"d":{"e":"f"}}}}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"o.a.0\"=i1 "
     "\"o.a.1.b\"=i2 \"o.a.2.0\"=i3 \"o.a.2.1\"=i4 \"o.c.d.e\"=s\"f\"} top{} "
     "sp{}"},
    {"args_nested_dup", R"j({"ph":1,"args":{"o":{"a":1,"a":2},"o":{"a":3}}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"o.a\"=i1 "
     "\"o.a\"=i2 \"o.a\"=i3} top{} sp{}"},
    {"args_empty_array", R"j({"ph":1,"args":{"a":[]}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} "
     "sp{\"a\":3}"},
    {"args_empty_object", R"j({"ph":1,"args":{"a":{}}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} "
     "sp{\"a\":4}"},
    {"args_nested_empty",
     R"j({"ph":1,"args":{"o":{"e":[],"f":{},"g":[[]],"h":[{}]},"x":1}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"x\"=i1} top{} "
     "sp{\"o.e\":3 \"o.f\":4 \"o.g.0\":3 \"o.h.0\":4}"},
    {"args_empty", R"j({"ph":1,"args":{}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"args_nested_escaped_keys", R"j({"ph":1,"args":{"o":{"kA":1,"k.b":2}}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"o.kA\"=i1 "
     "\"o.k.b\"=i2} top{} sp{}"},
    {"args_dotted_key", R"j({"ph":1,"args":{"a.b":1,"a":{"b":2}}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"a.b\"=i1 "
     "\"a.b\"=i2} top{} sp{}"},
    {"args_deep",
     R"j({"ph":1,"args":{"a":{"b":{"c":{"d":{"e":{"f":{"g":{"h":{"i":{"j":{"k":{"l":1}}}}}}}}}}}}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
     "args{\"a.b.c.d.e.f.g.h.i.j.k.l\"=i1} top{} sp{}"},
    {"args_array_index_10",
     R"j({"ph":1,"args":{"l":[0,1,2,3,4,5,6,7,8,9,10,11]}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"l.0\"=i0 "
     "\"l.1\"=i1 \"l.2\"=i2 \"l.3\"=i3 \"l.4\"=i4 \"l.5\"=i5 \"l.6\"=i6 "
     "\"l.7\"=i7 \"l.8\"=i8 \"l.9\"=i9 \"l.10\"=i10 \"l.11\"=i11} top{} sp{}"},
    {"args_not_object_array", R"j({"ph":1,"args":[1,2],"name":"n"})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"args_not_object_string", R"j({"ph":1,"args":"x","name":"n"})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"args_not_object_null", R"j({"ph":1,"args":null,"name":"n"})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} top{} "
     "sp{}"},
    {"fhash_hhash", R"j({"ph":1,"args":{"fhash":"f1","hhash":"h1","x":1}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=\"f1\" hh=\"h1\" "
     "args{\"x\"=i1} top{} sp{}"},
    {"fhash_not_string", R"j({"ph":1,"args":{"fhash":3,"hhash":null,"x":1}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"x\"=i1} top{} "
     "sp{}"},
    {"fhash_dup", R"j({"ph":1,"args":{"fhash":"a","fhash":"b","hhash":"c"}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=\"b\" hh=\"c\" args{} top{} "
     "sp{}"},
    {"fhash_dup_bad_later", R"j({"ph":1,"args":{"fhash":"a","fhash":3}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"fhash_dup_good_after_bad", R"j({"ph":1,"args":{"fhash":3,"fhash":"a"}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=\"a\" hh=- args{} top{} "
     "sp{}"},
    {"fhash_escaped", R"j({"ph":1,"args":{"fhash":"a\"b","hhash":"c"}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=\"a\\\"b\" hh=\"c\" args{} "
     "top{} sp{}"},
    {"fhash_nested_not_special",
     R"j({"ph":1,"args":{"o":{"fhash":"z"},"x":1}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
     "args{\"o.fhash\"=s\"z\" \"x\"=i1} top{} sp{}"},
    {"fhash_container", R"j({"ph":1,"args":{"fhash":{"a":1},"hhash":[1]}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"fhash_empty", R"j({"ph":1,"args":{"fhash":"","hhash":""}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=\"\" hh=\"\" args{} top{} "
     "sp{}"},
    {"pretty",
     R"j({ "ph" : 1 , "name" : "n" , "args" : { "a" : [ 1 , 2 ] , "b" : { } } })j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{\"a.0\"=i1 "
     "\"a.1\"=i2} top{} sp{\"b\":4}"},
    {"array_root", R"j([{"ph":1}])j",
     "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"empty_array_root", R"j([])j",
     "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"array_root_numbers", R"j([1,2,3])j",
     "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
    {"unicode_value_args",
     R"j({"ph":1,"name":"n","args":{"fname":"/data/)j"
     "\xC3\xBC"
     R"j(/)j"
     "\xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB"
     R"j(.h5","size":4096}})j",
     "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- "
     "args{\"fname\"=s\"/data/"
     "\xC3\xBC"
     "/"
     "\xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB"
     ".h5\" \"size\"=i4096} top{} sp{}"},
    {"dftracer_line",
     R"j({"id":0,"name":"read","cat":"POSIX","pid":1234,"tid":5678,"ts":1000,"dur":20,"ph":"X","args":{"hhash":"abc","fhash":"def","fname":"/data/a.h5","size":4096,"ret":4096,"level":3}})j",
     "ph=1 pid=1234 tid=5678 ts=1000 dur=20 cat=\"POSIX\" name=\"read\" "
     "fh=\"def\" hh=\"abc\" args{\"fname\"=s\"/data/a.h5\" \"size\"=i4096 "
     "\"ret\"=i4096 \"level\"=i3} top{} sp{}"},
    {"nested_in_other_keys",
     R"j({"ph":1,"meta":{"ts":9,"ph":2,"args":{"z":1}},"ts":5})j",
     "ph=1 pid=0 tid=0 ts=5 dur=- cat=- name=- fh=- hh=- args{} top{} sp{}"},
};

/// The text of `ev`: the fixed fields, the interned strings as text, `args`
/// and `top` as key=value (i int64, u uint64, d double, s string), and the
/// specials as key:kind.
inline String describe(const FoldEvent& ev, const StringIntern& intern) {
    using dftracer::utils::common::str_cat;
    constexpr std::uint32_t NO_ID = StringIntern::NO_ID;
    const auto quote = [](StringView s) {
        String out = "\"";
        for (char c : s) {
            if (c == '\\' || c == '"') out += '\\';
            if (c == '\n') {
                out += "\\n";
            } else if (c == '\t') {
                out += "\\t";
            } else {
                out += c;
            }
        }
        return out + "\"";
    };
    const auto text = [&](std::uint32_t id) {
        return id == NO_ID ? String("-") : quote(intern.resolve(id));
    };
    String out = str_cat("ph=", static_cast<int>(ev.phase), " pid=", ev.pid,
                         " tid=", ev.tid, " ts=", ev.ts,
                         " dur=", ev.has_dur ? str_cat(ev.dur) : String("-"),
                         " cat=", text(ev.cat_id), " name=", text(ev.name_id),
                         " fh=", text(ev.fhash_id), " hh=", text(ev.hhash_id));
    const auto list = [&](const char* label, const auto& args) {
        out += str_cat(" ", label, "{");
        bool first = true;
        for (const auto& [key, value] : args) {
            if (!first) out += " ";
            first = false;
            out += str_cat(quote(intern.resolve(key)), "=");
            switch (value.index()) {
                case 0:
                    out += str_cat("d", std::get<0>(value));
                    break;
                case 1:
                    out += str_cat("i", std::get<1>(value));
                    break;
                case 2:
                    out += str_cat("s", text(std::get<2>(value)));
                    break;
                default:
                    out += str_cat("u", std::get<3>(value));
                    break;
            }
        }
        out += "}";
    };
    list("args", ev.args);
    list("top", ev.top_fields);
    out += " sp{";
    bool first = true;
    for (const auto& s : ev.specials) {
        if (!first) out += " ";
        first = false;
        out += str_cat(quote(intern.resolve(s.key)), ":",
                       static_cast<int>(s.kind), s.top ? "t" : "");
    }
    out += "}";
    return out;
}

/// The DOM reader on `line`, with the parser the trace readers use, into `ev`.
/// False when the parser rejects the line.
inline bool read_dom(StringView line, FoldEvent& ev, StringIntern& intern,
                     bool needs_args) {
    static thread_local dftracer::utils::json::RecordParser parser;
    const simdjson::padded_string padded(line.data(), line.size());
    simdjson::dom::element root;
    if (parser.parse(padded).get(root)) return false;
    dftracer::utils::trace::views::detail::extract_fold_event_into(
        ev, root, intern, needs_args);
    return true;
}

}  // namespace fold_shapes

#endif  // DFTRACER_UTILS_TESTS_TRACE_VIEWS_FOLD_SHAPES_H
