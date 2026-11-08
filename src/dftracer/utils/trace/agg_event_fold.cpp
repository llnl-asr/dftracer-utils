// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/hash/hex64.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial_update.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/trace/agg_event_fold.h>
#include <dftracer/utils/trace/internal/utils.h>

#include <algorithm>

namespace dftracer::utils::trace {

namespace {

constexpr char EXTRA_SEP = '\x1f';

}  // namespace

std::size_t AggGroupHash::operator()(const AggGroup& g) const noexcept {
    using dftracer::utils::dstd::hash::splitmix64;
    return static_cast<std::size_t>(splitmix64(
        splitmix64(splitmix64((std::uint64_t{g.cat} << 32) | g.name) ^
                   ((std::uint64_t{g.host} << 32) | g.extra)) ^
        splitmix64(g.file ^ (std::uint64_t{g.file_text} << 8) ^
                   static_cast<std::uint64_t>(g.kind))));
}

void set_file_key(AggKey& key, dstd::StringView fhash,
                  dstd::StringIntern& strings) {
    if (fhash.empty()) return;
    key.file = index::schemas::dft::agg::agg_hash_of_hex(fhash);
    if (!dstd::hash::parse_hex64(fhash))
        key.file_text = strings.get_or_insert(fhash);
}

AggEventRules::AggEventRules(const AggQuery& query,
                             dstd::StringIntern& read_intern,
                             dstd::StringIntern& strings_intern)
    : q(query),
      file_rows(query.by_file && query.only_file_keys &&
                !query.filter.has_value() && !query.from_trace &&
                !query.by_pid && !query.by_tid && !query.by_host &&
                !query.by_extra && query.categories.empty() &&
                query.names.empty() && query.hosts.empty()),
      strings(strings_intern),
      read(read_intern),
      read_metric_ids(read),
      window_cfg(query.config),
      read_cfg(query.config) {
    for (const auto& c : q.categories) {
        dstd::String storage;
        categories.emplace(
            dstd::String(trace::internal::to_lower_ascii(c, storage)));
    }
    for (const auto& n : q.names) names.emplace(n);
    for (const auto& h : q.hosts) hosts.emplace(h);
    pids.assign(q.pids.begin(), q.pids.end());
    std::sort(pids.begin(), pids.end());
    window_cfg.time_interval_us = q.window_us;
    read_cfg.compute_percentiles = q.quantiles;
}

std::uint32_t AggEventFolder::read_cat(std::uint32_t id) {
    return build_cat_.get(id, [&](std::uint32_t i) {
        const dstd::StringView raw = i == dstd::StringIntern::NO_ID
                                         ? dstd::StringView{}
                                         : r_.read.resolve(i);
        dstd::String storage;
        const dstd::StringView lower =
            trace::internal::to_lower_ascii(raw, storage);
        if (!r_.categories.empty() &&
            r_.categories.find(lower) == r_.categories.end())
            return AGG_REJECT;
        return r_.strings.get_or_insert(lower);
    });
}

std::uint32_t AggEventFolder::read_name(std::uint32_t id) {
    return build_name_.get(id, [&](std::uint32_t i) {
        const dstd::StringView raw = i == dstd::StringIntern::NO_ID
                                         ? dstd::StringView{}
                                         : r_.read.resolve(i);
        if (!r_.names.empty() && r_.names.find(raw) == r_.names.end())
            return AGG_REJECT;
        return r_.strings.get_or_insert(raw);
    });
}

std::uint32_t AggEventFolder::read_host(std::uint32_t id) {
    return build_host_.get(id, [&](std::uint32_t i) {
        const dstd::StringView raw = i == dstd::StringIntern::NO_ID
                                         ? dstd::StringView{}
                                         : r_.read.resolve(i);
        if (!r_.hosts.empty() && r_.hosts.find(raw) == r_.hosts.end())
            return AGG_REJECT;
        return r_.strings.get_or_insert(raw);
    });
}

template <class Acc>
std::uint32_t AggEventFolder::extra_id(const Acc& acc) {
    extra_buf_.clear();
    bool any = false;
    bool first = true;
    for (const auto& k : r_.q.config.extra_group_keys) {
        if (!first) extra_buf_ += EXTRA_SEP;
        first = false;
        const auto v = acc.arg_string(k);
        if (!v.empty()) any = true;
        extra_buf_.append(v);
    }
    return any ? r_.strings.get_or_insert(extra_buf_)
               : dstd::StringIntern::NO_ID;
}

void AggEventFolder::add_event(
    const views::detail::FoldEvent& ev,
    const dstd::HashSet<AggGroup, AggGroupHash>* wanted) {
    const index::schemas::dft::agg::PodAccessor acc(ev, r_.read,
                                                    r_.read_metric_ids);
    if (!index::schemas::dft::agg::partial_holds(acc)) return;
    if (ev.ts < r_.q.ts_begin || ev.ts >= r_.q.ts_end) return;
    if (!r_.pids.empty() &&
        !std::binary_search(r_.pids.begin(), r_.pids.end(), ev.pid))
        return;
    constexpr std::uint32_t NO_ID = dstd::StringIntern::NO_ID;
    std::uint32_t cat = NO_ID;
    std::uint32_t name = NO_ID;
    std::uint32_t host = NO_ID;
    std::uint32_t extra = NO_ID;
    if (!r_.file_rows) {
        cat = read_cat(ev.cat_id);
        if (cat == AGG_REJECT) return;
        name = read_name(ev.name_id);
        if (name == AGG_REJECT) return;
        host = read_host(ev.hhash_id);
        if (host == AGG_REJECT) return;
        extra = extra_id(acc);
    }
    const AggKind kind = index::schemas::dft::agg::partial_kind(acc);
    AggKey key;
    if (r_.q.by_file) set_file_key(key, acc.arg_string("fhash"), r_.strings);
    if (wanted != nullptr) {
        AggGroup group{cat, name, host, extra, kind};
        if (r_.file_rows) {
            group.file = key.file;
            group.file_text = key.file_text;
        }
        if (wanted->find(group) == wanted->end()) return;
    }

    key.cat = cat;
    key.name = name;
    if (r_.q.by_host) key.host = host;
    if (r_.q.by_kind) key.kind = kind;
    if (r_.q.by_pid) key.pid = ev.pid;
    if (r_.q.by_tid) key.tid = ev.tid;
    if (r_.q.window_us != 0) key.window = r_.bucket(ev.ts);
    if (r_.q.by_extra) key.extra = extra;

    auto it = rows_.find(key);
    if (it == rows_.end())
        it = rows_
                 .emplace(key, index::schemas::dft::agg::AggPartial(
                                   r_.q.config.sketch_accuracy))
                 .first;
    index::schemas::dft::agg::fold_into_partial(it->second, acc, r_.read_cfg);
    ++stats_.events_read;
}

}  // namespace dftracer::utils::trace
