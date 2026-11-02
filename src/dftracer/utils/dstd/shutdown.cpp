// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/no_destructor.h>
#include <dftracer/utils/dstd/shutdown.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace dftracer::utils::dstd {

namespace {

constexpr std::size_t STAGE_COUNT = 3;
constexpr std::size_t HOOK_SLOTS = 24;

struct Table {
    std::mutex mutex;
    std::size_t count[STAGE_COUNT]{};
    void (*hooks[STAGE_COUNT][HOOK_SLOTS])(){};
    bool armed = false;
    bool ran = false;
};

Table& table() {
    static NoDestructor<Table> value;
    return *value;
}

[[noreturn]] void abort_with(const char* text) {
    std::fputs(text, stderr);
    std::abort();
}

}  // namespace

void add_shutdown_hook(ShutdownStage stage, void (*hook)()) {
    Table& t = table();
    std::lock_guard<std::mutex> lock(t.mutex);
    if (!t.armed) {
        if (std::atexit(&shutdown) != 0) {
            abort_with("dftracer-utils shutdown: cannot register at exit\n");
        }
        t.armed = true;
    }
    const std::size_t index = static_cast<std::size_t>(stage);
    const std::size_t slot = t.count[index]++;
    if (slot >= HOOK_SLOTS) {
        abort_with("dftracer-utils shutdown: too many shutdown hooks\n");
    }
    t.hooks[index][slot] = hook;
}

void shutdown() {
    Table& t = table();
    void (*hooks[STAGE_COUNT][HOOK_SLOTS])(){};
    std::size_t count[STAGE_COUNT]{};
    {
        std::lock_guard<std::mutex> lock(t.mutex);
        if (t.ran) return;
        t.ran = true;
        for (std::size_t index = 0; index < STAGE_COUNT; ++index) {
            count[index] = std::min(t.count[index], HOOK_SLOTS);
            std::copy_n(t.hooks[index], count[index], hooks[index]);
        }
    }
    // Hooks run unlocked, so a hook that waits on a thread which registers
    // a hook cannot deadlock.
    for (std::size_t index = 0; index < STAGE_COUNT; ++index) {
        for (std::size_t slot = count[index]; slot > 0; --slot) {
            if (void (*hook)() = hooks[index][slot - 1]) hook();
        }
    }
}

bool shutdown_started() noexcept {
    Table& t = table();
    std::lock_guard<std::mutex> lock(t.mutex);
    return t.ran;
}

}  // namespace dftracer::utils::dstd
