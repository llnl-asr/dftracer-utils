// SPDX-License-Identifier: MIT
// Ends a process that holds a started MPI communicator, in the ways a program
// ends: `leak` returns from main while the communicator is alive, `exit` calls
// std::exit with it alive, and `scope` ends it first. Run under a launcher: the
// exit hook must stop the threads of the backend before MPI ends, so the job
// ends with status 0.
#include <dftracer/utils/comm/group.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace dftracer::utils;

int main(int argc, char** argv) {
    auto started = comm::Communicator::start(comm::BackendKind::Mpi);
    if (!started) {
        std::fprintf(stderr, "start failed: %s\n", started.error().c_str());
        return 3;
    }
    const char* mode = argc > 1 ? argv[1] : "scope";
    if (std::strcmp(mode, "leak") == 0) {
        // Kept alive on purpose: the process ends with the communicator.
        comm::Communicator* kept = new comm::Communicator(std::move(*started));
        return kept == nullptr ? 4 : 0;
    }
    if (std::strcmp(mode, "exit") == 0) {
        // std::exit does not destroy the locals, so the communicator is alive.
        std::exit(0);
    }
    return 0;
}
