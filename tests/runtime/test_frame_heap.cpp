#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <doctest/doctest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "memory/fresh_process.h"

using namespace dftracer::utils;
using dftracer::utils::coro::CoroTask;
namespace memory = dftracer::utils::memory;

namespace {

CoroTask<void> idle() { co_return; }

// Called through a pointer the compiler cannot see through, so it cannot put
// the frame on the stack.
CoroTask<void> (*volatile MAKE_IDLE)() = idle;

// Writes the faulting address in hex, so the parent can tell a jump through the
// poison pattern from a crash on stale frame bytes.
void report_fault(int, siginfo_t* info, void*) {
    static const char DIGITS[] = "0123456789abcdef";
    char text[32] = "fault 0x";
    std::size_t at = 8;
    auto addr = reinterpret_cast<std::uintptr_t>(info->si_addr);
    for (int shift = 60; shift >= 0; shift -= 4) {
        text[at++] = DIGITS[(addr >> shift) & 0xf];
    }
    text[at++] = '\n';
    ssize_t written = ::write(STDERR_FILENO, text, at);
    (void)written;
    ::_exit(70);
}

}  // namespace

TEST_CASE("frames are charged to the task's pool") {
    memory::PoolHandle pool = memory::Manager::get().root().child("frames");
    REQUIRE(pool);
    {
        memory::Scope scope{*pool};
        CoroTask<void> task = MAKE_IDLE();
        CHECK(pool->blocks() == 1);
        CHECK(pool->live() > 0);
    }
    CHECK(pool->blocks() == 0);
    CHECK(pool->live() == 0);
}

TEST_CASE("a resumed destroyed frame child" * doctest::skip()) {
    fresh_process::announce();
    struct sigaction action = {};
    action.sa_sigaction = report_fault;
    action.sa_flags = SA_SIGINFO;
    ::sigaction(SIGSEGV, &action, nullptr);
    ::sigaction(SIGBUS, &action, nullptr);
    std::coroutine_handle<> handle;
    {
        CoroTask<void> task = MAKE_IDLE();
        handle = task.handle();
    }
    handle.resume();
}

TEST_CASE("a resumed destroyed frame crashes in debug mode") {
    ::setenv("DFTRACER_UTILS_MEMORY_DEBUG", "1", 1);
    auto result = fresh_process::run("a resumed destroyed frame child");
    ::unsetenv("DFTRACER_UTILS_MEMORY_DEBUG");
    REQUIRE(result.announced);
    CHECK(WIFEXITED(result.status));
    CHECK(WEXITSTATUS(result.status) == 70);
    CHECK(result.output.find("fault 0xdddddddd") != std::string::npos);
}
