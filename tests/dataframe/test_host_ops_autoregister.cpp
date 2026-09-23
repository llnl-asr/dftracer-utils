// The host ops must be in the registry from linking the utilities library
// alone. Nothing here calls register_host_ops(), so a static build that lets
// the linker drop the initializer's object file fails this and only this test.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/abi.h>
#include <doctest/doctest.h>

TEST_SUITE("host_ops") {
    TEST_CASE("linking the utilities library registers the host ops") {
        const char* const names[] = {
            "dftu.fs.scan_dir", "dftu.fs.scan_dir_pattern",
            "dftu.file.compress", "dftu.file.decompress",
            "dftu.text.line_filter"};
        for (const char* name : names) {
            REQUIRE_MESSAGE(dftu_op_find(name) != nullptr, name);
        }
    }
}
