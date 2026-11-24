// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/internal/tz.h>
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>

#include <iterator>
#include <mutex>

namespace dftracer::utils::dataframe::tz {

namespace {

constexpr dstd::StringView SYSTEM_DIRS[] = {
    "/usr/share/zoneinfo", "/usr/lib/zoneinfo", "/usr/share/lib/zoneinfo",
    "/etc/zoneinfo"};

constexpr dstd::StringView UTC_NAMES[] = {
    "UTC",     "utc",  "Z",        "Etc/UTC",   "UCT",
    "Etc/UCT", "Zulu", "Etc/Zulu", "Universal", "Etc/Universal"};
constexpr dstd::StringView GMT_NAMES[] = {"GMT", "Etc/GMT", "Greenwich",
                                          "Etc/Greenwich"};

struct State {
    dstd::Deque<Zone> zones;
    dstd::StringViewMap<const Zone*> by_name;
    dstd::Vector<dstd::String> user_dirs;
};

struct Cache {
    std::mutex mutex;
    dstd::Optional<State> state;
};

Cache& cache() {
    Cache& held = dstd::Singleton<Cache>::get();
    [[maybe_unused]] static const bool hooked = [] {
        dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, []() noexcept {
            Cache& dropped = dstd::Singleton<Cache>::get();
            const std::lock_guard<std::mutex> lock(dropped.mutex);
            dropped.state.reset();
        });
        return true;
    }();
    return held;
}

State& state(Cache& c) {
    if (!c.state) c.state.emplace();
    return *c.state;
}

// The separator of a directory list, as PATH: ';' where a path may hold a
// ':' after a drive letter.
#if defined(_WIN32)
constexpr char LIST_SEPARATOR = ';';
#else
constexpr char LIST_SEPARATOR = ':';
#endif

void split_dirs(dstd::StringView list, dstd::Vector<dstd::String>& out) {
    while (!list.empty()) {
        const std::size_t cut = list.find(LIST_SEPARATOR);
        const dstd::StringView one = list.substr(0, cut);
        if (!one.empty()) out.emplace_back(one);
        if (cut == dstd::StringView::npos) break;
        list.remove_prefix(cut + 1);
    }
}

dstd::Vector<dstd::String> dirs_of(const State& s) {
    dstd::Vector<dstd::String> dirs = s.user_dirs;
    if (const auto env = common::Env::get(common::constants::TZDIR_ENV))
        split_dirs(*env, dirs);
    if (const auto env = common::Env::get("TZDIR")) split_dirs(*env, dirs);
    for (const dstd::StringView dir : SYSTEM_DIRS) dirs.emplace_back(dir);
    return dirs;
}

bool in_list(dstd::StringView name, const dstd::StringView* first,
             const dstd::StringView* last) {
    for (; first != last; ++first)
        if (*first == name) return true;
    return false;
}

// "+05:30", "-0800" and the same after "UTC", as pandas reads them. Hours and
// minutes are two digits each, and the offset is within a day.
bool parse_fixed_offset(dstd::StringView name, std::int32_t& offset) {
    if (name.starts_with("UTC")) name.remove_prefix(3);
    if (name.size() < 5 || (name[0] != '+' && name[0] != '-')) return false;
    const std::int32_t sign = name[0] == '-' ? -1 : 1;
    name.remove_prefix(1);
    const auto two = [&](std::size_t at) -> std::int32_t {
        if (!dstd::is_ascii_digit(name[at]) ||
            !dstd::is_ascii_digit(name[at + 1]))
            return -1;
        return (name[at] - '0') * 10 + (name[at + 1] - '0');
    };
    std::int32_t hours = 0;
    std::int32_t minutes = 0;
    if (name.size() == 5 && name[2] == ':') {
        hours = two(0);
        minutes = two(3);
    } else if (name.size() == 4) {
        hours = two(0);
        minutes = two(2);
    } else {
        return false;
    }
    if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) return false;
    offset = sign * (hours * 3600 + minutes * 60);
    return true;
}

dstd::String fixed_abbreviation(std::int32_t offset) {
    dstd::String out("UTC");
    out.push_back(offset < 0 ? '-' : '+');
    const std::int32_t magnitude = offset < 0 ? -offset : offset;
    const std::int32_t hours = magnitude / 3600;
    const std::int32_t minutes = magnitude % 3600 / 60;
    out.push_back(static_cast<char>('0' + hours / 10));
    out.push_back(static_cast<char>('0' + hours % 10));
    out.push_back(':');
    out.push_back(static_cast<char>('0' + minutes / 10));
    out.push_back(static_cast<char>('0' + minutes % 10));
    return out;
}

dstd::String joined(dstd::Span<const dstd::String> dirs) {
    dstd::String out;
    for (const dstd::String& dir : dirs) {
        if (!out.empty()) out.push_back(LIST_SEPARATOR);
        out.append(dir);
    }
    return out;
}

}  // namespace

namespace {

struct LastFailure {
    Failure kind = Failure::None;
    dstd::String message;
};

LastFailure& last_failure() {
    static thread_local LastFailure t_failure;
    return t_failure;
}

bool has(dstd::StringView text, dstd::StringView part) {
    return text.find(part) != dstd::StringView::npos;
}

}  // namespace

void record_failure(Failure kind, dstd::StringView message) {
    LastFailure& f = last_failure();
    f.kind = kind;
    f.message.assign(message);
}

void record_failure(const dstd::Error& error) {
    const dstd::StringView text = error.message();
    Failure kind = Failure::Other;
    if (common::errorcode_of(error) == common::ErrorCode::NotFound)
        kind = Failure::UnknownZone;
    else if (has(text, "invalid time zone name"))
        kind = Failure::InvalidZone;
    else if (has(text, "ambiguous"))
        kind = Failure::Ambiguous;
    else if (has(text, "nonexistent"))
        kind = Failure::Nonexistent;
    else if (has(text, "out of range"))
        kind = Failure::OutOfRange;
    record_failure(kind, text);
}

void clear_failure() {
    LastFailure& f = last_failure();
    f.kind = Failure::None;
    f.message.clear();
}

Failure take_failure(dstd::String* message) {
    LastFailure& f = last_failure();
    const Failure kind = f.kind;
    if (message) *message = std::move(f.message);
    dstd::String().swap(f.message);
    f.kind = Failure::None;
    return kind;
}

bool is_valid_zone_name(dstd::StringView name) noexcept {
    if (name.empty() || name.front() == '/' || name.back() == '/') return false;
    std::size_t component = 0;
    for (std::size_t i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == '/') {
            if (i == component) return false;
            component = i + 1;
            continue;
        }
        const char c = name[i];
        if (!dstd::is_ascii_alnum(c) && c != '_' && c != '+' && c != '-')
            return false;
    }
    return true;
}

void set_search_path(dstd::StringView paths) {
    Cache& c = cache();
    const std::lock_guard<std::mutex> lock(c.mutex);
    State& s = state(c);
    s.user_dirs.clear();
    split_dirs(paths, s.user_dirs);
}

dstd::Vector<dstd::String> search_dirs() {
    Cache& c = cache();
    const std::lock_guard<std::mutex> lock(c.mutex);
    return dirs_of(state(c));
}

dstd::Result<Zone> load_zone(dstd::StringView name,
                             dstd::Span<const dstd::String> dirs) {
    if (in_list(name, std::begin(UTC_NAMES), std::end(UTC_NAMES)))
        return fixed_zone(name, 0, "UTC");
    if (in_list(name, std::begin(GMT_NAMES), std::end(GMT_NAMES)))
        return fixed_zone(name, 0, "GMT");
    std::int32_t offset = 0;
    if (parse_fixed_offset(name, offset))
        return fixed_zone(name, offset, fixed_abbreviation(offset));
    if (!is_valid_zone_name(name))
        return common::make_error<"invalid time zone name '{}'">(
            common::ErrorCode::InvalidArgument, name);

    bool any_dir = false;
    for (const dstd::String& dir : dirs) {
        const auto is_dir = dstd::fs::is_directory(dir);
        if (!is_dir.has_value() || !*is_dir) continue;
        any_dir = true;
        const dstd::Path file = dstd::Path(dir) / name;
        const auto regular = dstd::fs::is_regular_file(file);
        if (!regular.has_value() || !*regular) continue;
        auto bytes = dstd::fs::read_file(file);
        if (!bytes)
            return common::make_error<"time zone '{}': cannot read {}: {}">(
                common::ErrorCode::Io, name, file.str(),
                bytes.error().message());
        return parse_tzif(
            name, dstd::Span<const std::uint8_t>(
                      reinterpret_cast<const std::uint8_t*>(bytes->data()),
                      bytes->size()));
    }
    if (!any_dir)
        return common::make_error<
            "no time zone database found for '{}'; searched {}">(
            common::ErrorCode::NotFound, name, joined(dirs));
    return common::make_error<"unknown time zone '{}'; searched {}">(
        common::ErrorCode::NotFound, name, joined(dirs));
}

dstd::Result<const Zone*> zone(dstd::StringView name) {
    Cache& c = cache();
    const std::lock_guard<std::mutex> lock(c.mutex);
    State& s = state(c);
    if (const auto it = s.by_name.find(name); it != s.by_name.end())
        return it->second;
    const dstd::Vector<dstd::String> dirs = dirs_of(s);
    auto loaded = load_zone(
        name, dstd::Span<const dstd::String>(dirs.data(), dirs.size()));
    if (!loaded) return dstd::Unexpected<dstd::Error>(loaded.error());
    s.zones.push_back(std::move(*loaded));
    const Zone* held = &s.zones.back();
    s.by_name.emplace(dstd::String(name), held);
    return held;
}

}  // namespace dftracer::utils::dataframe::tz
