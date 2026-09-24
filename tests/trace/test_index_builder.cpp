#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <fstream>
#include <memory>

using namespace dftracer::utils;
using namespace dftracer::utils::index::build;
using namespace dftracer::utils::trace::internal;
using namespace dftu_utils_test;

// `success` == indexed; the batch builder has no skip (a caller concern).
struct BuildResult {
    std::string file_path;
    std::string index_path;
    bool success = false;
    bool index_created = false;
};

static BuildResult run_builder(const std::string& gz,
                               const std::string& index_dir = "",
                               bool force = false,
                               std::size_t checkpoint_size = 0) {
    Runtime rt(4);
    BuildResult out;
    out.file_path = gz;
    auto* out_ptr = &out;

    auto task = run_coro_scope(
        rt.executor(),
        [gz, index_dir, force, checkpoint_size,
         out_ptr](CoroScope& scope) -> coro::CoroTask<void> {
            auto cfg = std::make_shared<IndexBuildBatchConfig>();
            cfg->file_paths = {gz};
            cfg->index_dir = index_dir;
            cfg->force_rebuild = force;
            if (checkpoint_size > 0) cfg->checkpoint_size = checkpoint_size;
            try {
                auto r = co_await BatchBuilder::process(&scope, std::move(cfg));
                out_ptr->success = r.indexed >= 1 && r.failed == 0;
                if (!r.results.empty()) {
                    out_ptr->index_path = r.results[0].index_path;
                    out_ptr->index_created = r.results[0].index_created;
                }
            } catch (const std::exception&) {
                out_ptr->success = false;
            }
        });

    rt.submit(std::move(task), "index-build").wait();
    rt.shutdown();
    return out;
}

TEST_SUITE("IndexBuilder") {
    TEST_CASE("IndexBuilder - Build index for compressed trace file") {
        TestEnvironment env(100);

        SUBCASE("Build index for gzip file") {
            std::string gz_file = env.create_dft_test_gzip_file(50);
            std::string db_root = determine_index_path(gz_file, "");

            auto output = run_builder(gz_file, /*index_dir=*/"",
                                      /*force=*/false, /*checkpoint_size=*/10);

            CHECK(output.file_path == gz_file);
            CHECK(output.index_path == db_root);
            CHECK(output.success == true);

            CHECK(fs::exists(db_root));
        }

        SUBCASE("Repeat build re-indexes") {
            std::string gz_file = env.create_dft_test_gzip_file(20);

            auto output1 = run_builder(gz_file);
            CHECK(output1.success == true);

            auto output2 = run_builder(gz_file);
            CHECK(output2.success == true);
        }
    }

    TEST_CASE("IndexBuilder - Force rebuild") {
        TestEnvironment env(100);

        std::string gz_file = env.create_dft_test_gzip_file(30);

        auto output1 = run_builder(gz_file, /*index_dir=*/"", /*force=*/true);
        CHECK(output1.success == true);

        auto output2 = run_builder(gz_file, /*index_dir=*/"", /*force=*/true);
        CHECK(output2.success == true);
    }

    TEST_CASE("IndexBuilder - Non-existent file") {
        auto output = run_builder("/non/existent/file.gz");

        CHECK(output.success == false);
        CHECK(output.index_created == false);
    }
}
