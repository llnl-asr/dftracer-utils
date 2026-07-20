#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/server/cancel.h>
#include <doctest/doctest.h>
#include <sys/socket.h>
#include <unistd.h>

using dftracer::utils::server::CancelRegistry;
using dftracer::utils::server::CancelToken;

TEST_SUITE("CancelToken") {
    TEST_CASE("default token is never cancelled") {
        CancelToken t;
        CHECK_FALSE(t.cancelled());
        CHECK_FALSE(static_cast<bool>(t));
        t.cancel();  // no-op, must not crash
        CHECK_FALSE(t.cancelled());
    }

    TEST_CASE("registered token flips via registry by id") {
        CancelRegistry reg;
        auto t = reg.create("req-1");
        CHECK(static_cast<bool>(t));
        CHECK_FALSE(t.cancelled());

        CHECK(reg.cancel("req-1"));
        CHECK(t.cancelled());
    }

    TEST_CASE("cancel of unknown id returns false") {
        CancelRegistry reg;
        CHECK_FALSE(reg.cancel("nope"));
        CHECK_FALSE(reg.cancel(""));
    }

    TEST_CASE("copies share the flag") {
        CancelRegistry reg;
        auto a = reg.create("r");
        CancelToken b = a;
        reg.cancel("r");
        CHECK(a.cancelled());
        CHECK(b.cancelled());
    }

    TEST_CASE("empty id yields unregistered but usable token") {
        CancelRegistry reg;
        auto t = reg.create("");
        CHECK(static_cast<bool>(t));
        CHECK(reg.size() == 0);
        t.cancel();
        CHECK(t.cancelled());
    }

    TEST_CASE("remove unregisters and size tracks in-flight") {
        CancelRegistry reg;
        (void)reg.create("a");
        (void)reg.create("b");
        CHECK(reg.size() == 2);
        reg.remove("a");
        CHECK(reg.size() == 1);
        CHECK_FALSE(reg.cancel("a"));
        CHECK(reg.cancel("b"));
    }

    TEST_CASE("direct cancel on token works without registry") {
        CancelRegistry reg;
        auto t = reg.create("x");
        t.cancel();
        CHECK(t.cancelled());
    }

    TEST_CASE("token detects peer disconnect via socket probe") {
        int fds[2];
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

        CancelRegistry reg;
        auto t = reg.create("d", fds[0]);
        CHECK_FALSE(t.cancelled());  // peer still connected

        ::close(fds[1]);             // client goes away

        // The probe is throttled to every 16th check; poll a few dozen times.
        bool detected = false;
        for (int i = 0; i < 64 && !detected; ++i) detected = t.cancelled();
        CHECK(detected);

        ::close(fds[0]);
    }

    TEST_CASE("connected peer with no data is not cancelled") {
        int fds[2];
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

        CancelRegistry reg;
        auto t = reg.create("d", fds[0]);
        bool any = false;
        for (int i = 0; i < 64; ++i) any = any || t.cancelled();
        CHECK_FALSE(any);

        ::close(fds[0]);
        ::close(fds[1]);
    }
}
