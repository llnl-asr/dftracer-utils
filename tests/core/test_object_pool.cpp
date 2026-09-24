#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/object_pool.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

using dftracer::utils::ObjectPool;

TEST_SUITE("ObjectPool") {
    TEST_CASE("blocks keep default new alignment, fresh and reused") {
        auto& pool = ObjectPool::instance();
        for (std::size_t size : {8, 24, 40, 200, 4096, 8192, 100000}) {
            CAPTURE(size);
            std::vector<void*> blocks;
            for (int round = 0; round < 2; ++round) {
                for (int i = 0; i < 4; ++i)
                    blocks.push_back(pool.allocate(size));
                for (void* p : blocks)
                    CHECK(reinterpret_cast<std::uintptr_t>(p) %
                              __STDCPP_DEFAULT_NEW_ALIGNMENT__ ==
                          0);
                for (void* p : blocks) pool.deallocate(p, size);
                blocks.clear();
            }
        }
    }
}
