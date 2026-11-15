// SPDX-License-Identifier: MIT
// A column a plugin builds borrows the plugin's buffer and carries a release
// callback that lives in the plugin library. The host unloads the plugin while
// the frame that holds that column is still alive, so the library must stay
// mapped until the last borrowed column is freed, and the release callback must
// run before the library closes.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dlfcn.h>
#include <doctest/doctest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace dftracer::utils;

#ifndef BORROWED_FRAME_PLUGIN_PATH
#error "BORROWED_FRAME_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

// True while the plugin library is still mapped in this process.
bool library_mapped() {
    void* handle = dlopen(BORROWED_FRAME_PLUGIN_PATH, RTLD_NOW | RTLD_NOLOAD);
    if (!handle) return false;
    dlclose(handle);
    return true;
}

// The marker file the plugin appends to ('r' per release, 'p' at its destroy),
// in a directory of its own, removed when the scope ends.
class Marker {
   public:
    Marker() {
        char dir[] = "/tmp/dftu_borrowed_XXXXXX";
        if (mkdtemp(dir) == nullptr) return;
        dir_ = dir;
        path_ = dstd::String(dir_) + "/marker";
        setenv("BORROWED_FRAME_PLUGIN_MARKER", path_.c_str(), 1);
    }
    Marker(const Marker&) = delete;
    Marker& operator=(const Marker&) = delete;
    ~Marker() {
        unsetenv("BORROWED_FRAME_PLUGIN_MARKER");
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

   private:
    dstd::String dir_;
    dstd::String path_;
};

dftu_dataframe* input_frame() {
    const std::int64_t ids[] = {1, 2};
    const char* names[] = {"id"};
    dftu_series* column =
        dftu_series_new_flat(DFTU_TYPE_INT64, ids, 2, nullptr);
    if (column == nullptr) return nullptr;
    return dftu_dataframe_new(names, &column, 1);
}

dstd::Result<plugins::host::Plugins> load_plugin() {
    return plugins::host::Plugins::Builder()
        .add(BORROWED_FRAME_PLUGIN_PATH)
        .build();
}

}  // namespace

TEST_CASE("a column borrowed through the C ABI outlives the plugin library") {
    Marker marker;
    REQUIRE(marker.ok());

    dstd::Optional<plugins::host::Plugins> loaded;
    {
        auto built = load_plugin();
        REQUIRE(built.has_value());
        loaded = std::move(built).value();
    }

    const dftu_op_desc* op = dftu_op_find("borrowed_frame_plugin.abi");
    REQUIRE(op != nullptr);
    dftu_dataframe* in = input_frame();
    REQUIRE(in != nullptr);
    const dftu_dataframe* operands[] = {in};
    dftu_dataframe* out = dftu_op_run_frame(op, operands, 1, nullptr);
    dftu_dataframe_free(in);
    REQUIRE(out != nullptr);

    loaded.reset();
    // The frame still borrows this library's bytes, so it stays mapped and the
    // release callback has not run.
    CHECK(marker.text().empty());
    CHECK(library_mapped());

    dftu_series* value = dftu_dataframe_column(out, "value");
    REQUIRE(value != nullptr);
    const auto* bytes =
        static_cast<const std::int64_t*>(dftu_series_data(value));
    REQUIRE(bytes != nullptr);
    CHECK(bytes[0] == 7);
    CHECK(bytes[1] == 8);
    CHECK(bytes[2] == 9);
    dftu_series_free(value);

    dftu_dataframe_free(out);
    // Release ran exactly once, then the plugin's destroy, then the close.
    CHECK(marker.text() == "rp");
    CHECK_FALSE(library_mapped());
}

TEST_CASE("a column the SDK builder borrows outlives the plugin library") {
    Marker marker;
    REQUIRE(marker.ok());

    dstd::Optional<plugins::host::Plugins> loaded;
    {
        auto built = load_plugin();
        REQUIRE(built.has_value());
        loaded = std::move(built).value();
    }

    const dftu_op_desc* op = dftu_op_find("borrowed_frame_plugin.sdk");
    REQUIRE(op != nullptr);
    dftu_dataframe* in = input_frame();
    REQUIRE(in != nullptr);
    const dftu_dataframe* operands[] = {in};
    dftu_dataframe* out = dftu_op_run_frame(op, operands, 1, nullptr);
    dftu_dataframe_free(in);
    REQUIRE(out != nullptr);

    loaded.reset();
    CHECK(marker.text().empty());
    CHECK(library_mapped());

    dftu_series* value = dftu_dataframe_column(out, "value");
    REQUIRE(value != nullptr);
    const auto* bytes =
        static_cast<const std::int64_t*>(dftu_series_data(value));
    REQUIRE(bytes != nullptr);
    CHECK(bytes[0] == 4);
    CHECK(bytes[1] == 5);
    CHECK(bytes[2] == 6);
    dftu_series_free(value);

    dftu_dataframe_free(out);
    // The SDK's release callback runs first (it keeps no marker), then the
    // plugin's destroy, then the close.
    CHECK(marker.text() == "p");
    CHECK_FALSE(library_mapped());
}

TEST_CASE(
    "a second load of the same library does not drop the first one's pin") {
    Marker marker;
    REQUIRE(marker.ok());

    dstd::Optional<plugins::host::Plugins> loaded;
    {
        auto built = load_plugin();
        REQUIRE(built.has_value());
        loaded = std::move(built).value();
    }
    {
        // The same file again: it maps at the same base and fails on the names
        // the first set already registered, so its library dies at once.
        auto again = load_plugin();
        CHECK_FALSE(again.has_value());
    }

    const dftu_op_desc* op = dftu_op_find("borrowed_frame_plugin.abi");
    REQUIRE(op != nullptr);
    dftu_dataframe* in = input_frame();
    REQUIRE(in != nullptr);
    const dftu_dataframe* operands[] = {in};
    dftu_dataframe* out = dftu_op_run_frame(op, operands, 1, nullptr);
    dftu_dataframe_free(in);
    REQUIRE(out != nullptr);

    loaded.reset();
    CHECK(library_mapped());

    dftu_dataframe_free(out);
    CHECK_FALSE(library_mapped());
}
