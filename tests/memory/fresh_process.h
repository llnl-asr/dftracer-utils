// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_MEMORY_FRESH_PROCESS_H
#define DFTRACER_UTILS_TESTS_MEMORY_FRESH_PROCESS_H

#include <dftracer/utils/dstd/clock.h>
#include <no_core_dump.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fresh_process {

inline std::string self_path() {
    char buffer[4096];
#if defined(__APPLE__)
    auto size = static_cast<std::uint32_t>(sizeof(buffer));
    if (_NSGetExecutablePath(buffer, &size) != 0) return {};
    return buffer;
#else
    ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n <= 0) return {};
    buffer[n] = '\0';
    return buffer;
#endif
}

struct Result {
    int status = 0;
    bool announced = false;
    /// What the process wrote to standard error. run() also copies it to this
    /// process's standard error.
    std::string output;

    bool ok() const {
        int copy = status;
        return announced && WIFEXITED(copy) && WEXITSTATUS(copy) == 0;
    }

    /// The process ran the case, ended by SIGABRT and wrote @p text.
    bool aborted_with(std::string_view text) const {
        int copy = status;
        return announced && WIFSIGNALED(copy) && WTERMSIG(copy) == SIGABRT &&
               output.find(text) != std::string::npos;
    }
};

/// Called first by a test case that run() starts. doctest exits with 0 when no
/// test case matches the name, and this tells run() that the case really ran.
inline void announce() {
    // Raw read: the memory tests link no common, and this runs before the
    // exact counters of a fresh process, where an allocation would show.
    const char* fd = std::getenv("DFTRACER_UTILS_FRESH_PROCESS_FD");
    if (fd == nullptr) return;
    char byte = 'x';
    ssize_t written = ::write(std::atoi(fd), &byte, 1);
    (void)written;
}

/// Like announce(), for a case that ends in abort(): restores the default
/// SIGABRT action, or doctest reports the abort as a crash.
inline void announce_abort() {
    announce();
    ::signal(SIGABRT, SIG_DFL);
}

/// Runs one test case of this binary that is skipped by default in a new
/// process, so it starts with no heap, cache or thread state from the tests
/// that ran before it.
inline Result run(std::string_view test_case, int timeout_ms = 0) {
    std::string self = self_path();
    std::string filter = "--test-case=";
    filter.append(test_case);
    int fds[2];
    int err[2];
    Result result;
    if (::pipe(fds) != 0) return result;
    if (::pipe(err) != 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return result;
    }
    pid_t pid = ::fork();
    if (pid == 0) {
        no_core_dump::disable();
        ::close(fds[0]);
        ::close(err[0]);
        ::dup2(err[1], STDERR_FILENO);
        ::close(err[1]);
        ::setenv("DFTRACER_UTILS_FRESH_PROCESS_FD",
                 std::to_string(fds[1]).c_str(), 1);
        ::execl(self.c_str(), self.c_str(), filter.c_str(), "--no-skip",
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(fds[1]);
    ::close(err[1]);
    // A timeout bounds the read as well as the wait, so a hung child that
    // keeps its stderr open fails the test instead of hanging it.
    const auto deadline = dftracer::utils::dstd::steady_now() +
                          std::chrono::milliseconds(timeout_ms);
    bool timed_out = false;
    char chunk[512];
    for (;;) {
        int wait_ms = -1;
        if (timeout_ms > 0) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - dftracer::utils::dstd::steady_now());
            if (left.count() <= 0) {
                timed_out = true;
                break;
            }
            wait_ms = static_cast<int>(left.count());
        }
        pollfd pfd{err[0], POLLIN, 0};
        const int ready = ::poll(&pfd, 1, wait_ms);
        if (ready < 0 && errno == EINTR) continue;
        if (ready == 0) {
            timed_out = true;
            break;
        }
        const ssize_t got = ::read(err[0], chunk, sizeof(chunk));
        if (got <= 0) break;
        result.output.append(chunk, static_cast<std::size_t>(got));
    }
    ::close(err[0]);
    std::fputs(result.output.c_str(), stderr);
    while (!timed_out && timeout_ms > 0 &&
           ::waitpid(pid, &result.status, WNOHANG) == 0) {
        if (dftracer::utils::dstd::steady_now() >= deadline) {
            timed_out = true;
            break;
        }
        ::usleep(10000);
    }
    if (timed_out || timeout_ms <= 0) {
        if (timed_out) ::kill(pid, SIGKILL);
        ::waitpid(pid, &result.status, 0);
    }
    char byte = 0;
    result.announced = ::read(fds[0], &byte, 1) == 1;
    ::close(fds[0]);
    return result;
}

}  // namespace fresh_process

#endif  // DFTRACER_UTILS_TESTS_MEMORY_FRESH_PROCESS_H
