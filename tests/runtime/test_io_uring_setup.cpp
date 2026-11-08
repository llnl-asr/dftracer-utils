// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#ifdef DFTRACER_UTILS_HAVE_IO_URING

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/runtime/io/io_uring_prep.h>
#include <dftracer/utils/runtime/io/io_uring_register.h>
#include <dftracer/utils/runtime/io/io_uring_wrapper.h>
#include <sched.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace dftracer::utils;
using runtime::io::uring::Ring;
using runtime::io::uring::RingOptions;

namespace {

constexpr unsigned RING_ENTRIES = 8;
constexpr int WAIT_LIMIT_MS = 5000;

bool kernel_at_least(int major, int minor) {
    struct utsname name;
    if (::uname(&name) != 0) return true;
    int have_major = 0;
    int have_minor = 0;
    if (std::sscanf(name.release, "%d.%d", &have_major, &have_minor) != 2) {
        return true;
    }
    return have_major > major || (have_major == major && have_minor >= minor);
}

// Opens `ring` with `options`. A refusal skips only on a kernel older than
// the one that has the flag; on a newer one it fails, and so does a ring that
// opens without the flags.
bool open_ring(Ring& ring, const RingOptions& options, int major, int minor,
               const char* what) {
    Ring base;
    if (!base.init(RING_ENTRIES)) {
        MESSAGE("skipped: no io_uring");
        return false;
    }
    if (ring.init(RING_ENTRIES, options)) return true;
    if (kernel_at_least(major, minor)) {
        FAIL_CHECK("a kernel from " << major << "." << minor << " refused "
                                    << dstd::String(what));
        return false;
    }
    MESSAGE("skipped: a kernel before " << major << "." << minor << " lacks "
                                        << dstd::String(what));
    return false;
}

// Runs more NOPs than the ring holds, so every slot and the wrap are used.
// Each completion must carry the user_data of its own entry.
void check_runs_nops(Ring& ring, unsigned rounds = 4 * RING_ENTRIES) {
    for (unsigned i = 0; i < rounds; ++i) {
        struct io_uring_sqe* sqe = ring.get_sqe();
        REQUIRE(sqe != nullptr);
        runtime::io::uring::prep_nop(sqe);
        sqe->user_data = 1000 + i;
        REQUIRE(ring.enter(false) >= 0);
        struct io_uring_cqe* cqe = nullptr;
        for (int waited = 0; waited < WAIT_LIMIT_MS; ++waited) {
            ring.flush_completions();
            cqe = ring.peek_cqe();
            if (cqe != nullptr) break;
            ::usleep(1000);
        }
        REQUIRE(cqe != nullptr);
        CHECK(cqe->user_data == 1000 + i);
        CHECK(cqe->res == 0);
        ring.cqe_seen();
    }
}

// Older kernels refuse a NOP on a polling ring with -EINVAL.
bool polling_ring_takes_nop(Ring& ring) {
    struct io_uring_sqe* sqe = ring.get_sqe();
    REQUIRE(sqe != nullptr);
    runtime::io::uring::prep_nop(sqe);
    REQUIRE(ring.enter(false) >= 0);
    struct io_uring_cqe* cqe = nullptr;
    for (int waited = 0; waited < WAIT_LIMIT_MS && cqe == nullptr; ++waited) {
        ring.flush_completions();
        cqe = ring.peek_cqe();
        if (cqe == nullptr) ::usleep(1000);
    }
    REQUIRE(cqe != nullptr);
    const int res = cqe->res;
    ring.cqe_seen();
    return res == 0;
}

}  // namespace

TEST_CASE("ring setup: a larger CQ than the default") {
    Ring ring;
    const RingOptions options{.flags = IORING_SETUP_CQSIZE, .cq_entries = 64};
    if (!open_ring(ring, options, 5, 5, "CQSIZE")) return;
    CHECK(ring.cq_entries() == 64);
    check_runs_nops(ring);
}

TEST_CASE("ring setup: CLAMP accepts a size past the kernel's limit") {
    Ring plain;
    if (!plain.init(RING_ENTRIES)) {
        MESSAGE("skipped: no io_uring");
        return;
    }
    Ring refused;
    CHECK_FALSE(refused.init(1U << 30));
    Ring clamped;
    REQUIRE(clamped.init(1U << 30, RingOptions{.flags = IORING_SETUP_CLAMP}));
    CHECK(clamped.cq_entries() > 0);
    check_runs_nops(clamped);
}

TEST_CASE("ring setup: the other flags that change no layout") {
    struct Flag {
        unsigned flags;
        int major;
        int minor;
        const char* name;
    };
    for (const Flag& flag :
         {Flag{IORING_SETUP_SUBMIT_ALL, 5, 18, "SUBMIT_ALL"},
          Flag{IORING_SETUP_COOP_TASKRUN, 5, 19, "COOP_TASKRUN"}}) {
        Ring ring;
        if (!open_ring(ring, RingOptions{.flags = flag.flags}, flag.major,
                       flag.minor, flag.name)) {
            continue;
        }
        check_runs_nops(ring);
    }
}

TEST_CASE("ring setup: SUBMIT_ALL goes on past a bad entry") {
    Ring ring;
    if (!open_ring(ring, RingOptions{.flags = IORING_SETUP_SUBMIT_ALL}, 5, 18,
                   "SUBMIT_ALL")) {
        return;
    }
    struct io_uring_sqe* bad = ring.get_sqe();
    REQUIRE(bad != nullptr);
    runtime::io::uring::prep_nop(bad);
    bad->opcode = 255;
    bad->user_data = 1;
    struct io_uring_sqe* good = ring.get_sqe();
    REQUIRE(good != nullptr);
    runtime::io::uring::prep_nop(good);
    good->user_data = 2;
    CHECK(ring.enter(false) == 2);
    unsigned seen = 0;
    for (int waited = 0; waited < WAIT_LIMIT_MS && seen < 2; ++waited) {
        if (struct io_uring_cqe* cqe = ring.peek_cqe()) {
            CHECK(cqe->res == (cqe->user_data == 1 ? -EINVAL : 0));
            ring.cqe_seen();
            ++seen;
        } else {
            ::usleep(1000);
        }
    }
    CHECK(seen == 2);
}

TEST_CASE("ring setup: big entries use every slot of their array") {
    for (const unsigned flags :
         {unsigned{IORING_SETUP_SQE128}, unsigned{IORING_SETUP_CQE32},
          unsigned{IORING_SETUP_SQE128 | IORING_SETUP_CQE32}}) {
        Ring ring;
        if (!open_ring(ring, RingOptions{.flags = flags}, 5, 19,
                       "SQE128 and CQE32")) {
            continue;
        }
        check_runs_nops(ring);
    }
}

TEST_CASE("ring setup: no SQ index array") {
    Ring ring;
    if (!open_ring(ring, RingOptions{.flags = IORING_SETUP_NO_SQARRAY}, 6, 6,
                   "NO_SQARRAY")) {
        return;
    }
    check_runs_nops(ring);
}

TEST_CASE("ring setup: rings in our own memory") {
    Ring ring;
    if (!open_ring(ring, RingOptions{.flags = IORING_SETUP_NO_MMAP}, 6, 5,
                   "NO_MMAP")) {
        return;
    }
    check_runs_nops(ring);
}

TEST_CASE("ring setup: a ring known only by its registered index") {
    Ring ring;
    if (!open_ring(ring,
                   RingOptions{.flags = IORING_SETUP_NO_MMAP |
                                        IORING_SETUP_REGISTERED_FD_ONLY},
                   6, 5, "REGISTERED_FD_ONLY")) {
        return;
    }
    CHECK(ring.is_fd_registered());
    check_runs_nops(ring);
    runtime::io::uring::Probe probe;
    CHECK(ring.probe(probe));
}

TEST_CASE("ring setup: a second ring shares the kernel workers of the first") {
    Ring first;
    if (!first.init(RING_ENTRIES)) {
        MESSAGE("skipped: no io_uring");
        return;
    }
    Ring second;
    REQUIRE(second.init(
        RING_ENTRIES,
        RingOptions{.flags = IORING_SETUP_ATTACH_WQ, .attach_fd = first.fd()}));
    check_runs_nops(second);
    Ring refused;
    CHECK_FALSE(
        refused.init(RING_ENTRIES, RingOptions{.flags = IORING_SETUP_ATTACH_WQ,
                                               .attach_fd = STDIN_FILENO}));
}

TEST_CASE("ring setup: a polled ring on a chosen CPU") {
    cpu_set_t allowed;
    REQUIRE(::sched_getaffinity(0, sizeof(allowed), &allowed) == 0);
    unsigned cpu = 0;
    while (cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &allowed)) ++cpu;
    REQUIRE(cpu < CPU_SETSIZE);
    Ring ring;
    if (!open_ring(
            ring,
            RingOptions{.flags = IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF,
                        .sq_idle_ms = 100,
                        .sq_cpu = cpu},
            5, 11, "SQPOLL for an ordinary user")) {
        return;
    }
    REQUIRE((ring.features() & IORING_FEAT_SQPOLL_NONFIXED) != 0);
    check_runs_nops(ring);
}

TEST_CASE("ring setup: a polling ring finds its completions in an enter") {
    Ring ring;
    if (!open_ring(ring, RingOptions{.flags = IORING_SETUP_IOPOLL}, 5, 1,
                   "IOPOLL")) {
        return;
    }
    if (!polling_ring_takes_nop(ring)) {
        MESSAGE("skipped: this kernel has no NOP on a polling ring");
        return;
    }
    check_runs_nops(ring);
    Ring hybrid;
    if (!open_ring(hybrid,
                   RingOptions{.flags = IORING_SETUP_IOPOLL |
                                        IORING_SETUP_HYBRID_IOPOLL},
                   6, 13, "HYBRID_IOPOLL")) {
        return;
    }
    check_runs_nops(hybrid);
}

TEST_CASE("ring setup: SQ_REWIND reads from slot 0 at each enter") {
    Ring ring;
    if (!open_ring(ring,
                   RingOptions{.flags = IORING_SETUP_NO_SQARRAY |
                                        IORING_SETUP_SQ_REWIND},
                   7, 0, "SQ_REWIND")) {
        return;
    }
    check_runs_nops(ring);
}

TEST_CASE("ring setup: SQ_REWIND batches go through enter_wait") {
    Ring ring;
    if (!open_ring(ring,
                   RingOptions{.flags = IORING_SETUP_NO_SQARRAY |
                                        IORING_SETUP_SQ_REWIND},
                   7, 0, "SQ_REWIND")) {
        return;
    }
    constexpr unsigned BATCH = 4;
    for (unsigned round = 0; round < 6; ++round) {
        for (unsigned i = 0; i < BATCH; ++i) {
            struct io_uring_sqe* sqe = ring.get_sqe();
            REQUIRE(sqe != nullptr);
            runtime::io::uring::prep_nop(sqe);
            sqe->user_data = round * 100 + i;
        }
        CHECK(ring.enter_wait(WAIT_LIMIT_MS) == static_cast<int>(BATCH));
        unsigned seen = 0;
        for (int waited = 0; waited < WAIT_LIMIT_MS && seen < BATCH; ++waited) {
            ring.flush_completions();
            if (struct io_uring_cqe* cqe = ring.peek_cqe()) {
                CHECK(cqe->user_data / 100 == round);
                CHECK(cqe->res == 0);
                ring.cqe_seen();
                ++seen;
            } else {
                ::usleep(1000);
            }
        }
        CHECK(seen == BATCH);
    }
}

TEST_CASE("ring setup: mixed entries post and pad as the kernel documents") {
    Ring ring;
    if (!open_ring(ring,
                   RingOptions{.flags = IORING_SETUP_CQE_MIXED |
                                        IORING_SETUP_SQE_MIXED},
                   6, 19, "mixed entries")) {
        return;
    }
    check_runs_nops(ring);
}

TEST_CASE("ring setup: a 128 byte NOP wraps the SQ and posts a 32 byte CQE") {
    Ring ring;
    if (!open_ring(ring,
                   RingOptions{.flags = IORING_SETUP_CQE_MIXED |
                                        IORING_SETUP_SQE_MIXED},
                   6, 19, "mixed entries")) {
        return;
    }
    // Three slots per round on eight, so a big entry meets the end of the SQ
    // and the padding NOP that posts nothing.
    unsigned wide = 0;
    for (unsigned round = 0; round < 12; ++round) {
        struct io_uring_sqe* small = ring.get_sqe();
        REQUIRE(small != nullptr);
        runtime::io::uring::prep_nop(small);
        small->user_data = round * 10 + 1;
        struct io_uring_sqe* big = ring.get_sqe128();
        REQUIRE(big != nullptr);
        std::memset(big, 0, 2 * sizeof(*big));
        runtime::io::uring::prep_nop128(big);
        runtime::io::uring::set_op_flags(big, IORING_NOP_CQE32);
        big->user_data = round * 10 + 2;
        REQUIRE(ring.enter(false) >= 0);

        unsigned seen = 0;
        for (int waited = 0; waited < WAIT_LIMIT_MS && seen < 2; ++waited) {
            ring.flush_completions();
            struct io_uring_cqe* cqe = ring.peek_cqe();
            if (cqe == nullptr) {
                ::usleep(1000);
                continue;
            }
            CHECK((cqe->flags & IORING_CQE_F_SKIP) == 0);
            CHECK(cqe->res == 0);
            if (cqe->user_data == round * 10 + 2) {
                CHECK((cqe->flags & IORING_CQE_F_32) != 0);
                ++wide;
            } else {
                CHECK(cqe->user_data == round * 10 + 1);
                CHECK((cqe->flags & IORING_CQE_F_32) == 0);
            }
            ring.cqe_seen();
            ++seen;
        }
        CHECK(seen == 2);
    }
    CHECK(wide == 12);
}

TEST_CASE("ring setup: enter through a registered index") {
    Ring ring;
    if (!ring.init(RING_ENTRIES)) {
        MESSAGE("skipped: no io_uring");
        return;
    }
    unsigned index = 0;
    const int registered = runtime::io::uring::register_ring_fd(ring, index);
    if (registered == -EINVAL && !kernel_at_least(5, 18)) {
        MESSAGE("skipped: a kernel before 5.18 lacks registered ring fds");
        return;
    }
    REQUIRE(registered == 0);
    ring.enter_through_index(index);
    check_runs_nops(ring);

    struct io_uring_sqe* sqe = ring.get_sqe();
    REQUIRE(sqe != nullptr);
    runtime::io::uring::prep_nop(sqe);
    sqe->user_data = 7;
    CHECK(ring.enter_wait(WAIT_LIMIT_MS) == 1);
    struct io_uring_cqe* cqe = ring.peek_cqe();
    REQUIRE(cqe != nullptr);
    CHECK(cqe->user_data == 7);
    ring.cqe_seen();
}

#else

TEST_CASE("ring setup: this platform has no io_uring") {}

#endif
