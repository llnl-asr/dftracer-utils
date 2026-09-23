// Example dftracer-utils plugin: counts events matching a predicate built with
// the ergonomic F/Field builder, e.g. `F("dur") > 300`. The plugin includes
// only plugin.h and links nothing; the host parses the rendered DSL string.
//
// Build: c++ -std=c++20 -shared -fPIC -I<repo>/include \
//            -o query_filter.so query_filter.cpp
// Run:   dftracer_run -d ./traces --plugin ./query_filter.so

#include <dftracer/utils/plugins/plugin.h>

#include <cstdint>
#include <cstdio>

using namespace dftracer::utils::plugins;

struct QueryFilter {
    std::uint64_t total = 0;
    std::uint64_t matched = 0;

    explicit QueryFilter(const Config&) {}

    void step(const Batch& b, Host h) {
        dftu_query* q = h.query_compile(F("dur") > 300);
        for (const Event& e : b) {
            ++total;
            if (q && h.query_matches(q, e.frame(), e.row())) ++matched;
        }
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
            h.log(DFTU_LOG_INFO,
                  std::string_view(line, static_cast<std::size_t>(n)));
    }
};

extern "C" DFTU_PLUGIN_EXPORT dftu_plugin* dftracer_plugin(dftu_plugin_host* h, const dftu_value* config) {
    (void)h;
    return make_plugin<QueryFilter>(config);
}
