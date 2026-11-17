// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/memory/detail/heap.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/memory/pool.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>
#include <sys/wait.h>

#include <cstddef>
#include <new>

using namespace dftracer::utils;

namespace {

constexpr const char* CHILD_CASE =
    "exit hook order: a shutdown hook frees a pool block at exit";

constexpr std::size_t HELD_BYTES = 4096;

struct Held {
    memory::PoolHandle pool;
    void* block = nullptr;
};

Held& held() {
    static Held value;
    return value;
}

void hold() {
    Held& h = held();
    h.pool = memory::detail::Manager::get().root().child(
        "held", memory::PoolKind::Results);
    REQUIRE(h.pool);
    h.block = memory::detail::allocate_at(HELD_BYTES, alignof(std::max_align_t),
                                          *h.pool, __builtin_return_address(0));
}

void release() noexcept {
    Held& h = held();
    if (h.block == nullptr) return;
    memory::detail::deallocate(h.block);
    h.block = nullptr;
}

}  // namespace

TEST_CASE("exit hook order: a shutdown hook frees a pool block at exit" *
          doctest::skip()) {
    fresh_process::announce();
    // Arming shutdown first puts its registration before the exit check's, so
    // only a check that runs the hooks itself reaches a zero count here.
    dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, &release);
    hold();
    memory::detail::set_exit_check(true);
}

TEST_CASE("exit hook order: the exit check runs the shutdown hooks first") {
    auto result = fresh_process::run(CHILD_CASE);
    REQUIRE(result.announced);
    CHECK(result.output.find("exit check failed") == dstd::StringView::npos);
    CHECK(WIFEXITED(result.status));
    CHECK(WEXITSTATUS(result.status) == 0);
}
