// SPDX-License-Identifier: MIT
// Example dftracer-utils plugin: counts events matching a predicate built with
// the ergonomic F/Field builder, e.g. `F("dur") > 300`. The plugin includes
// only plugin.h and links nothing; the host parses the rendered DSL string.
//
// Build: c++ -std=c++20 -shared -fPIC -I<repo>/include -o query_filter.so
// query_filter.cpp Run:   dftracer_run ./traces --plugin ./query_filter.so

#include <dftracer/utils/plugins/plugin.h>

#include <cstdint>
#include <cstdio>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

struct QueryFilter {
    std::uint64_t total = 0;
    std::uint64_t matched = 0;

    explicit QueryFilter(const Config&) {}

    dftracer::utils::dstd::Result<void> step(const Batch& b, Host h) {
        DFTRACER_UTILS_TRY(dftu_duql * q, h.duql_compile(F("dur") > 300));
        for (const Event& e : b) {
            ++total;
            if (h.duql_matches(q, e.frame(), e.row())) ++matched;
        }
        return ok();
    }

    void merge(QueryFilter& other) {
        total += other.total;
        matched += other.matched;
    }

    void finalize(Host h) {
        char line[128];
        int n = std::snprintf(line, sizeof line,
                              "query_filter: matched=%llu total=%llu",
                              static_cast<unsigned long long>(matched),
                              static_cast<unsigned long long>(total));
        if (n > 0)
            h.log(DFTU_LOG_INFO, dftracer::utils::dstd::StringView(
                                     line, static_cast<std::size_t>(n)));
    }
};

DFTU_PLUGIN("query_filter", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                (void)config;
                p.fold<QueryFilter>();
                return {};
            })
