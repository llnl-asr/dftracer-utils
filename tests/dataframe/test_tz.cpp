// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/tz.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

// The expected values below come from Python's zoneinfo (offsets, rules) and
// pandas 3.1 (wall time to UTC) over the TZif files in data/tz, which are
// copied from the Python tzdata package (see data/tz/README).
//
// pandas moves a nonexistent time forward to the next whole hour and backward
// to the whole hour before it, less one tick. Here a shift forward is the
// first instant after the gap and a shift backward is one tick before it, so
// the two agree for a gap that ends on the hour (America/New_York) and the
// expected values for the other zones are the transition instants.

namespace {

constexpr std::int64_t INT64_LOW = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t INT64_HIGH = std::numeric_limits<std::int64_t>::max();

dstd::Vector<dstd::String> fixture_dirs() {
    dstd::Vector<dstd::String> dirs;
    dirs.emplace_back(DFTRACER_UTILS_TZ_DATA_DIR);
    return dirs;
}

tz::Zone fixture(dstd::StringView name) {
    const auto dirs = fixture_dirs();
    auto zone = tz::load_zone(name, dirs);
    REQUIRE_MESSAGE(zone.has_value(), name);
    return std::move(*zone);
}

dstd::Span<const std::uint8_t> as_bytes(dstd::StringView s) {
    return dstd::Span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

bool has_code(const dstd::Error& error, common::ErrorCode code) {
    return common::errorcode_of(error) == code;
}

bool mentions(const dstd::Error& error, dstd::StringView text) {
    return error.message().find(text) != dstd::StringView::npos;
}

struct OffsetCase {
    const char* zone;
    std::int64_t utc;
    std::int32_t offset;
    bool is_dst;
    const char* abbreviation;
};

constexpr OffsetCase OFFSET_CASES[] = {
    {"America/New_York", -3786825600LL, -17762, false, "LMT"},
    {"America/New_York", -2717650801LL, -17762, false, "LMT"},
    {"America/New_York", -2717650800LL, -18000, false, "EST"},
    {"America/New_York", -2147483648LL, -18000, false, "EST"},
    {"America/New_York", 0LL, -18000, false, "EST"},
    {"America/New_York", 1710053999LL, -18000, false, "EST"},
    {"America/New_York", 1710054000LL, -14400, true, "EDT"},
    {"America/New_York", 1719835200LL, -14400, true, "EDT"},
    {"America/New_York", 1730613599LL, -14400, true, "EDT"},
    {"America/New_York", 1730613600LL, -18000, false, "EST"},
    {"America/New_York", 1909094400LL, -14400, true, "EDT"},
    {"America/New_York", 1893456000LL, -18000, false, "EST"},
    {"America/New_York", 4118083200LL, -14400, true, "EDT"},
    {"America/New_York", 4133980799LL, -18000, false, "EST"},
    {"America/New_York", 16740864000LL, -14400, true, "EDT"},
    {"America/New_York", 221845392000LL, -18000, false, "EST"},
    {"Europe/London", -3881520000LL, -75, false, "LMT"},
    {"Europe/London", -2208988800LL, 0, false, "GMT"},
    {"Europe/London", 57722400LL, 0, false, "GMT"},
    {"Europe/London", 1711846799LL, 0, false, "GMT"},
    {"Europe/London", 1711846800LL, 3600, true, "BST"},
    {"Europe/London", 1729990799LL, 3600, true, "BST"},
    {"Europe/London", 1729990800LL, 0, false, "GMT"},
    {"Europe/London", 1906502400LL, 3600, true, "BST"},
    {"Europe/London", 4115491200LL, 3600, true, "BST"},
    {"Europe/London", 4102444800LL, 0, false, "GMT"},
    {"Australia/Lord_Howe", -2524521600LL, 38180, false, "LMT"},
    {"Australia/Lord_Howe", 1712415599LL, 39600, true, "+11"},
    {"Australia/Lord_Howe", 1712415600LL, 37800, false, "+1030"},
    {"Australia/Lord_Howe", 1704067200LL, 39600, true, "+11"},
    {"Australia/Lord_Howe", 1893456000LL, 39600, true, "+11"},
    {"Australia/Lord_Howe", 1909094400LL, 37800, false, "+1030"},
    {"Australia/Lord_Howe", 4102444800LL, 39600, true, "+11"},
    {"Australia/Lord_Howe", 4118083200LL, 37800, false, "+1030"},
    {"Australia/Lord_Howe", 1712415599LL, 39600, true, "+11"},
    {"Australia/Lord_Howe", 1712415600LL, 37800, false, "+1030"},
    {"Australia/Lord_Howe", 1728142199LL, 37800, false, "+1030"},
    {"Australia/Lord_Howe", 1728142200LL, 39600, true, "+11"},
    {"Asia/Kolkata", -5364662400LL, 21208, false, "LMT"},
    {"Asia/Kolkata", -2208988800LL, 19270, false, "MMT"},
    {"Asia/Kolkata", -862617600LL, 23400, true, "+0630"},
    {"Asia/Kolkata", 1704067200LL, 19800, false, "IST"},
    {"Asia/Kolkata", 1909094400LL, 19800, false, "IST"},
    {"Asia/Kolkata", 4102444800LL, 19800, false, "IST"},
    {"Asia/Kolkata", -891581401LL, 19800, false, "IST"},
    {"Asia/Kolkata", -891581400LL, 23400, true, "+0630"},
    {"Asia/Kolkata", -872058601LL, 23400, true, "+0630"},
    {"Asia/Kolkata", -872058600LL, 19800, false, "IST"},
    {"Asia/Kolkata", -862637401LL, 19800, false, "IST"},
    {"Asia/Kolkata", -862637400LL, 23400, true, "+0630"},
    {"Asia/Kolkata", -764145001LL, 23400, true, "+0630"},
    {"Asia/Kolkata", -764145000LL, 19800, false, "IST"},
    {"Pacific/Chatham", -5364662400LL, 44028, false, "LMT"},
    {"Pacific/Chatham", -631152000LL, 45900, false, "+1245"},
    {"Pacific/Chatham", 1704067200LL, 49500, true, "+1345"},
    {"Pacific/Chatham", 1906502400LL, 45900, false, "+1245"},
    {"Pacific/Chatham", 4102444800LL, 49500, true, "+1345"},
    {"Pacific/Chatham", 1712411999LL, 49500, true, "+1345"},
    {"Pacific/Chatham", 1712412000LL, 45900, false, "+1245"},
    {"Pacific/Chatham", 1727531999LL, 45900, false, "+1245"},
    {"Pacific/Chatham", 1727532000LL, 49500, true, "+1345"},
    {"Pacific/Apia", -5364662400LL, 45184, false, "LMT"},
    {"Pacific/Apia", -631152000LL, -41400, false, "-1130"},
    {"Pacific/Apia", 1325239199LL, -36000, true, "-10"},
    {"Pacific/Apia", 1325239200LL, 50400, true, "+14"},
    {"Pacific/Apia", 1325332800LL, 50400, true, "+14"},
    {"Pacific/Apia", 1617458399LL, 50400, true, "+14"},
    {"Pacific/Apia", 1617458400LL, 46800, false, "+13"},
    {"Pacific/Apia", 1704067200LL, 46800, false, "+13"},
    {"Pacific/Apia", 4102444800LL, 46800, false, "+13"},
    {"America/Sao_Paulo", -5364662400LL, -11188, false, "LMT"},
    {"America/Sao_Paulo", 1541300399LL, -10800, false, "-03"},
    {"America/Sao_Paulo", 1541300400LL, -7200, true, "-02"},
    {"America/Sao_Paulo", 1550368799LL, -7200, true, "-02"},
    {"America/Sao_Paulo", 1550368800LL, -10800, false, "-03"},
    {"America/Sao_Paulo", 1704067200LL, -10800, false, "-03"},
    {"America/Sao_Paulo", 1893456000LL, -10800, false, "-03"},
    {"America/Sao_Paulo", 4118083200LL, -10800, false, "-03"},
    {"Asia/Tokyo", -5364662400LL, 33539, false, "LMT"},
    {"Asia/Tokyo", -620438400LL, 32400, false, "JST"},
    {"Asia/Tokyo", 1704067200LL, 32400, false, "JST"},
    {"Asia/Tokyo", 4102444800LL, 32400, false, "JST"},
};

enum class Expect { Ok, Nat, Error };

struct WallCase {
    const char* zone;
    std::int64_t wall_ns;
    tz::Ambiguous ambiguous;
    tz::Nonexistent nonexistent;
    std::int64_t shift_ns;
    Expect expect;
    std::int64_t utc_ns;
};

using Ambiguous = tz::Ambiguous;
using Nonexistent = tz::Nonexistent;

constexpr WallCase WALL_CASES[] = {
    {"America/New_York", 1730597400000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"America/New_York", 1730597400000000000LL, Ambiguous::Earliest,
     Nonexistent::Raise, 0LL, Expect::Ok, 1730611800000000000LL},
    {"America/New_York", 1730597400000000000LL, Ambiguous::Latest,
     Nonexistent::Raise, 0LL, Expect::Ok, 1730615400000000000LL},
    {"America/New_York", 1730597400000000000LL, Ambiguous::Nat,
     Nonexistent::Raise, 0LL, Expect::Nat, 0LL},
    {"America/New_York", 1710037800000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"America/New_York", 1710037800000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftForward, 0LL, Expect::Ok, 1710054000000000000LL},
    {"America/New_York", 1710037800000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftBackward, 0LL, Expect::Ok, 1710053999999999999LL},
    {"America/New_York", 1710037800000000000LL, Ambiguous::Raise,
     Nonexistent::Nat, 0LL, Expect::Nat, 0LL},
    {"America/New_York", 1710037800000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 1800000000000LL, Expect::Ok, 1710054000000000000LL},
    {"America/New_York", 1710037800000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 5400000000000LL, Expect::Ok, 1710057600000000000LL},
    {"America/New_York", 1710036000000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"America/New_York", 1710036000000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftForward, 0LL, Expect::Ok, 1710054000000000000LL},
    {"America/New_York", 1710036000000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftBackward, 0LL, Expect::Ok, 1710053999999999999LL},
    {"America/New_York", 1710036000000000000LL, Ambiguous::Raise,
     Nonexistent::Nat, 0LL, Expect::Nat, 0LL},
    {"America/New_York", 1710036000000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 1800000000000LL, Expect::Error, 0LL},
    {"America/New_York", 1710036000000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 5400000000000LL, Expect::Ok, 1710055800000000000LL},
    {"America/New_York", 1710035999999999999LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Ok, 1710053999999999999LL},
    {"America/New_York", 1710039600000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Ok, 1710054000000000000LL},
    {"America/New_York", 1730595599999999999LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Ok, 1730609999999999999LL},
    {"America/New_York", 1730595600000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"America/New_York", 1730595600000000000LL, Ambiguous::Earliest,
     Nonexistent::Raise, 0LL, Expect::Ok, 1730610000000000000LL},
    {"America/New_York", 1730595600000000000LL, Ambiguous::Latest,
     Nonexistent::Raise, 0LL, Expect::Ok, 1730613600000000000LL},
    {"America/New_York", 1730595600000000000LL, Ambiguous::Nat,
     Nonexistent::Raise, 0LL, Expect::Nat, 0LL},
    {"America/New_York", 1730599199999999999LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"America/New_York", 1730599199999999999LL, Ambiguous::Earliest,
     Nonexistent::Raise, 0LL, Expect::Ok, 1730613599999999999LL},
    {"America/New_York", 1730599199999999999LL, Ambiguous::Latest,
     Nonexistent::Raise, 0LL, Expect::Ok, 1730617199999999999LL},
    {"America/New_York", 1730599199999999999LL, Ambiguous::Nat,
     Nonexistent::Raise, 0LL, Expect::Nat, 0LL},
    {"America/New_York", 1730599200000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Ok, 1730617200000000000LL},
    {"America/New_York", 1719835200500000001LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Ok, 1719849600500000001LL},
    {"America/New_York", 4108674600000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"America/New_York", 4108674600000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftForward, 0LL, Expect::Ok, 4108690800000000000LL},
    {"America/New_York", 4108674600000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftBackward, 0LL, Expect::Ok, 4108690799999999999LL},
    {"America/New_York", 4108674600000000000LL, Ambiguous::Raise,
     Nonexistent::Nat, 0LL, Expect::Nat, 0LL},
    {"America/New_York", 4108674600000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 1800000000000LL, Expect::Ok, 4108690800000000000LL},
    {"America/New_York", 4108674600000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 5400000000000LL, Expect::Ok, 4108694400000000000LL},
    {"America/New_York", 4129234200000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"America/New_York", 4129234200000000000LL, Ambiguous::Earliest,
     Nonexistent::Raise, 0LL, Expect::Ok, 4129248600000000000LL},
    {"America/New_York", 4129234200000000000LL, Ambiguous::Latest,
     Nonexistent::Raise, 0LL, Expect::Ok, 4129252200000000000LL},
    {"America/New_York", 4129234200000000000LL, Ambiguous::Nat,
     Nonexistent::Raise, 0LL, Expect::Nat, 0LL},
    {"America/New_York", -2840140800000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Ok, -2840123038000000000LL},
    {"America/New_York", 1710037800123456789LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"America/New_York", 1710037800123456789LL, Ambiguous::Raise,
     Nonexistent::ShiftForward, 0LL, Expect::Ok, 1710054000000000000LL},
    {"America/New_York", 1710037800123456789LL, Ambiguous::Raise,
     Nonexistent::ShiftBackward, 0LL, Expect::Ok, 1710053999999999999LL},
    {"America/New_York", 1710037800123456789LL, Ambiguous::Raise,
     Nonexistent::Nat, 0LL, Expect::Nat, 0LL},
    {"America/New_York", 1710037800123456789LL, Ambiguous::Raise,
     Nonexistent::Shift, 1800000000000LL, Expect::Ok, 1710054000123456789LL},
    {"America/New_York", 1710037800123456789LL, Ambiguous::Raise,
     Nonexistent::Shift, 5400000000000LL, Expect::Ok, 1710057600123456789LL},
    {"Europe/London", 1711848600000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"Europe/London", 1711848600000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftForward, 0LL, Expect::Ok, 1711846800000000000LL},
    {"Europe/London", 1711848600000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftBackward, 0LL, Expect::Ok, 1711846799999999999LL},
    {"Europe/London", 1711848600000000000LL, Ambiguous::Raise, Nonexistent::Nat,
     0LL, Expect::Nat, 0LL},
    {"Europe/London", 1711848600000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 1800000000000LL, Expect::Ok, 1711846800000000000LL},
    {"Europe/London", 1711848600000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 5400000000000LL, Expect::Ok, 1711850400000000000LL},
    {"Europe/London", 1729992600000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"Europe/London", 1729992600000000000LL, Ambiguous::Earliest,
     Nonexistent::Raise, 0LL, Expect::Ok, 1729989000000000000LL},
    {"Europe/London", 1729992600000000000LL, Ambiguous::Latest,
     Nonexistent::Raise, 0LL, Expect::Ok, 1729992600000000000LL},
    {"Europe/London", 1729992600000000000LL, Ambiguous::Nat, Nonexistent::Raise,
     0LL, Expect::Nat, 0LL},
    {"Europe/London", 1717200000000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Ok, 1717196400000000000LL},
    {"Australia/Lord_Howe", 1712454300000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"Australia/Lord_Howe", 1712454300000000000LL, Ambiguous::Earliest,
     Nonexistent::Raise, 0LL, Expect::Ok, 1712414700000000000LL},
    {"Australia/Lord_Howe", 1712454300000000000LL, Ambiguous::Latest,
     Nonexistent::Raise, 0LL, Expect::Ok, 1712416500000000000LL},
    {"Australia/Lord_Howe", 1712454300000000000LL, Ambiguous::Nat,
     Nonexistent::Raise, 0LL, Expect::Nat, 0LL},
    {"Australia/Lord_Howe", 1728180900000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"Australia/Lord_Howe", 1728180900000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftForward, 0LL, Expect::Ok, 1728142200000000000LL},
    {"Australia/Lord_Howe", 1728180900000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftBackward, 0LL, Expect::Ok, 1728142199999999999LL},
    {"Australia/Lord_Howe", 1728180900000000000LL, Ambiguous::Raise,
     Nonexistent::Nat, 0LL, Expect::Nat, 0LL},
    {"Australia/Lord_Howe", 1728180900000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 1800000000000LL, Expect::Ok, 1728143100000000000LL},
    {"Australia/Lord_Howe", 1728180900000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 5400000000000LL, Expect::Ok, 1728146700000000000LL},
    {"Australia/Lord_Howe", 1728181800000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Ok, 1728142200000000000LL},
    {"Australia/Lord_Howe", 1728179999000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Ok, 1728142199000000000LL},
    {"Pacific/Apia", 1325246400000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"Pacific/Apia", 1325246400000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftForward, 0LL, Expect::Ok, 1325239200000000000LL},
    {"Pacific/Apia", 1325246400000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftBackward, 0LL, Expect::Ok, 1325239199999999999LL},
    {"Pacific/Apia", 1325246400000000000LL, Ambiguous::Raise, Nonexistent::Nat,
     0LL, Expect::Nat, 0LL},
    {"Pacific/Apia", 1325246400000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 1800000000000LL, Expect::Error, 0LL},
    {"Pacific/Apia", 1325246400000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 5400000000000LL, Expect::Error, 0LL},
    {"Pacific/Apia", 1325203200000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"Pacific/Apia", 1325203200000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftForward, 0LL, Expect::Ok, 1325239200000000000LL},
    {"Pacific/Apia", 1325203200000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftBackward, 0LL, Expect::Ok, 1325239199999999999LL},
    {"Pacific/Apia", 1325203200000000000LL, Ambiguous::Raise, Nonexistent::Nat,
     0LL, Expect::Nat, 0LL},
    {"Pacific/Apia", 1325203200000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 1800000000000LL, Expect::Error, 0LL},
    {"Pacific/Apia", 1325203200000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 5400000000000LL, Expect::Error, 0LL},
    {"Pacific/Apia", 1325203199000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Ok, 1325239199000000000LL},
    {"Pacific/Apia", 1325289600000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Ok, 1325239200000000000LL},
    {"America/Sao_Paulo", 1541291400000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"America/Sao_Paulo", 1541291400000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftForward, 0LL, Expect::Ok, 1541300400000000000LL},
    {"America/Sao_Paulo", 1541291400000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftBackward, 0LL, Expect::Ok, 1541300399999999999LL},
    {"America/Sao_Paulo", 1541291400000000000LL, Ambiguous::Raise,
     Nonexistent::Nat, 0LL, Expect::Nat, 0LL},
    {"America/Sao_Paulo", 1541291400000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 1800000000000LL, Expect::Ok, 1541300400000000000LL},
    {"America/Sao_Paulo", 1541291400000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 5400000000000LL, Expect::Ok, 1541304000000000000LL},
    {"America/Sao_Paulo", 1550359800000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"America/Sao_Paulo", 1550359800000000000LL, Ambiguous::Earliest,
     Nonexistent::Raise, 0LL, Expect::Ok, 1550367000000000000LL},
    {"America/Sao_Paulo", 1550359800000000000LL, Ambiguous::Latest,
     Nonexistent::Raise, 0LL, Expect::Ok, 1550370600000000000LL},
    {"America/Sao_Paulo", 1550359800000000000LL, Ambiguous::Nat,
     Nonexistent::Raise, 0LL, Expect::Nat, 0LL},
    {"America/Sao_Paulo", 1550357999000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Ok, 1550365199000000000LL},
    {"America/Sao_Paulo", 1704067200000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Ok, 1704078000000000000LL},
    {"Pacific/Chatham", 1712458800000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"Pacific/Chatham", 1712458800000000000LL, Ambiguous::Earliest,
     Nonexistent::Raise, 0LL, Expect::Ok, 1712409300000000000LL},
    {"Pacific/Chatham", 1712458800000000000LL, Ambiguous::Latest,
     Nonexistent::Raise, 0LL, Expect::Ok, 1712412900000000000LL},
    {"Pacific/Chatham", 1712458800000000000LL, Ambiguous::Nat,
     Nonexistent::Raise, 0LL, Expect::Nat, 0LL},
    {"Pacific/Chatham", 1727578800000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Error, 0LL},
    {"Pacific/Chatham", 1727578800000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftForward, 0LL, Expect::Ok, 1727532000000000000LL},
    {"Pacific/Chatham", 1727578800000000000LL, Ambiguous::Raise,
     Nonexistent::ShiftBackward, 0LL, Expect::Ok, 1727531999999999999LL},
    {"Pacific/Chatham", 1727578800000000000LL, Ambiguous::Raise,
     Nonexistent::Nat, 0LL, Expect::Nat, 0LL},
    {"Pacific/Chatham", 1727578800000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 1800000000000LL, Expect::Error, 0LL},
    {"Pacific/Chatham", 1727578800000000000LL, Ambiguous::Raise,
     Nonexistent::Shift, 5400000000000LL, Expect::Ok, 1727534700000000000LL},
    {"Pacific/Chatham", 1704110400000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Ok, 1704060900000000000LL},
    {"Asia/Kolkata", 1704110400000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Ok, 1704090600000000000LL},
    {"Asia/Kolkata", -2208988800000000000LL, Ambiguous::Raise,
     Nonexistent::Raise, 0LL, Expect::Ok, -2209008070000000000LL},
};

constexpr dstd::StringView FOOTER_RULES[] = {
    "<-02>2<-01>,M3.5.0/-1,M10.5.0/0",
    "IST-1GMT0,M10.5.0,M3.5.0/1",
    "AEST-10AEDT,M10.1.0,M4.1.0/3",
    "<-04>4<-03>,M9.1.6/24,M4.1.6/24",
    "EET-2EEST,M4.5.5/0,M10.5.4/24",
    "<+00>0<+02>-2,M3.5.0/1,M10.5.0/3",
    "CST5CDT,M3.2.0/0,M11.1.0/1",
    "<+0330>-3:30",
    "NZST-12NZDT,M9.5.0,M4.1.0/3",
    "IST-5:30",
    "<-03>3",
};

struct FooterCase {
    int rule;
    std::int64_t utc;
    std::int32_t offset;
};

constexpr FooterCase FOOTER_CASES[] = {
    {0, 2240611200LL, -7200},   {0, 2256249600LL, -3600},
    {0, 2248304399LL, -7200},   {0, 2248304400LL, -3600},
    {0, 2266448399LL, -3600},   {0, 2266448400LL, -7200},
    {0, 4102444800LL, -7200},   {0, 4118083200LL, -3600},
    {0, 4109878799LL, -7200},   {0, 4109878800LL, -3600},
    {0, 4128627599LL, -3600},   {0, 4128627600LL, -7200},
    {1, 2240611200LL, 0},       {1, 2256249600LL, 3600},
    {1, 2248304399LL, 0},       {1, 2248304400LL, 3600},
    {1, 2266448399LL, 3600},    {1, 2266448400LL, 0},
    {1, 4102444800LL, 0},       {1, 4118083200LL, 3600},
    {1, 4109878799LL, 0},       {1, 4109878800LL, 3600},
    {1, 4128627599LL, 3600},    {1, 4128627600LL, 0},
    {2, 2240611200LL, 39600},   {2, 2256249600LL, 36000},
    {2, 2248876799LL, 39600},   {2, 2248876800LL, 36000},
    {2, 2264601599LL, 36000},   {2, 2264601600LL, 39600},
    {2, 4102444800LL, 39600},   {2, 4118083200LL, 36000},
    {2, 4110451199LL, 39600},   {2, 4110451200LL, 36000},
    {2, 4126175999LL, 36000},   {2, 4126176000LL, 39600},
    {3, 2240611200LL, -10800},  {3, 2256249600LL, -14400},
    {3, 2248916399LL, -10800},  {3, 2248916400LL, -14400},
    {3, 2262225599LL, -14400},  {3, 2262225600LL, -10800},
    {3, 4102444800LL, -10800},  {3, 4118083200LL, -14400},
    {3, 4110490799LL, -10800},  {3, 4110490800LL, -14400},
    {3, 4123799999LL, -14400},  {3, 4123800000LL, -10800},
    {4, 2240611200LL, 7200},    {4, 2256249600LL, 10800},
    {4, 2250539999LL, 7200},    {4, 2250540000LL, 10800},
    {4, 2266865999LL, 10800},   {4, 2266866000LL, 7200},
    {4, 4102444800LL, 7200},    {4, 4118083200LL, 10800},
    {4, 4112719199LL, 7200},    {4, 4112719200LL, 10800},
    {4, 4128440399LL, 10800},   {4, 4128440400LL, 7200},
    {5, 2240611200LL, 0},       {5, 2256249600LL, 7200},
    {5, 2248304399LL, 0},       {5, 2248304400LL, 7200},
    {5, 2266448399LL, 7200},    {5, 2266448400LL, 0},
    {5, 4102444800LL, 0},       {5, 4118083200LL, 7200},
    {5, 4109878799LL, 0},       {5, 4109878800LL, 7200},
    {5, 4128627599LL, 7200},    {5, 4128627600LL, 0},
    {6, 2240611200LL, -18000},  {6, 2256249600LL, -14400},
    {6, 2246504399LL, -18000},  {6, 2246504400LL, -14400},
    {6, 2267067599LL, -14400},  {6, 2267067600LL, -18000},
    {6, 4102444800LL, -18000},  {6, 4118083200LL, -14400},
    {6, 4108683599LL, -18000},  {6, 4108683600LL, -14400},
    {6, 4129246799LL, -14400},  {6, 4129246800LL, -18000},
    {7, 2240611200LL, 12600},   {7, 2256249600LL, 12600},
    {7, 4102444800LL, 12600},   {7, 4118083200LL, 12600},
    {8, 2240611200LL, 46800},   {8, 2256249600LL, 43200},
    {8, 2248869599LL, 46800},   {8, 2248869600LL, 43200},
    {8, 2263989599LL, 43200},   {8, 2263989600LL, 46800},
    {8, 4102444800LL, 46800},   {8, 4118083200LL, 43200},
    {8, 4110443999LL, 46800},   {8, 4110444000LL, 43200},
    {8, 4125563999LL, 43200},   {8, 4125564000LL, 46800},
    {9, 2240611200LL, 19800},   {9, 2256249600LL, 19800},
    {9, 4102444800LL, 19800},   {9, 4118083200LL, 19800},
    {10, 2240611200LL, -10800}, {10, 2256249600LL, -10800},
    {10, 4102444800LL, -10800}, {10, 4118083200LL, -10800},
};

struct RuleCase {
    std::int64_t utc;
    std::int32_t offset;
};

constexpr RuleCase JULIAN_1[] = {
    {1677653999LL, -18000}, {1677654000LL, -14400}, {1698386399LL, -14400},
    {1698386400LL, -18000}, {1709276399LL, -18000}, {1709276400LL, -14400},
    {1730008799LL, -14400}, {1730008800LL, -18000}, {1740812399LL, -18000},
    {1740812400LL, -14400}, {1761544799LL, -14400}, {1761544800LL, -18000},
};
constexpr RuleCase ZERO_BASED[] = {
    {1677659399LL, -18000}, {1677659400LL, -14400}, {1698375599LL, -14400},
    {1698375600LL, -18000}, {1709195399LL, -18000}, {1709195400LL, -14400},
    {1729911599LL, -14400}, {1729911600LL, -18000}, {1740817799LL, -18000},
    {1740817800LL, -14400}, {1761533999LL, -14400}, {1761534000LL, -18000},
};
constexpr RuleCase WRAP_YEAR[] = {
    {1672545599LL, -14400}, {1672545600LL, -18000}, {1703998799LL, -18000},
    {1703998800LL, -14400}, {1704081599LL, -14400}, {1704081600LL, -18000},
    {1735621199LL, -18000}, {1735621200LL, -14400}, {1735703999LL, -14400},
    {1735704000LL, -18000}, {1767157199LL, -18000}, {1767157200LL, -14400},
};

std::int64_t per_second(TimeUnit unit) {
    switch (unit) {
        case TimeUnit::Second:
            return 1;
        case TimeUnit::Milli:
            return 1'000;
        case TimeUnit::Micro:
            return 1'000'000;
        case TimeUnit::Nano:
            return 1'000'000'000;
    }
    return 1;
}

struct Converted {
    bool ok = false;
    bool valid = false;
    std::int64_t value = 0;
    dstd::String error;
};

Converted convert(const tz::Zone& zone, dstd::Span<const std::int64_t> wall,
                  TimeUnit unit, const tz::ToUtcOptions& options,
                  dstd::Vector<std::int64_t>& out,
                  dstd::Vector<std::uint8_t>& valid) {
    out.assign(wall.size(), -1);
    valid.assign(wall.size(), 2);
    auto r = zone.to_utc(wall, unit, options,
                         dstd::Span<std::int64_t>(out.data(), out.size()),
                         dstd::Span<std::uint8_t>(valid.data(), valid.size()));
    Converted c;
    c.ok = r.has_value();
    if (!c.ok) c.error = dstd::String(r.error().message());
    return c;
}

Converted convert_one(const tz::Zone& zone, std::int64_t wall, TimeUnit unit,
                      const tz::ToUtcOptions& options) {
    dstd::Vector<std::int64_t> out;
    dstd::Vector<std::uint8_t> valid;
    const std::int64_t one[1] = {wall};
    Converted c = convert(zone, dstd::Span<const std::int64_t>(one, 1), unit,
                          options, out, valid);
    if (c.ok) {
        c.valid = valid[0] == 1;
        c.value = out[0];
    }
    return c;
}

std::uint64_t next_random(std::uint64_t& state) {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

dstd::String temp_dir(dstd::StringView leaf) {
    dstd::String out(dstd::fs::temp_directory().str());
    out += "/dftu_tz_";
    out += leaf;
    out += "_";
    out += dstd::to_string(::getpid());
    return out;
}

void remove_tree(dstd::StringView root) {
    [[maybe_unused]] const auto removed = dstd::fs::remove_all(root);
}

void write_zone(dstd::StringView root, dstd::StringView name,
                dstd::StringView bytes) {
    const dstd::Path file = dstd::Path(root) / name;
    const dstd::Path parent(file.str().substr(0, file.str().rfind('/')));
    REQUIRE(dstd::fs::create_directories(parent).has_value());
    REQUIRE(dstd::fs::write_file(file, bytes).has_value());
}

dstd::String read_fixture(dstd::StringView name) {
    auto bytes =
        dstd::fs::read_file(dstd::Path(DFTRACER_UTILS_TZ_DATA_DIR) / name);
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

void put32(dstd::String& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<char>((v >> shift) & 0xFF));
}

// A version 1 TZif file with the given transitions and local time types
// (offset, is_dst, abbreviation) and no footer.
struct V1Type {
    std::int32_t offset;
    bool is_dst;
    const char* abbreviation;
};

dstd::String make_v1(dstd::Span<const std::int32_t> times,
                     dstd::Span<const std::uint8_t> kinds,
                     dstd::Span<const V1Type> types) {
    dstd::String chars;
    dstd::Vector<std::uint8_t> index;
    for (const V1Type& t : types) {
        index.push_back(static_cast<std::uint8_t>(chars.size()));
        chars.append(t.abbreviation);
        chars.push_back('\0');
    }
    dstd::String out("TZif");
    out.push_back('\0');
    out.append(15, '\0');
    put32(out, 0);
    put32(out, 0);
    put32(out, 0);
    put32(out, static_cast<std::uint32_t>(times.size()));
    put32(out, static_cast<std::uint32_t>(types.size()));
    put32(out, static_cast<std::uint32_t>(chars.size()));
    for (const std::int32_t t : times)
        put32(out, static_cast<std::uint32_t>(t));
    for (const std::uint8_t k : kinds) out.push_back(static_cast<char>(k));
    for (std::size_t i = 0; i < types.size(); ++i) {
        put32(out, static_cast<std::uint32_t>(types[i].offset));
        out.push_back(types[i].is_dst ? 1 : 0);
        out.push_back(static_cast<char>(index[i]));
    }
    out.append(chars);
    return out;
}

}  // namespace

TEST_SUITE("tz") {
    TEST_CASE("offsets, DST flags and abbreviations match zoneinfo") {
        for (const OffsetCase& c : OFFSET_CASES) {
            const tz::Zone zone = fixture(c.zone);
            const tz::Segment s = zone.segment_at(c.utc);
            CAPTURE(c.zone);
            CAPTURE(c.utc);
            CHECK(s.offset == c.offset);
            CHECK(zone.offset_at(c.utc) == c.offset);
            CHECK(s.begin <= c.utc);
            CHECK(c.utc < s.end);
            CHECK(zone.types()[s.type].is_dst == c.is_dst);
            CHECK(zone.types()[s.type].abbreviation == c.abbreviation);
        }
    }

    TEST_CASE("a segment ends where the next one begins") {
        const tz::Zone zone = fixture("America/New_York");
        for (const std::int64_t t :
             {std::int64_t{-3'000'000'000}, std::int64_t{1'710'053'999},
              std::int64_t{4'118'083'200}, std::int64_t{16'740'864'000}}) {
            const tz::Segment s = zone.segment_at(t);
            REQUIRE(s.end != INT64_HIGH);
            const tz::Segment next = zone.segment_at(s.end);
            CHECK(next.begin == s.end);
            CHECK(next.offset != s.offset);
            const tz::Segment back = zone.segment_at(s.end - 1);
            CHECK(back.begin == s.begin);
            CHECK(back.offset == s.offset);
        }
        const tz::Segment first = zone.segment_at(INT64_LOW);
        CHECK(first.begin == INT64_LOW);
        CHECK(first.offset == -17762);
    }

    TEST_CASE("the footer gives the slim file its rules after the table") {
        const tz::Zone zone = fixture("America/New_York");
        CHECK(zone.transition_count() < 250);
        CHECK(zone.offset_at(1'909'094'400) == -14400);
        CHECK(zone.offset_at(4'102'444'800 + 86400 * 180) == -14400);
    }

    TEST_CASE("footer rules alone give the offsets of the real zone") {
        for (const FooterCase& c : FOOTER_CASES) {
            auto zone = tz::zone_from_posix("rule", FOOTER_RULES[c.rule]);
            REQUIRE_MESSAGE(zone.has_value(), FOOTER_RULES[c.rule]);
            CAPTURE(FOOTER_RULES[c.rule]);
            CAPTURE(c.utc);
            CHECK(zone->offset_at(c.utc) == c.offset);
        }
    }

    TEST_CASE("Jn, n and wrapping year rules") {
        struct Rules {
            dstd::StringView text;
            dstd::Span<const RuleCase> cases;
        };
        const Rules rules[] = {
            {"EST5EDT,J60,J300", JULIAN_1},
            {"EST5EDT,59/3:30,299/-1", ZERO_BASED},
            {"EST5EDT,J365/0,J1/0", WRAP_YEAR},
        };
        for (const Rules& r : rules) {
            auto zone = tz::zone_from_posix("rule", r.text);
            REQUIRE_MESSAGE(zone.has_value(), r.text);
            for (const RuleCase& c : r.cases) {
                CAPTURE(r.text);
                CAPTURE(c.utc);
                CHECK(zone->offset_at(c.utc) == c.offset);
            }
        }
    }

    TEST_CASE("a POSIX rule names the zone it builds") {
        auto zone = tz::zone_from_posix("India", "IST-5:30");
        REQUIRE(zone.has_value());
        CHECK(zone->name() == "India");
        CHECK(zone->offset_at(0) == 19800);
        CHECK(zone->types()[0].abbreviation == "IST");
        CHECK_FALSE(zone->types()[0].is_dst);
        const tz::Segment s = zone->segment_at(123);
        CHECK(s.begin == INT64_LOW);
        CHECK(s.end == INT64_HIGH);

        auto quoted = tz::zone_from_posix("x", "<+0330>-3:30");
        REQUIRE(quoted.has_value());
        CHECK(quoted->offset_at(5) == 12600);
        CHECK(quoted->types()[0].abbreviation == "+0330");
    }

    TEST_CASE("a bad POSIX rule is a parse error") {
        const dstd::StringView bad[] = {
            "",
            "EST",
            "EST5EDT",
            "EST5EDT,M3.2.0",
            "EST5EDT,M13.1.0,M11.1.0",
            "EST5EDT,M3.6.0,M11.1.0",
            "EST5EDT,M3.2.7,M11.1.0",
            "EST5EDT,J0,J5",
            "EST5EDT,J366,J5",
            "EST5EDT,M3.2.0/168,M11.1.0",
            "EST25",
            "<EST5",
            "<>5",
            "ES5",
            "EST5EDT,M3.2.0,M11.1.0,X",
            "EST5:60",
        };
        for (const dstd::StringView text : bad) {
            auto zone = tz::zone_from_posix("x", text);
            CAPTURE(text);
            REQUIRE_FALSE(zone.has_value());
            CHECK(has_code(zone.error(), common::ErrorCode::Parse));
        }
    }

    TEST_CASE("TZif versions 1 to 4") {
        SUBCASE("a version 2 file read as version 3 and 4") {
            for (const char version : {'3', '4'}) {
                dstd::String bytes = read_fixture("America/New_York");
                REQUIRE(bytes[4] == '2');
                bytes[4] = version;
                const std::size_t second = bytes.find("TZif", 4);
                REQUIRE(second != dstd::String::npos);
                bytes[second + 4] = version;
                auto zone = tz::parse_tzif("America/New_York", as_bytes(bytes));
                REQUIRE(zone.has_value());
                CHECK(zone->offset_at(1'710'053'999) == -18000);
                CHECK(zone->offset_at(1'710'054'000) == -14400);
                CHECK(zone->offset_at(4'118'083'200) == -14400);
            }
        }
        SUBCASE("version 1 has the 32-bit block only and no footer") {
            const std::int32_t times[] = {1000, 2000};
            const std::uint8_t kinds[] = {1, 0};
            const V1Type types[] = {{0, false, "AAA"}, {3600, true, "BBB"}};
            const dstd::String bytes = make_v1(times, kinds, types);
            auto zone = tz::parse_tzif("v1", as_bytes(bytes));
            REQUIRE(zone.has_value());
            CHECK(zone->transition_count() == 2);
            CHECK(zone->offset_at(-5) == 0);
            CHECK(zone->offset_at(999) == 0);
            CHECK(zone->offset_at(1000) == 3600);
            CHECK(zone->offset_at(1999) == 3600);
            CHECK(zone->offset_at(2000) == 0);
            CHECK(zone->offset_at(1'000'000'000) == 0);
            const tz::Segment s = zone->segment_at(1500);
            CHECK(s.begin == 1000);
            CHECK(s.end == 2000);
            CHECK(zone->types()[s.type].abbreviation == "BBB");
            CHECK(zone->types()[s.type].is_dst);
            const tz::Segment last = zone->segment_at(5000);
            CHECK(last.begin == 2000);
            CHECK(last.end == INT64_HIGH);
        }
        SUBCASE("a file with no transitions has one offset") {
            const V1Type types[] = {{-7200, false, "ZZZ"}};
            const dstd::String bytes = make_v1({}, {}, types);
            auto zone = tz::parse_tzif("one", as_bytes(bytes));
            REQUIRE(zone.has_value());
            CHECK(zone->offset_at(0) == -7200);
            CHECK(zone->offset_at(INT64_HIGH) == -7200);
            CHECK(zone->offset_at(INT64_LOW) == -7200);
        }
    }

    TEST_CASE("a damaged TZif file is a parse error") {
        const dstd::String good = read_fixture("Asia/Tokyo");
        SUBCASE("bad magic") {
            dstd::String bytes = good;
            bytes[0] = 'X';
            auto zone = tz::parse_tzif("x", as_bytes(bytes));
            REQUIRE_FALSE(zone.has_value());
            CHECK(has_code(zone.error(), common::ErrorCode::Parse));
            CHECK(mentions(zone.error(), "'x'"));
        }
        SUBCASE("every truncation") {
            // A version 2 file ends with its footer, so every shorter file is
            // damaged, also one cut right after the 64-bit block.
            for (std::size_t n = 0; n < good.size(); ++n) {
                const dstd::String cut = good.substr(0, n);
                auto zone = tz::parse_tzif("x", as_bytes(cut));
                CAPTURE(n);
                REQUIRE_FALSE(zone.has_value());
                CHECK(has_code(zone.error(), common::ErrorCode::Parse));
            }
        }
        SUBCASE("unknown version") {
            dstd::String bytes = good;
            bytes[4] = '9';
            auto zone = tz::parse_tzif("x", as_bytes(bytes));
            REQUIRE_FALSE(zone.has_value());
            CHECK(has_code(zone.error(), common::ErrorCode::Parse));
        }
        SUBCASE("a transition type that does not exist") {
            const std::int32_t times[] = {1000};
            const std::uint8_t kinds[] = {5};
            const V1Type types[] = {{0, false, "AAA"}};
            const dstd::String bytes = make_v1(times, kinds, types);
            auto zone = tz::parse_tzif("x", as_bytes(bytes));
            REQUIRE_FALSE(zone.has_value());
            CHECK(has_code(zone.error(), common::ErrorCode::Parse));
        }
        SUBCASE("transitions out of order") {
            const std::int32_t times[] = {2000, 1000};
            const std::uint8_t kinds[] = {0, 0};
            const V1Type types[] = {{0, false, "AAA"}};
            const dstd::String bytes = make_v1(times, kinds, types);
            auto zone = tz::parse_tzif("x", as_bytes(bytes));
            REQUIRE_FALSE(zone.has_value());
            CHECK(has_code(zone.error(), common::ErrorCode::Parse));
        }
        SUBCASE("an offset that is more than a day and a bit") {
            const V1Type types[] = {{200'000, false, "AAA"}};
            const dstd::String bytes = make_v1({}, {}, types);
            auto zone = tz::parse_tzif("x", as_bytes(bytes));
            REQUIRE_FALSE(zone.has_value());
            CHECK(has_code(zone.error(), common::ErrorCode::Parse));
        }
    }

    TEST_CASE("a mutated TZif file parses or fails and never crashes") {
        const std::int64_t probes[] = {
            INT64_LOW,       -3'000'000'000LL, 0,
            1'710'054'000LL, 4'118'083'200LL,  INT64_HIGH};
        const auto exercise = [&](dstd::StringView bytes) {
            auto zone = tz::parse_tzif("fuzz", as_bytes(bytes));
            if (!zone.has_value()) return;
            for (const std::int64_t t : probes) {
                const tz::Segment s = zone->segment_at(t);
                REQUIRE(s.type < zone->types().size());
                (void)zone->resolve_local(t);
            }
            const std::int64_t wall[] = {0, 1'710'037'800LL, 1'730'597'400LL};
            std::int64_t out[3];
            std::uint8_t valid[3];
            tz::ToUtcOptions nat;
            nat.ambiguous = Ambiguous::Nat;
            nat.nonexistent = Nonexistent::Nat;
            (void)zone->to_utc(dstd::Span<const std::int64_t>(wall, 3),
                               TimeUnit::Second, nat,
                               dstd::Span<std::int64_t>(out, 3),
                               dstd::Span<std::uint8_t>(valid, 3));
        };
        for (const char* name :
             {"America/New_York", "Asia/Tokyo", "Australia/Lord_Howe"}) {
            const dstd::String good = read_fixture(name);
            for (std::size_t n = 0; n < good.size(); ++n)
                exercise(good.substr(0, n));
            for (std::size_t at = 0; at < good.size(); ++at)
                for (const unsigned char flip :
                     {std::uint8_t{0x01}, std::uint8_t{0x80},
                      std::uint8_t{0xFF}}) {
                    dstd::String bytes = good;
                    bytes[at] = static_cast<char>(
                        static_cast<unsigned char>(bytes[at]) ^ flip);
                    exercise(bytes);
                }
            // Both headers keep six counts at bytes 20 to 43 of the header.
            const std::size_t second = good.find("TZif", 4);
            REQUIRE(second != dstd::String::npos);
            for (const std::size_t header : {std::size_t{0}, second})
                for (std::size_t field = 0; field < 6; ++field)
                    for (const std::uint32_t big :
                         {0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0x10000u,
                          0x01000000u}) {
                        dstd::String bytes = good;
                        const std::size_t at = header + 20 + field * 4;
                        for (int k = 0; k < 4; ++k)
                            bytes[at + static_cast<std::size_t>(k)] =
                                static_cast<char>((big >> (24 - 8 * k)) & 0xFF);
                        exercise(bytes);
                    }
        }
    }

    TEST_CASE("zone names are checked before any file is opened") {
        CHECK(tz::is_valid_zone_name("America/New_York"));
        CHECK(tz::is_valid_zone_name("Etc/GMT+5"));
        CHECK(tz::is_valid_zone_name("America/Port-au-Prince"));
        CHECK(tz::is_valid_zone_name("GMT0"));
        for (const dstd::StringView bad :
             {"", "/etc/passwd", "../etc/passwd", "America/../../x", "..",
              "America//New_York", "America/", "a.b", "A B", "a\\b",
              "caf\xC3\xA9", "a:b", "Zone\n"})
            CHECK_FALSE(tz::is_valid_zone_name(bad));

        const dstd::Vector<dstd::String> dirs = fixture_dirs();
        for (const dstd::StringView bad :
             {"../etc/passwd", "/etc/passwd", "America//New_York", "A.B"}) {
            auto zone = tz::load_zone(bad, dirs);
            CAPTURE(bad);
            REQUIRE_FALSE(zone.has_value());
            CHECK(has_code(zone.error(), common::ErrorCode::InvalidArgument));
            CHECK(mentions(zone.error(), bad));
        }
    }

    TEST_CASE("fixed offsets and the UTC names need no file") {
        const dstd::Vector<dstd::String> none;
        const struct {
            const char* name;
            std::int32_t offset;
            const char* abbreviation;
        } fixed[] = {
            {"UTC", 0, "UTC"},
            {"utc", 0, "UTC"},
            {"Z", 0, "UTC"},
            {"GMT", 0, "GMT"},
            {"Etc/UTC", 0, "UTC"},
            {"Zulu", 0, "UTC"},
            {"+05:30", 19800, "UTC+05:30"},
            {"-0800", -28800, "UTC-08:00"},
            {"UTC-03:00", -10800, "UTC-03:00"},
            {"UTC+05:45", 20700, "UTC+05:45"},
            {"+23:59", 86340, "UTC+23:59"},
            {"-00:30", -1800, "UTC-00:30"},
        };
        for (const auto& f : fixed) {
            auto zone = tz::load_zone(f.name, none);
            CAPTURE(f.name);
            REQUIRE(zone.has_value());
            CHECK(zone->name() == f.name);
            CHECK(zone->offset_at(0) == f.offset);
            CHECK(zone->offset_at(INT64_LOW) == f.offset);
            CHECK(zone->offset_at(INT64_HIGH) == f.offset);
            CHECK(zone->types()[0].abbreviation == f.abbreviation);
            CHECK_FALSE(zone->types()[0].is_dst);
        }
        for (const dstd::StringView bad :
             {"+24:00", "+05:60", "+5:30", "+05", "-8", "UTC+3", "05:30",
              "+05:3", "+05-30"}) {
            auto zone = tz::load_zone(bad, none);
            CAPTURE(bad);
            REQUIRE_FALSE(zone.has_value());
        }
    }

    TEST_CASE(
        "an unknown zone and a missing database name what they searched") {
        const dstd::Vector<dstd::String> dirs = fixture_dirs();
        auto unknown = tz::load_zone("Mars/Olympus_Mons", dirs);
        REQUIRE_FALSE(unknown.has_value());
        CHECK(has_code(unknown.error(), common::ErrorCode::NotFound));
        CHECK(mentions(unknown.error(), "Mars/Olympus_Mons"));
        CHECK(mentions(unknown.error(), DFTRACER_UTILS_TZ_DATA_DIR));

        auto directory = tz::load_zone("America", dirs);
        REQUIRE_FALSE(directory.has_value());
        CHECK(has_code(directory.error(), common::ErrorCode::NotFound));

        dstd::Vector<dstd::String> missing;
        missing.emplace_back("/no/such/zoneinfo/dir");
        auto none = tz::load_zone("America/New_York", missing);
        REQUIRE_FALSE(none.has_value());
        CHECK(has_code(none.error(), common::ErrorCode::NotFound));
        CHECK(mentions(none.error(), "no time zone database"));
        CHECK(mentions(none.error(), "/no/such/zoneinfo/dir"));
    }

    TEST_CASE("the first directory that holds the zone wins") {
        const dstd::String root = temp_dir("order");
        const dstd::String a = root + "/a";
        const dstd::String b = root + "/b";
        const dstd::String empty = root + "/empty";
        write_zone(a, "Test/Zone", read_fixture("Asia/Tokyo"));
        write_zone(b, "Test/Zone", read_fixture("Asia/Kolkata"));
        REQUIRE(dstd::fs::create_directories(empty).has_value());

        const auto offset_in =
            [](std::initializer_list<const dstd::String*> d) {
                dstd::Vector<dstd::String> dirs;
                for (const dstd::String* one : d) dirs.push_back(*one);
                auto zone = tz::load_zone("Test/Zone", dirs);
                REQUIRE(zone.has_value());
                return zone->offset_at(0);
            };
        CHECK(offset_in({&a, &b}) == 32400);
        CHECK(offset_in({&b, &a}) == 19800);
        CHECK(offset_in({&empty, &b, &a}) == 19800);
        CHECK(offset_in({&empty, &a}) == 32400);
        remove_tree(root);
    }

    TEST_CASE("a damaged file in the first directory is an error, not a skip") {
        const dstd::String root = temp_dir("damaged");
        const dstd::String a = root + "/a";
        write_zone(a, "Test/Broken", dstd::String("not a zone file at all"));
        write_zone(root + "/b", "Test/Broken", read_fixture("Asia/Tokyo"));
        dstd::Vector<dstd::String> dirs;
        dirs.push_back(a);
        dirs.push_back(root + "/b");
        auto zone = tz::load_zone("Test/Broken", dirs);
        REQUIRE_FALSE(zone.has_value());
        CHECK(has_code(zone.error(), common::ErrorCode::Parse));
        remove_tree(root);
    }

    TEST_CASE(
        "the search path: set list, DFTRACER_UTILS_TZDIR, TZDIR, system") {
        const dstd::String root = temp_dir("path");
        const dstd::String set_dir = root + "/set";
        const dstd::String tzdir = root + "/tzdir";
        const dstd::String ours = root + "/ours";
        write_zone(set_dir, "Test/Cached", read_fixture("Asia/Tokyo"));
        write_zone(ours, "Test/FromEnv", read_fixture("Asia/Kolkata"));
        write_zone(tzdir, "Test/FromTzdir", read_fixture("Pacific/Chatham"));

        ::unsetenv("DFTRACER_UTILS_TZDIR");
        ::unsetenv("TZDIR");
        const dstd::String list = set_dir + ":" + root + "/second";
        dftu_tz_set_search_path(list.data(),
                                static_cast<std::int32_t>(list.size()));
        dstd::Vector<dstd::String> dirs = tz::search_dirs();
        REQUIRE(dirs.size() >= 3);
        CHECK(dirs[0] == set_dir);
        CHECK(dirs[1] == root + "/second");
        CHECK(dirs[2] == "/usr/share/zoneinfo");
        CHECK(dirs.back() == "/etc/zoneinfo");

        ::setenv("DFTRACER_UTILS_TZDIR", ours.c_str(), 1);
        ::setenv("TZDIR", tzdir.c_str(), 1);
        dirs = tz::search_dirs();
        REQUIRE(dirs.size() >= 5);
        CHECK(dirs[0] == set_dir);
        CHECK(dirs[1] == root + "/second");
        CHECK(dirs[2] == ours);
        CHECK(dirs[3] == tzdir);
        CHECK(dirs[4] == "/usr/share/zoneinfo");

        auto cached = tz::zone("Test/Cached");
        REQUIRE(cached.has_value());
        CHECK((*cached)->offset_at(0) == 32400);
        auto again = tz::zone("Test/Cached");
        REQUIRE(again.has_value());
        CHECK(*again == *cached);

        auto from_env = tz::zone("Test/FromEnv");
        REQUIRE(from_env.has_value());
        CHECK((*from_env)->offset_at(0) == 19800);
        auto from_tzdir = tz::zone("Test/FromTzdir");
        REQUIRE(from_tzdir.has_value());
        CHECK((*from_tzdir)->offset_at(1'704'067'200) == 49500);

        auto missing = tz::zone("Test/NotThere");
        REQUIRE_FALSE(missing.has_value());
        CHECK(has_code(missing.error(), common::ErrorCode::NotFound));
        CHECK(mentions(missing.error(), "Test/NotThere"));

        dftu_tz_set_search_path(nullptr, 0);
        dirs = tz::search_dirs();
        REQUIRE(dirs.size() >= 3);
        CHECK(dirs[0] == ours);
        CHECK(dirs[1] == tzdir);
        // A zone loaded earlier stays.
        auto kept = tz::zone("Test/Cached");
        REQUIRE(kept.has_value());
        CHECK(*kept == *cached);

        ::unsetenv("DFTRACER_UTILS_TZDIR");
        ::unsetenv("TZDIR");
        remove_tree(root);
    }

    TEST_CASE("the cache builds one Zone per name under concurrent callers") {
        dftu_tz_set_search_path(
            DFTRACER_UTILS_TZ_DATA_DIR,
            static_cast<std::int32_t>(
                dstd::StringView(DFTRACER_UTILS_TZ_DATA_DIR).size()));
        constexpr int THREADS = 8;
        const tz::Zone* seen[THREADS] = {};
        dstd::Vector<dstd::Thread> pool;
        for (int i = 0; i < THREADS; ++i)
            pool.emplace_back([&seen, i] {
                auto zone = tz::zone("Europe/London");
                seen[i] = zone.has_value() ? *zone : nullptr;
            });
        for (dstd::Thread& t : pool) t.join();
        REQUIRE(seen[0] != nullptr);
        for (int i = 1; i < THREADS; ++i) CHECK(seen[i] == seen[0]);
        CHECK(seen[0]->offset_at(1'719'835'200) == 3600);
        dftu_tz_set_search_path(nullptr, 0);
    }

    TEST_CASE("the batch offset matches the single lookup in every unit") {
        const char* const names[] = {"America/New_York", "Australia/Lord_Howe",
                                     "Pacific/Apia", "Asia/Kolkata"};
        std::uint64_t rng = 42;
        for (const char* name : names) {
            const tz::Zone zone = fixture(name);
            for (const TimeUnit unit : {TimeUnit::Second, TimeUnit::Milli,
                                        TimeUnit::Micro, TimeUnit::Nano}) {
                const std::int64_t per = per_second(unit);
                const std::int64_t limit = INT64_HIGH / per;
                dstd::Vector<std::int64_t> ticks;
                for (int i = 0; i < 4000; ++i) {
                    const std::int64_t seconds =
                        static_cast<std::int64_t>(
                            next_random(rng) %
                            static_cast<std::uint64_t>(std::min<std::int64_t>(
                                limit, 7'000'000'000LL))) -
                        std::min<std::int64_t>(limit, 3'500'000'000LL);
                    ticks.push_back(seconds * per +
                                    static_cast<std::int64_t>(
                                        next_random(rng) %
                                        static_cast<std::uint64_t>(per)));
                }
                for (int sorted = 0; sorted < 2; ++sorted) {
                    if (sorted) std::sort(ticks.begin(), ticks.end());
                    dstd::Vector<std::int32_t> out(ticks.size());
                    zone.offsets_at(
                        dstd::Span<const std::int64_t>(ticks.data(),
                                                       ticks.size()),
                        unit, dstd::Span<std::int32_t>(out.data(), out.size()));
                    for (std::size_t i = 0; i < ticks.size(); ++i) {
                        const std::int64_t seconds =
                            ticks[i] >= 0 ? ticks[i] / per
                                          : -((-ticks[i] + per - 1) / per);
                        REQUIRE_MESSAGE(out[i] == zone.offset_at(seconds),
                                        name);
                    }
                }
            }
        }
    }

    TEST_CASE("the batch offset at the edges of a tick range") {
        const tz::Zone zone = fixture("America/New_York");
        const std::int64_t ticks[] = {1'710'053'999'999'999'999LL,
                                      1'710'054'000'000'000'000LL,
                                      -1,
                                      INT64_LOW,
                                      INT64_HIGH,
                                      1'710'053'999'999'999'999LL};
        std::int32_t out[6];
        zone.offsets_at(dstd::Span<const std::int64_t>(ticks, 6),
                        TimeUnit::Nano, dstd::Span<std::int32_t>(out, 6));
        CHECK(out[0] == -18000);
        CHECK(out[1] == -14400);
        CHECK(out[2] == -18000);
        CHECK(out[3] == -17762);
        CHECK(out[4] == -14400);
        CHECK(out[5] == -18000);
    }

    TEST_CASE("wall time to UTC follows pandas") {
        for (const WallCase& c : WALL_CASES) {
            const tz::Zone zone = fixture(c.zone);
            tz::ToUtcOptions options;
            options.ambiguous = c.ambiguous;
            options.nonexistent = c.nonexistent;
            options.shift = c.shift_ns;
            const Converted got =
                convert_one(zone, c.wall_ns, TimeUnit::Nano, options);
            CAPTURE(c.zone);
            CAPTURE(c.wall_ns);
            CAPTURE(static_cast<int>(c.ambiguous));
            CAPTURE(static_cast<int>(c.nonexistent));
            CAPTURE(c.shift_ns);
            switch (c.expect) {
                case Expect::Error:
                    CHECK_FALSE(got.ok);
                    break;
                case Expect::Nat:
                    REQUIRE(got.ok);
                    CHECK_FALSE(got.valid);
                    CHECK(got.value == 0);
                    break;
                case Expect::Ok:
                    REQUIRE(got.ok);
                    CHECK(got.valid);
                    CHECK(got.value == c.utc_ns);
                    break;
            }
        }
    }

    TEST_CASE("the other units keep the sub-second part") {
        const tz::Zone zone = fixture("America/New_York");
        tz::ToUtcOptions latest;
        latest.ambiguous = Ambiguous::Latest;
        tz::ToUtcOptions earliest;
        earliest.ambiguous = Ambiguous::Earliest;
        // 2024-11-03 01:30:00.123456 wall time, twice.
        Converted c =
            convert_one(zone, 1'730'597'400'123'456LL, TimeUnit::Micro, latest);
        REQUIRE(c.ok);
        CHECK(c.value == 1'730'615'400'123'456LL);
        c = convert_one(zone, 1'730'597'400'123LL, TimeUnit::Milli, earliest);
        REQUIRE(c.ok);
        CHECK(c.value == 1'730'611'800'123LL);
        c = convert_one(zone, 1'730'597'400LL, TimeUnit::Second, earliest);
        REQUIRE(c.ok);
        CHECK(c.value == 1'730'611'800LL);

        tz::ToUtcOptions forward;
        forward.nonexistent = Nonexistent::ShiftForward;
        // 2024-03-10 02:30:00.9 does not exist: the first instant after the
        // gap is 03:00:00 EDT, and its sub-second part is dropped.
        c = convert_one(zone, 1'710'037'800'900'000LL, TimeUnit::Micro,
                        forward);
        REQUIRE(c.ok);
        CHECK(c.value == 1'710'054'000'000'000LL);
        tz::ToUtcOptions backward;
        backward.nonexistent = Nonexistent::ShiftBackward;
        c = convert_one(zone, 1'710'037'800'900LL, TimeUnit::Milli, backward);
        REQUIRE(c.ok);
        CHECK(c.value == 1'710'053'999'999LL);
        tz::ToUtcOptions shift;
        shift.nonexistent = Nonexistent::Shift;
        shift.shift = 3600'000'000LL;
        c = convert_one(zone, 1'710'037'800'250'000LL, TimeUnit::Micro, shift);
        REQUIRE(c.ok);
        CHECK(c.value == 1'710'055'800'250'000LL);
    }

    TEST_CASE("an error names the first bad row") {
        const tz::Zone zone = fixture("America/New_York");
        dstd::Vector<std::int64_t> out;
        dstd::Vector<std::uint8_t> valid;
        // Row 0 and 1 are fine, row 2 is in the gap and row 3 is ambiguous.
        const std::int64_t wall[] = {1'710'000'000, 1'710'030'000,
                                     1'710'037'800, 1'730'597'400};
        Converted c = convert(zone, dstd::Span<const std::int64_t>(wall, 4),
                              TimeUnit::Second, {}, out, valid);
        REQUIRE_FALSE(c.ok);
        CHECK(c.error.find("nonexistent") != dstd::String::npos);
        CHECK(c.error.find("row 2") != dstd::String::npos);

        const std::int64_t amb_first[] = {1'730'597'400, 1'710'037'800};
        c = convert(zone, dstd::Span<const std::int64_t>(amb_first, 2),
                    TimeUnit::Second, {}, out, valid);
        REQUIRE_FALSE(c.ok);
        CHECK(c.error.find("ambiguous") != dstd::String::npos);
        CHECK(c.error.find("row 0") != dstd::String::npos);
    }

    TEST_CASE("infer takes the first instant until the wall time steps back") {
        const tz::Zone zone = fixture("America/New_York");
        tz::ToUtcOptions infer;
        infer.ambiguous = Ambiguous::Infer;
        dstd::Vector<std::int64_t> out;
        dstd::Vector<std::uint8_t> valid;
        SUBCASE("one repeated hour") {
            const std::int64_t wall[] = {
                1730593800000000000LL, 1730596200000000000LL,
                1730598600000000000LL, 1730596200000000000LL,
                1730598600000000000LL, 1730599800000000000LL};
            // pandas 3.1: DatetimeIndex.tz_localize("America/New_York",
            // ambiguous="infer").
            const std::int64_t expected[] = {
                1730608200000000000LL, 1730610600000000000LL,
                1730613000000000000LL, 1730614200000000000LL,
                1730616600000000000LL, 1730617800000000000LL};
            const Converted c =
                convert(zone, dstd::Span<const std::int64_t>(wall, 6),
                        TimeUnit::Nano, infer, out, valid);
            REQUIRE(c.ok);
            for (int i = 0; i < 6; ++i) {
                CHECK(valid[i] == 1);
                CHECK(out[i] == expected[i]);
            }
        }
        SUBCASE("a run with no repeat cannot be inferred") {
            // pandas: "2024-11-03 01:10:00 is an ambiguous time and cannot be
            // inferred."
            const std::int64_t wall[] = {1730596200000000000LL,
                                         1730598600000000000LL,
                                         1730599800000000000LL};
            const Converted c =
                convert(zone, dstd::Span<const std::int64_t>(wall, 3),
                        TimeUnit::Nano, infer, out, valid);
            REQUIRE_FALSE(c.ok);
            CHECK(c.error.find("row 0") != dstd::String::npos);
        }
        SUBCASE("two steps back in one run") {
            // pandas: "There are 2 dst switches when there should only be 1."
            const std::int64_t wall[] = {
                1730596200000000000LL, 1730598600000000000LL,
                1730596200000000000LL, 1730598600000000000LL,
                1730596200000000000LL};
            const Converted c =
                convert(zone, dstd::Span<const std::int64_t>(wall, 5),
                        TimeUnit::Nano, infer, out, valid);
            REQUIRE_FALSE(c.ok);
            CHECK(c.error.find("more than one") != dstd::String::npos);
        }
        SUBCASE("consecutive ambiguous rows of two years are one run") {
            // pandas splits runs at a row that is not ambiguous, not at a
            // transition, so the second year adds a second step back.
            const std::int64_t wall[] = {
                1709985600000000000LL, 1730596200000000000LL,
                1730598600000000000LL, 1730596200000000000LL,
                1730598600000000000LL, 1762047000000000000LL,
                1762047000000000000LL};
            const Converted c =
                convert(zone, dstd::Span<const std::int64_t>(wall, 7),
                        TimeUnit::Nano, infer, out, valid);
            REQUIRE_FALSE(c.ok);
        }
        SUBCASE("a row that is not ambiguous ends the run") {
            const std::int64_t wall[] = {
                1730596200000000000LL, 1730598600000000000LL,
                1730596200000000000LL, 1730598600000000000LL,
                1730603000000000000LL,  // 03:03:20 EST, unique
                1762045800000000000LL, 1762048200000000000LL,
                1762045800000000000LL, 1762048200000000000LL};
            const Converted c =
                convert(zone, dstd::Span<const std::int64_t>(wall, 9),
                        TimeUnit::Nano, infer, out, valid);
            REQUIRE(c.ok);
            const std::int64_t edt = 14'400'000'000'000LL;
            const std::int64_t est = 18'000'000'000'000LL;
            CHECK(out[0] == wall[0] + edt);
            CHECK(out[1] == wall[1] + edt);
            CHECK(out[2] == wall[2] + est);
            CHECK(out[3] == wall[3] + est);
            CHECK(out[4] == wall[4] + est);
            CHECK(out[5] == wall[5] + edt);
            CHECK(out[6] == wall[6] + edt);
            CHECK(out[7] == wall[7] + est);
            CHECK(out[8] == wall[8] + est);
        }
        SUBCASE("Lord_Howe repeats half an hour") {
            const tz::Zone howe = fixture("Australia/Lord_Howe");
            const std::int64_t wall[] = {
                1712454000000000000LL, 1712454600000000000LL,
                1712454000000000000LL, 1712454600000000000LL};
            const std::int64_t expected[] = {
                1712414400000000000LL, 1712415000000000000LL,
                1712416200000000000LL, 1712416800000000000LL};
            const Converted c =
                convert(howe, dstd::Span<const std::int64_t>(wall, 4),
                        TimeUnit::Nano, infer, out, valid);
            REQUIRE(c.ok);
            for (int i = 0; i < 4; ++i) CHECK(out[i] == expected[i]);
        }
    }

    TEST_CASE("wall times at the Int64 limits in seconds never wrap") {
        const auto utc = [](const tz::Zone& zone, std::int64_t wall) {
            return convert_one(zone, wall, TimeUnit::Second, {});
        };
        const dstd::Vector<dstd::String> none;
        auto utc_zone = tz::load_zone("UTC", none);
        auto east = tz::load_zone("+05:30", none);
        REQUIRE(utc_zone.has_value());
        REQUIRE(east.has_value());
        const tz::Zone ny = fixture("America/New_York");

        Converted c = utc(*utc_zone, INT64_LOW);
        REQUIRE(c.ok);
        CHECK(c.value == INT64_LOW);
        c = utc(*utc_zone, INT64_HIGH);
        REQUIRE(c.ok);
        CHECK(c.value == INT64_HIGH);

        CHECK_FALSE(utc(*east, INT64_LOW).ok);
        CHECK_FALSE(utc(*east, INT64_LOW + 19799).ok);
        c = utc(*east, INT64_LOW + 19800);
        REQUIRE(c.ok);
        CHECK(c.value == INT64_LOW);
        c = utc(*east, INT64_HIGH);
        REQUIRE(c.ok);
        CHECK(c.value == INT64_HIGH - 19800);

        // Before 1883 America/New_York is 17762 s behind UTC, and past the
        // table it is 18000 s behind.
        c = utc(ny, INT64_LOW);
        REQUIRE(c.ok);
        CHECK(c.value == INT64_LOW + 17762);
        CHECK_FALSE(utc(ny, INT64_HIGH).ok);
        CHECK_FALSE(utc(ny, INT64_HIGH - 17999).ok);
        c = utc(ny, INT64_HIGH - 18000);
        REQUIRE(c.ok);
        CHECK(c.value == INT64_HIGH);

        // Neither the offsets nor the resolution of a wall time near the
        // limits may overflow.
        for (const std::int64_t w :
             {INT64_LOW, INT64_LOW + 1, INT64_HIGH - 1, INT64_HIGH}) {
            const tz::Resolution r = ny.resolve_local(w);
            CHECK(r.kind != tz::Resolution::Kind::Ambiguous);
        }
    }

    TEST_CASE("a wall time that does not fit the tick range is an error") {
        const tz::Zone tokyo = fixture("Asia/Tokyo");
        Converted c = convert_one(tokyo, INT64_LOW + 5, TimeUnit::Nano, {});
        REQUIRE_FALSE(c.ok);
        CHECK(c.error.find("out of range") != dstd::String::npos);
        const tz::Zone ny = fixture("America/New_York");
        c = convert_one(ny, INT64_HIGH - 5, TimeUnit::Nano, {});
        REQUIRE_FALSE(c.ok);
        c = convert_one(tokyo, INT64_HIGH - 5, TimeUnit::Nano, {});
        REQUIRE(c.ok);
        CHECK(c.value == INT64_HIGH - 5 - 32400LL * 1'000'000'000LL);
    }

    TEST_CASE("every result is a preimage and every miss is not unique") {
        const char* const names[] = {"America/New_York",    "Europe/London",
                                     "Australia/Lord_Howe", "Pacific/Chatham",
                                     "Pacific/Apia",        "America/Sao_Paulo",
                                     "Asia/Kolkata"};
        std::uint64_t rng = 7;
        for (const char* name : names) {
            const tz::Zone zone = fixture(name);
            // Walls around random transitions and anywhere in 1880 to 2120.
            dstd::Vector<std::int64_t> wall;
            for (int i = 0; i < 6000; ++i) {
                std::int64_t t = static_cast<std::int64_t>(next_random(rng) %
                                                           7'600'000'000ULL) -
                                 2'800'000'000LL;
                if (i % 2 == 0) {
                    const tz::Segment s = zone.segment_at(t);
                    if (s.end != INT64_HIGH) t = s.end;
                }
                wall.push_back(
                    t + static_cast<std::int64_t>(next_random(rng) % 14401) -
                    7200);
            }
            const auto preimages = [&](std::int64_t w) {
                dstd::Vector<std::int64_t> found;
                for (const tz::LocalType& type : zone.types()) {
                    const std::int64_t u = w - type.offset;
                    if (zone.offset_at(u) == type.offset &&
                        std::find(found.begin(), found.end(), u) == found.end())
                        found.push_back(u);
                }
                std::sort(found.begin(), found.end());
                return found;
            };

            tz::ToUtcOptions nat;
            nat.ambiguous = Ambiguous::Nat;
            nat.nonexistent = Nonexistent::Nat;
            tz::ToUtcOptions earliest;
            earliest.ambiguous = Ambiguous::Earliest;
            earliest.nonexistent = Nonexistent::Nat;
            tz::ToUtcOptions latest;
            latest.ambiguous = Ambiguous::Latest;
            latest.nonexistent = Nonexistent::Nat;
            for (const bool sorted : {false, true}) {
                if (sorted) std::sort(wall.begin(), wall.end());
                const dstd::Span<const std::int64_t> all(wall.data(),
                                                         wall.size());
                dstd::Vector<std::int64_t> a, b, c;
                dstd::Vector<std::uint8_t> va, vb, vc;
                REQUIRE(convert(zone, all, TimeUnit::Second, nat, a, va).ok);
                REQUIRE(
                    convert(zone, all, TimeUnit::Second, earliest, b, vb).ok);
                REQUIRE(convert(zone, all, TimeUnit::Second, latest, c, vc).ok);
                for (std::size_t i = 0; i < wall.size(); ++i) {
                    const dstd::Vector<std::int64_t> truth = preimages(wall[i]);
                    CAPTURE(name);
                    CAPTURE(wall[i]);
                    if (truth.size() == 1) {
                        REQUIRE(va[i] == 1);
                        CHECK(a[i] == truth[0]);
                        CHECK(b[i] == truth[0]);
                        CHECK(c[i] == truth[0]);
                    } else if (truth.size() >= 2) {
                        CHECK(va[i] == 0);
                        REQUIRE(vb[i] == 1);
                        REQUIRE(vc[i] == 1);
                        CHECK(b[i] == truth.front());
                        CHECK(c[i] == truth.back());
                    } else {
                        CHECK(va[i] == 0);
                        CHECK(vb[i] == 0);
                        CHECK(vc[i] == 0);
                    }
                }
            }
        }
    }

    TEST_CASE("shifting a gap by a duration reads the offset after the gap") {
        const tz::Zone zone = fixture("Pacific/Apia");
        tz::ToUtcOptions shift;
        shift.nonexistent = Nonexistent::Shift;
        // 2011-12-30 12:00 does not exist. A day of shift lands on the 31st.
        shift.shift = 86400LL * 1'000'000'000LL;
        Converted c = convert_one(zone, 1'325'246'400'000'000'000LL,
                                  TimeUnit::Nano, shift);
        REQUIRE(c.ok);
        CHECK(c.value == 1'325'246'400'000'000'000LL +
                             86400LL * 1'000'000'000LL -
                             50400LL * 1'000'000'000LL);
        // Staying in the gap is an error.
        shift.shift = 3600LL * 1'000'000'000LL;
        c = convert_one(zone, 1'325'246'400'000'000'000LL, TimeUnit::Nano,
                        shift);
        CHECK_FALSE(c.ok);
    }
}
