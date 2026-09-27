#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/json/json.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/trace/args_map.h>
#include <dftracer/utils/trace/event.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/statistics/detail_stats_view.h>
#include <simdjson.h>

#include <charconv>
#include <string>
#include <string_view>
#include <unordered_set>

using dftracer::utils::json::JsonValue;
using dftracer::utils::trace::DFTracerEvent;

namespace dftracer::utils::trace::statistics {

inline constexpr std::string_view GLOBAL_GROUP_KEY = "__global__";

static bool is_io_event(std::string_view name) {
    using namespace dftracer::utils::trace::internal;
    for (auto op : posix_ops::FILE_READ)
        if (op == name) return true;
    for (auto op : posix_ops::FILE_WRITE)
        if (op == name) return true;
    return false;
}

static void build_group_key(std::string& key,
                            const std::vector<std::string>& group_by,
                            const DFTracerEvent& ev) {
    key.clear();
    for (std::size_t i = 0; i < group_by.size(); ++i) {
        if (i > 0) key.push_back('|');
        const auto& dim = group_by[i];
        if (dim == "name") {
            key += ev.name;
        } else if (dim == "cat") {
            key += ev.cat;
        } else if (dim == "pid") {
            char buf[32];
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), ev.pid);
            if (ec == std::errc()) key.append(buf, ptr - buf);
        } else if (dim == "tid") {
            char buf[32];
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), ev.tid);
            if (ec == std::errc()) key.append(buf, ptr - buf);
        } else if (dim == "pid_tid") {
            char buf[64];
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), ev.pid);
            if (ec == std::errc()) {
                key.append(buf, ptr - buf);
                key.push_back(':');
                auto [ptr2, ec2] =
                    std::to_chars(ptr, buf + sizeof(buf), ev.tid);
                if (ec2 == std::errc()) key.append(ptr, ptr2 - ptr);
            }
        } else if (dim == "fhash") {
            if (ev.args.exists())
                key += ev.args["fhash"].get<std::string_view>();
        } else if (dim == "hhash") {
            if (ev.args.exists())
                key += ev.args["hhash"].get<std::string_view>();
        }
    }
}

static void update_event(
    DetailedStatistics& acc, const DFTracerEvent& ev,
    const std::vector<std::string>* group_by,
    const std::unordered_set<std::string_view>& name_filter,
    const std::unordered_set<std::string_view>& cat_filter,
    std::string& group_key_buf) {
    static const std::string global_key{GLOBAL_GROUP_KEY};

    if (ev.is_metadata()) return;
    if (!name_filter.empty() && name_filter.find(ev.name) == name_filter.end())
        return;
    if (!cat_filter.empty() && cat_filter.find(ev.cat) == cat_filter.end())
        return;

    const double dur = static_cast<double>(ev.dur);
    acc.duration.update(dur);

    const bool has_grouping = group_by && !group_by->empty();
    const std::string* io_key_ptr;
    if (has_grouping) {
        build_group_key(group_key_buf, *group_by, ev);
        acc.grouped_duration[group_key_buf].update(dur);
        acc.group_key_category.try_emplace(group_key_buf, ev.cat);
        io_key_ptr = &group_key_buf;
    } else {
        io_key_ptr = &global_key;
    }

    if (is_io_event(ev.name) && ev.args.exists()) {
        auto ret_opt = ev.args["ret"].get_optional<std::int64_t>();
        if (ret_opt.has_value() && ret_opt.value() > 0) {
            const double ret = static_cast<double>(ret_opt.value());
            auto& io = acc.grouped_io[*io_key_ptr];
            io.duration.update(dur);
            io.size.update(ret);
            if (dur > 0) io.bandwidth.update(ret * 1e6 / dur);
            auto offset_opt = ev.args["offset"].get_optional<std::uint64_t>();
            if (offset_opt.has_value())
                io.offset.update(static_cast<double>(offset_opt.value()));
        }
    }

    acc.events_scanned++;
}

coro::CoroTask<DetailedStatistics> DetailStatsView::collect(
    const DetailNeeds& needs) const {
    std::unordered_set<std::string_view> name_filter;
    std::unordered_set<std::string_view> cat_filter;
    if (needs.filter_names)
        for (const auto& n : *needs.filter_names) name_filter.insert(n);
    if (needs.filter_categories)
        for (const auto& c : *needs.filter_categories) cat_filter.insert(c);

    const std::vector<std::string>* group_by = needs.group_by;
    std::size_t slots = needs.num_slots ? needs.num_slots : 4;

    auto fold = [group_by, &name_filter, &cat_filter](
                    DetailedStatistics& acc,
                    const std::vector<std::string_view>& lines) {
        dftracer::utils::json::RecordParser parser;
        std::string group_key_buf;
        group_key_buf.reserve(128);
        for (std::string_view line : lines) {
            auto result = parser.parse(line.data(), line.size());
            if (result.error()) continue;
            auto root = result.value_unsafe();
            if (!root.is_object()) continue;
            JsonValue json(root);
            DFTracerEvent ev;
            if (!DFTracerEvent::parse(json, ev)) continue;
            update_event(acc, ev, group_by, name_filter, cat_filter,
                         group_key_buf);
        }
    };
    auto combine = [](DetailedStatistics&& a,
                      DetailedStatistics&& b) -> DetailedStatistics {
        a.merge(b);
        return std::move(a);
    };

    views::View v = view();
    if (needs.query) v = v.filter(*needs.query);
    auto res = co_await v.map_batches<DetailedStatistics>(
        std::move(fold), std::move(combine), slots);
    res.value.chunks_scanned = res.stats.chunks_scanned;
    res.value.chunks_skipped = res.stats.chunks_skipped;
    co_return std::move(res.value);
}

}  // namespace dftracer::utils::trace::statistics
