// SPDX-License-Identifier: MIT
// One thread, one io_uring ring and cached reads: what each ring feature costs
// or gains on its own, without the runtime in between. Rounds alternate the
// setups and each prints the median of its rounds.
#ifdef DFTRACER_UTILS_HAVE_IO_URING

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/io/io_uring_prep.h>
#include <dftracer/utils/runtime/io/io_uring_register.h>
#include <dftracer/utils/runtime/io/io_uring_wrapper.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace dftracer::utils;

namespace {

constexpr std::uint64_t FILE_BYTES = std::uint64_t{64} << 20;
constexpr unsigned READ_BYTES = 4096;
constexpr unsigned RING_ENTRIES = 256;
constexpr std::uint64_t READS_PER_ROUND = 200000;

struct Setup {
    const char* name;
    unsigned flags;
    bool has_fixed_file;
    bool has_fixed_buffer;
    bool has_ring_index;
};

const unsigned OWNER_FLAGS =
    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

const Setup SETUPS[] = {
    {"plain", 0, false, false, false},
    {"coop", IORING_SETUP_COOP_TASKRUN, false, false, false},
    {"coop_flag", IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG, false,
     false, false},
    {"single", IORING_SETUP_SINGLE_ISSUER, false, false, false},
    {"owner", OWNER_FLAGS, false, false, false},
    {"owner_nosqa", OWNER_FLAGS | IORING_SETUP_NO_SQARRAY, false, false, false},
    {"owner_ffile", OWNER_FLAGS, true, false, false},
    {"owner_fbuf", OWNER_FLAGS, false, true, false},
    {"owner_ring", OWNER_FLAGS, false, false, true},
    {"owner_all", OWNER_FLAGS | IORING_SETUP_NO_SQARRAY, true, true, true},
};

struct Prepared {
    runtime::io::uring::Ring ring;
    int file_arg = -1;
    bool is_usable = false;
};

bool prepare(Prepared& out, const Setup& setup, int fd, dstd::Vector<char>& buf,
             unsigned batch) {
    runtime::io::uring::RingOptions options;
    options.flags = setup.flags;
    if (!out.ring.init(RING_ENTRIES, options)) return false;
    if ((setup.flags & IORING_SETUP_R_DISABLED) != 0) out.ring.enable();
    out.file_arg = fd;
    if (setup.has_fixed_file) {
        if (runtime::io::uring::register_files(out.ring, &fd, 1) < 0)
            return false;
        out.file_arg = 0;
    }
    if (setup.has_fixed_buffer) {
        struct iovec whole{buf.data(), READ_BYTES * batch};
        if (runtime::io::uring::register_buffers(out.ring, &whole, 1) < 0)
            return false;
    }
    if (setup.has_ring_index) {
        unsigned index = 0;
        if (runtime::io::uring::register_ring_fd(out.ring, index) < 0)
            return false;
        out.ring.enter_through_index(index);
    }
    return true;
}

// Nanoseconds per read, or a negative value when a read failed.
double run_round(Prepared& prepared, const Setup& setup,
                 dstd::Vector<char>& buf, unsigned batch,
                 std::uint64_t& position) {
    runtime::io::uring::Ring& ring = prepared.ring;
    const auto begin = std::chrono::steady_clock::now();
    std::uint64_t done = 0;
    while (done < READS_PER_ROUND) {
        for (unsigned i = 0; i < batch; ++i) {
            struct io_uring_sqe* sqe = ring.get_sqe();
            if (sqe == nullptr) return -1;
            char* const target = buf.data() + std::size_t{i} * READ_BYTES;
            position = (position + 7 * READ_BYTES) % (FILE_BYTES - READ_BYTES);
            if (setup.has_fixed_buffer) {
                runtime::io::uring::prep_read_fixed(
                    sqe, prepared.file_arg, target, READ_BYTES, position, 0);
            } else {
                runtime::io::uring::prep_read(sqe, prepared.file_arg, target,
                                              READ_BYTES,
                                              static_cast<off_t>(position));
            }
            if (setup.has_fixed_file) sqe->flags |= IOSQE_FIXED_FILE;
            sqe->user_data = i;
        }
        if (ring.enter(true) < 0) return -1;
        unsigned reaped = 0;
        while (reaped < batch) {
            struct io_uring_cqe* cqe = ring.peek_cqe();
            if (cqe == nullptr) {
                if (ring.enter(true) < 0) return -1;
                continue;
            }
            if (cqe->res != static_cast<int>(READ_BYTES)) return -1;
            ring.cqe_seen();
            ++reaped;
        }
        done += batch;
    }
    const std::chrono::duration<double, std::nano> took =
        std::chrono::steady_clock::now() - begin;
    return took.count() / static_cast<double>(done);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ring_bench DIR [ROUNDS]\n");
        return 2;
    }
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 7;
    dstd::String path(argv[1]);
    path += "/ring_bench.dat";
    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) return 1;
    {
        dstd::Vector<char> block(std::size_t{1} << 20, 'x');
        for (std::uint64_t at = 0; at < FILE_BYTES; at += block.size()) {
            if (::write(fd, block.data(), block.size()) !=
                static_cast<ssize_t>(block.size())) {
                return 1;
            }
        }
    }
    constexpr std::size_t SETUP_COUNT = sizeof(SETUPS) / sizeof(SETUPS[0]);
    for (const unsigned batch : {1U, 8U, 32U}) {
        dstd::Vector<char> buf(std::size_t{batch} * READ_BYTES);
        dstd::Vector<dstd::Vector<double>> times(SETUP_COUNT);
        dstd::Vector<bool> is_refused(SETUP_COUNT, false);
        std::uint64_t position = 0;
        for (int round = 0; round < rounds; ++round) {
            for (std::size_t s = 0; s < SETUP_COUNT; ++s) {
                if (is_refused[s]) continue;
                Prepared prepared;
                if (!prepare(prepared, SETUPS[s], fd, buf, batch)) {
                    is_refused[s] = true;
                    continue;
                }
                const double ns =
                    run_round(prepared, SETUPS[s], buf, batch, position);
                if (ns < 0) {
                    is_refused[s] = true;
                    continue;
                }
                times[s].push_back(ns);
            }
        }
        double base = 0;
        for (std::size_t s = 0; s < SETUP_COUNT; ++s) {
            if (is_refused[s] || times[s].empty()) {
                std::printf("batch=%-3u %-12s refused\n", batch,
                            SETUPS[s].name);
                continue;
            }
            std::sort(times[s].begin(), times[s].end());
            const double median = times[s][times[s].size() / 2];
            if (std::strcmp(SETUPS[s].name, "owner") == 0) base = median;
            std::printf("batch=%-3u %-12s ns_per_read=%8.1f min=%8.1f\n", batch,
                        SETUPS[s].name, median, times[s].front());
        }
        std::printf("batch=%-3u owner median is %.1f\n", batch, base);
    }
    ::close(fd);
    ::unlink(path.c_str());
    return 0;
}

#else

#include <cstdio>

int main() {
    std::printf("ring_bench needs io_uring\n");
    return 0;
}

#endif
