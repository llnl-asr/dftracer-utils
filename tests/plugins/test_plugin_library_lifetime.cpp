// SPDX-License-Identifier: MIT
// The host keeps a plugin library loaded while a plan holds a node or provider
// from it. Unloading the Plugins set removes the names at once. The node's and
// provider's destroy run when the plan is gone, then the plugin's own destroy,
// then the library closes.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dlfcn.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace dftracer::utils;

#ifndef LIFETIME_PLUGIN_PATH
#error "LIFETIME_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

// True while the plugin library is still mapped in this process.
bool library_mapped() {
    void* h = dlopen(LIFETIME_PLUGIN_PATH, RTLD_NOW | RTLD_NOLOAD);
    if (!h) return false;
    dlclose(h);
    return true;
}

// The marker file the plugin appends to, in a directory of its own. The file
// and the directory are removed and the variable unset when the scope ends,
// also when a REQUIRE fails.
class Marker {
   public:
    Marker() {
        char dir[] = "/tmp/dftu_lifetime_XXXXXX";
        if (mkdtemp(dir) == nullptr) return;
        dir_ = dir;
        path_ = dstd::String(dir_) + "/marker";
        setenv("LIFETIME_PLUGIN_MARKER", path_.c_str(), 1);
    }
    Marker(const Marker&) = delete;
    Marker& operator=(const Marker&) = delete;
    ~Marker() {
        unsetenv("LIFETIME_PLUGIN_MARKER");
        if (dir_.empty()) return;
        std::remove(path_.c_str());
        rmdir(dir_.c_str());
    }

    bool ok() const { return !dir_.empty(); }

    dstd::String text() const {
        dstd::String text;
        std::FILE* f = std::fopen(path_.c_str(), "rb");
        if (!f) return text;
        char buf[64];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            text.append(buf, n);
        std::fclose(f);
        return text;
    }

    // collect() returns once its root task has completed, but the plan it was
    // handed is a parameter of that task's frame, which the runtime may free
    // a moment later on another thread, so the last reference to the node and
    // the provider can drop after dftu_lazyframe_free returns. Waits, on the
    // marker itself, until `size` bytes are there.
    dstd::String wait_for(std::size_t size) const {
        dstd::String seen = text();
        for (int i = 0; i < 1000000 && seen.size() < size; ++i) {
            std::this_thread::yield();
            seen = text();
        }
        return seen;
    }

   private:
    dstd::String dir_;
    dstd::String path_;
};

}  // namespace

TEST_CASE("a plan keeps the plugin library loaded after the plugin unloads") {
    Marker marker;
    REQUIRE(marker.ok());

    dstd::Optional<plugins::host::Plugins> loaded;
    {
        auto built =
            plugins::host::Plugins::Builder().add(LIFETIME_PLUGIN_PATH).build();
        REQUIRE(built.has_value());
        loaded = std::move(built).value();
    }
    REQUIRE(library_mapped());

    dftu_lazyframe* source =
        dftu_lazyframe_from_provider("lifetime_plugin.src");
    REQUIRE(source != nullptr);
    dftu_op_arg args{};
    dftu_lazyframe* plan =
        dftu_lazyframe_op(source, "lifetime_plugin.pass", &args);
    REQUIRE(plan != nullptr);
    dftu_lazyframe_free(source);

    loaded.reset();
    // The names go at once; the library and the plugin stay for the plan that
    // holds them.
    CHECK(dftu_lazyframe_from_provider("lifetime_plugin.src") == nullptr);
    CHECK(dftu_node_unregister("lifetime_plugin.pass") != 0);
    CHECK(library_mapped());
    CHECK(marker.text().empty());

    dftu_dataframe* out = dftu_lazyframe_collect(plan, -1);
    REQUIRE(out != nullptr);
    CHECK(dftu_dataframe_num_rows(out) == 3);
    dftu_dataframe_free(out);
    CHECK(library_mapped());
    CHECK(marker.text().empty());

    dftu_lazyframe_free(plan);
    const dstd::String order = marker.wait_for(3);
    // The node and the provider first, in either order, then the plugin.
    REQUIRE(order.size() == 3);
    CHECK(order[2] == 'p');
    CHECK(order.find('s') != dstd::String::npos);
    CHECK(order.find('n') != dstd::String::npos);
    CHECK_FALSE(library_mapped());
}

TEST_CASE("an unloaded plugin with no plan left closes its library at once") {
    Marker marker;
    REQUIRE(marker.ok());
    {
        auto built =
            plugins::host::Plugins::Builder().add(LIFETIME_PLUGIN_PATH).build();
        REQUIRE(built.has_value());
        CHECK(library_mapped());
    }
    CHECK(marker.text() == "snp");
    CHECK_FALSE(library_mapped());
}
