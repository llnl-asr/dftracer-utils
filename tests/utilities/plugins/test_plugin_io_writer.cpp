// SPDX-License-Identifier: MIT
// Exercises the C++ SDK wrappers added over the plugin C ABI's fuller io
// surface (vectored readv/writev/preadv/pwritev, lseek, sendfile), the
// ergonomic Writer over the writer extension, and the coro spawn/then
// combinators. Each drives a real PluginFold-wired dftu_plugin_host, the way a
// loaded plugin would.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/domains/plugins/fold_adapter.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <utilities/plugins/test_abi_ctx.h>

#include <array>
#include <cstdint>
#include <cstring>

using namespace dftracer::utils;

namespace {

struct TrivialSlice {
    explicit TrivialSlice(const dftracer::utils::plugins::Config&) {}
    void step(const dftu_dataframe*, dftracer::utils::plugins::Host) {}
    void merge(TrivialSlice&) {}
    void finalize(dftracer::utils::plugins::Host) {}
};

struct HostFixture {
    dstd::StringIntern intern;
    dftu_plugin* plugin =
        dftracer::utils::plugins::make_plugin<TrivialSlice>(nullptr);
    dstd::UniquePtr<domains::plugins::PluginFold> fold =
        dstd::make_unique<domains::plugins::PluginFold>(plugin, intern);
    dftu_plugin_host& host() { return fold->host(); }
    ~HostFixture() {
        fold.reset();
        if (plugin && plugin->destroy) plugin->destroy(plugin->self);
    }
};

std::uint64_t file_size(const dstd::String& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0
               ? static_cast<std::uint64_t>(st.st_size)
               : 0;
}

dstd::String read_all(const dstd::String& path) {
    return dstd::fs::read_file(dstd::PathView(path)).value_or(dstd::String());
}

// Drive a plugin Task to completion the way the fold drives a plugin
// coroutine, and check it ends with DFTU_OK.
template <class MakeTask>
void drive_task(dftu_plugin_host& host, MakeTask make) {
    test_plugin_common::TaskDriver driver(host);
    dstd::String message;
    CHECK(driver.run(make(driver.host()), &message) == DFTU_OK);
    CHECK(message.empty());
}

// Vectored write then vectored positional read back, with an lseek in between.
dftracer::utils::plugins::Task vectored_roundtrip(
    dftracer::utils::plugins::Host h, const char* path, std::int64_t* wrote,
    std::int64_t* off, dstd::String* got) {
    dftracer::utils::plugins::Io io = h.io();
    int fd = -1;
    co_await io.open(path, O_RDWR | O_CREAT | O_TRUNC, 0644, &fd);
    if (fd < 0) co_return;

    const char a[] = "hello ";
    const char b[] = "vectored world";
    struct iovec wv[2];
    wv[0].iov_base = const_cast<char*>(a);
    wv[0].iov_len = 6;  // no NUL
    wv[1].iov_base = const_cast<char*>(b);
    wv[1].iov_len = 14;
    co_await io.writev(fd, wv, 2, wrote);

    // Confirm the offset advanced to the total written via SEEK_CUR(0).
    co_await io.lseek(fd, 0, SEEK_CUR, off);

    char buf0[6] = {};
    char buf1[14] = {};
    struct iovec rv[2];
    rv[0].iov_base = buf0;
    rv[0].iov_len = 6;
    rv[1].iov_base = buf1;
    rv[1].iov_len = 14;
    std::int64_t rn = 0;
    co_await io.preadv(fd, rv, 2, 0, &rn);
    got->assign(buf0, 6);
    got->append(buf1, 14);

    co_await io.close(fd);
}

// sendfile the whole of src into an already-open destination fd (a socket, so
// the path is portable: macOS sendfile only targets sockets).
dftracer::utils::plugins::Task sendfile_to_fd(dftracer::utils::plugins::Host h,
                                              const char* src, int out_fd,
                                              std::uint64_t count,
                                              std::int64_t* sent) {
    dftracer::utils::plugins::Io io = h.io();
    int in = -1;
    co_await io.open(src, O_RDONLY, 0, &in);
    if (in < 0) co_return;
    co_await io.sendfile(out_fd, in, 0, count, sent);
    co_await io.close(in);
}

// pwrite a payload, then pwritev-append after it, and pread the whole back.
dftracer::utils::plugins::Task positional_vectored(
    dftracer::utils::plugins::Host h, const char* path, dstd::String* got) {
    dftracer::utils::plugins::Io io = h.io();
    int fd = -1;
    co_await io.open(path, O_RDWR | O_CREAT | O_TRUNC, 0644, &fd);
    if (fd < 0) co_return;
    const char head[] = "AAAA";
    std::int64_t n = 0;
    co_await io.pwrite(fd, head, 4, 0, &n);
    const char t0[] = "BB";
    const char t1[] = "CC";
    struct iovec wv[2];
    wv[0].iov_base = const_cast<char*>(t0);
    wv[0].iov_len = 2;
    wv[1].iov_base = const_cast<char*>(t1);
    wv[1].iov_len = 2;
    co_await io.pwritev(fd, wv, 2, 4, &n);
    char buf[8] = {};
    co_await io.pread(fd, buf, 8, 0, &n);
    got->assign(buf, 8);
    co_await io.close(fd);
}

// Two spawns joined with all(), then a spawn that runs after both completed.
dftracer::utils::plugins::Task spawn_then(dftracer::utils::plugins::Host h,
                                          int* order) {
    int a_done = 0, b_done = 0, after = 0;
    auto fa = [&] { a_done = 1; };
    auto fb = [&] { b_done = 1; };
    auto ta = h.spawn(fa);
    auto tb = h.spawn(fb);
    // GCC 12.1 rejects a braced list inside a co_await expression ("array used
    // as initializer"), so the op is built in its own statement.
    auto both = h.all({ta.release(), tb.release()});
    co_await both;
    *order = (a_done && b_done) ? 1 : 0;

    auto fc = [&] { after = a_done + b_done; };  // sees both prior spawns
    co_await h.spawn(fc);
    *order += (after == 2) ? 10 : 0;
}

// Typed jobs: a piped job, a job-level when_all (concatenated outputs), and a
// job-level when_any (first to finish).
dftracer::utils::plugins::Task job_ops(dftracer::utils::plugins::Host h,
                                       std::int64_t* piped, std::int64_t* all0,
                                       std::int64_t* all1,
                                       std::int64_t* raced) {
    auto a = plugins::make_job<std::int64_t, std::int64_t>(
        h, [](std::int64_t x) { return x * 2; });
    auto b = plugins::make_job<std::int64_t, std::int64_t>(
        h, [](std::int64_t x) { return x + 1; });

    std::int64_t in = 5;
    std::int64_t p = 0;
    co_await plugins::run(a | b, in, p);  // (5*2)+1 == 11
    *piped = p;

    std::array<std::int64_t, 2> both{};
    co_await plugins::run(plugins::when_all(a, b), in, both);
    *all0 = both[0];  // 10
    *all1 = both[1];  // 6

    std::int64_t one = 0;
    co_await plugins::run(plugins::when_any(a, b), in, one);
    *raced = one;  // 10 or 6
}

// Free function (not a coroutine lambda) so the frame's captures cannot dangle.
dftracer::utils::plugins::Task write_via_writer(
    dftracer::utils::plugins::Host h, const char* path) {
    dftracer::utils::plugins::Writer w = h.writer(path, 1, /*gzip=*/false);
    if (!w) co_return;
    co_await w.open();
    co_await w.chunk(0, dstd::StringView{"writer-chunk-payload\n"});
    co_await w.close();
}

}  // namespace

TEST_CASE("plugin cxx: vectored io writev/preadv and lseek round-trip") {
    dftu_utils_test::TestEnvironment env(0);
    dstd::String path = env.dir() + "/vec.bin";

    HostFixture fx;
    std::int64_t wrote = 0, off = 0;
    dstd::String got;
    drive_task(fx.host(), [&](dftracer::utils::plugins::Host h) {
        return vectored_roundtrip(h, path.c_str(), &wrote, &off, &got);
    });

    CHECK(wrote == 20);
    CHECK(off == 20);  // lseek SEEK_CUR reported the post-write offset
    CHECK(got == "hello vectored world");
}

TEST_CASE("plugin cxx: positional vectored pwritev/pwrite round-trip") {
    dftu_utils_test::TestEnvironment env(0);
    dstd::String path = env.dir() + "/posvec.bin";
    HostFixture fx;
    dstd::String got;
    drive_task(fx.host(), [&](dftracer::utils::plugins::Host h) {
        return positional_vectored(h, path.c_str(), &got);
    });
    CHECK(got == "AAAABBCC");
}

TEST_CASE("plugin cxx: sendfile transfers a file into a socket") {
    dftu_utils_test::TestEnvironment env(0);
    dstd::String src = env.dir() + "/src.bin";
    const dstd::String body = "zero-copy transfer payload\n";
    {
        dstd::fs::write_file(dstd::PathView(src), body).discard();
    }

    int sv[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    HostFixture fx;
    std::int64_t sent = 0;
    drive_task(fx.host(), [&](dftracer::utils::plugins::Host h) {
        return sendfile_to_fd(h, src.c_str(), sv[1], body.size(), &sent);
    });

    CHECK(sent == static_cast<std::int64_t>(body.size()));
    char buf[64] = {};
    ssize_t n = ::read(sv[0], buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(body.size()));
    CHECK(dstd::String(buf, body.size()) == body);
    ::close(sv[0]);
    ::close(sv[1]);
}

TEST_CASE(
    "plugin cxx: spawn joined with all() and a later spawn sequence work") {
    HostFixture fx;
    int order = 0;
    drive_task(fx.host(), [&](dftracer::utils::plugins::Host h) {
        return spawn_then(h, &order);
    });
    CHECK(order ==
          11);  // both spawns ran (1) and the later spawn observed both (+10)
}

TEST_CASE("plugin cxx: Writer open/chunk/close writes a non-empty file") {
    dftu_utils_test::TestEnvironment env(0);
    dstd::String path = env.dir() + "/writer_out.dat";

    HostFixture fx;
    drive_task(fx.host(), [&](dftracer::utils::plugins::Host h) {
        return write_via_writer(h, path.c_str());
    });

    // The sharded writer emits per-worker shard files ("<path>.shard_<N>").
    dstd::String shard = path + ".shard_0";
    CHECK(file_size(shard) > 0);
    CHECK(read_all(shard) == "writer-chunk-payload\n");
}

static dftracer::utils::plugins::Task merge_two(
    dftracer::utils::plugins::Host h, const char* target, const char* a,
    const char* b) {
    auto merge = h.merge_shards(target, {a, b});
    co_await merge;
}

TEST_CASE("plugin cxx: typed job pipe/when_all/when_any run") {
    HostFixture fx;
    std::int64_t piped = 0, all0 = 0, all1 = 0, raced = 0;
    drive_task(fx.host(), [&](dftracer::utils::plugins::Host h) {
        return job_ops(h, &piped, &all0, &all1, &raced);
    });
    CHECK(piped == 11);  // (5*2)+1
    CHECK(all0 == 10);   // 5*2
    CHECK(all1 == 6);    // 5+1
    CHECK((raced == 10 || raced == 6));
}

TEST_CASE("plugin cxx: merge_shards concatenates shard files into a target") {
    dftu_utils_test::TestEnvironment env(0);
    dstd::String a = env.dir() + "/a.shard";
    dstd::String b = env.dir() + "/b.shard";
    dstd::String target = env.dir() + "/merged.out";
    {
        dstd::String fa;
        fa += "AAAA";
        dstd::String fb;
        fb += "BBBB";
        dstd::fs::write_file(dstd::PathView(a), fa).discard();
        dstd::fs::write_file(dstd::PathView(b), fb).discard();
    }

    HostFixture fx;
    drive_task(fx.host(), [&](dftracer::utils::plugins::Host h) {
        return merge_two(h, target.c_str(), a.c_str(), b.c_str());
    });

    CHECK(read_all(target) == "AAAABBBB");
}
