// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_MPI_LAUNCHER_H
#define DFTRACER_UTILS_TESTS_MPI_LAUNCHER_H

// Helpers of the tests that start a tool under an MPI launcher. Only the test
// files that exist with the MPI option ON include it.

#include <testing_utilities.h>

namespace dftu_utils_test {

// -- Shared helpers for the MPI binary integration tests --

// Locate an MPI launcher: $MPIEXEC_EXECUTABLE if set, else mpiexec/mpirun on
// PATH. Returns "" if none is found.
inline dftracer::utils::dstd::String find_mpi_launcher() {
    const char* env_path = std::getenv("MPIEXEC_EXECUTABLE");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;
    for (const auto& name : {"mpiexec", "mpirun"}) {
        dftracer::utils::dstd::String cmd =
            dftracer::utils::dstd::String("command -v ") + name +
            " 2>/dev/null";
        FILE* p = ::popen(cmd.c_str(), "r");
        if (!p) continue;
        char buf[4096];
        dftracer::utils::dstd::String out;
        while (std::fgets(buf, sizeof(buf), p)) out += buf;
        ::pclose(p);
        while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
            out.pop_back();
        if (!out.empty() && ::access(out.c_str(), X_OK) == 0) return out;
    }
    return "";
}

// Launch `binary` under `launcher` with `np` ranks. Returns the exit code.
// OpenMPI refuses to run as root (common in CI containers); permit it via env
// vars rather than --allow-run-as-root, which MPICH does not recognize. MPICH
// ignores unknown env vars, so this is portable across implementations.
inline int run_mpi(
    const dftracer::utils::dstd::String& launcher, int np,
    const dftracer::utils::dstd::String& binary,
    const dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>&
        binary_args) {
    ::setenv("OMPI_ALLOW_RUN_AS_ROOT", "1", 1);
    ::setenv("OMPI_ALLOW_RUN_AS_ROOT_CONFIRM", "1", 1);
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> args = {
        "-n", dftracer::utils::dstd::to_string(np), binary};
    for (const auto& a : binary_args) args.push_back(a);
    return run_process(launcher, args);
}

// Shared setup for the *_mpi binary integration tests: locates the serial and
// MPI binaries plus an MPI launcher, and records a skip reason if any is
// missing. Tests derive a small Env from this with their specific names.
struct MpiTestEnv {
    dftracer::utils::dstd::String serial_bin;
    dftracer::utils::dstd::String mpi_bin;
    dftracer::utils::dstd::String launcher;
    bool ready = false;
    dftracer::utils::dstd::String skip_reason;

    MpiTestEnv(const dftracer::utils::dstd::String& serial_name,
               const char* serial_env,
               const dftracer::utils::dstd::String& mpi_name,
               const char* mpi_env) {
        serial_bin = find_binary_by_name(serial_env, serial_name);
        mpi_bin = find_binary_by_name(mpi_env, mpi_name);
        launcher = find_mpi_launcher();
        if (serial_bin.empty()) {
            skip_reason = serial_name + " binary not found";
            return;
        }
        if (mpi_bin.empty()) {
            skip_reason = mpi_name + " binary not found (set " +
                          dftracer::utils::dstd::String(mpi_env) +
                          " or build with DFTRACER_UTILS_ENABLE_MPI=ON)";
            return;
        }
        if (launcher.empty()) {
            skip_reason = "no mpiexec/mpirun on PATH";
            return;
        }
        ready = true;
    }
};
}  // namespace dftu_utils_test

#endif  // DFTRACER_UTILS_TESTS_MPI_LAUNCHER_H
