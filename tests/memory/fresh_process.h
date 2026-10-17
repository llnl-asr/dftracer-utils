#ifndef DFTRACER_UTILS_TESTS_MEMORY_FRESH_PROCESS_H
#define DFTRACER_UTILS_TESTS_MEMORY_FRESH_PROCESS_H

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

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
    /// What the process wrote to standard error; run() also copies it to this
    /// process's standard error.
    std::string output;

    bool ok() const {
        int copy = status;
        return announced && WIFEXITED(copy) && WEXITSTATUS(copy) == 0;
    }
};

/// Called first by a test case that run() starts. doctest exits with 0 when no
/// test case matches the name, and this tells run() that the case really ran.
inline void announce() {
    const char* fd = std::getenv("DFTRACER_FRESH_PROCESS_FD");
    if (fd == nullptr) return;
    char byte = 'x';
    ssize_t written = ::write(std::atoi(fd), &byte, 1);
    (void)written;
}

/// Runs one test case of this binary that is skipped by default in a new
/// process, so it starts with no heap, cache or thread state from the tests
/// that ran before it.
inline Result run(const char* test_case) {
    std::string self = self_path();
    std::string filter = std::string("--test-case=") + test_case;
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
        ::close(fds[0]);
        ::close(err[0]);
        ::dup2(err[1], STDERR_FILENO);
        ::close(err[1]);
        ::setenv("DFTRACER_FRESH_PROCESS_FD", std::to_string(fds[1]).c_str(),
                 1);
        ::execl(self.c_str(), self.c_str(), filter.c_str(), "--no-skip",
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(fds[1]);
    ::close(err[1]);
    char chunk[512];
    ssize_t got;
    while ((got = ::read(err[0], chunk, sizeof(chunk))) > 0) {
        result.output.append(chunk, static_cast<std::size_t>(got));
    }
    ::close(err[0]);
    std::fputs(result.output.c_str(), stderr);
    ::waitpid(pid, &result.status, 0);
    char byte = 0;
    result.announced = ::read(fds[0], &byte, 1) == 1;
    ::close(fds[0]);
    return result;
}

}  // namespace fresh_process

#endif  // DFTRACER_UTILS_TESTS_MEMORY_FRESH_PROCESS_H
