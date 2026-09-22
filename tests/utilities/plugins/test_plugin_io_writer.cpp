// Exercises the C++ SDK wrappers added over the plugin C ABI's fuller io
// surface (vectored readv/writev/preadv/pwritev, lseek, sendfile), the
// ergonomic Writer over the writer extension, and the coro spawn/then
// combinators. Each drives a real PluginFold-wired dftu_plugin_host, the way a
// loaded plugin would.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/fold_adapter.h>
#include <dftracer/utils/plugins/plugin.h>
// compose.h pulls plugin.h/arrow_abi.h; keep it after fold_adapter.h so
// nanoarrow's full definitions win over arrow_abi.h's fallback structs.
#include <dftracer/utils/plugins/compose.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "testing_utilities.h"

using dftracer::utils::CoroScope;
using dftracer::utils::Runtime;
using dftracer::utils::StringIntern;
using dftracer::utils::plugins::PluginFold;
namespace coro = dftracer::utils::coro;

namespace {

struct TrivialSlice {
    explicit TrivialSlice(const dftracer::utils::plugins::Config&) {}
    void step(const dftu_dataframe*, dftracer::utils::plugins::Host) {}
    void merge(TrivialSlice&) {}
    void finalize(dftracer::utils::plugins::Host) {}
};

struct HostFixture {
    StringIntern intern;
    dftu_plugin* plugin =
        dftracer::utils::plugins::make_plugin<TrivialSlice>(nullptr);
    std::unique_ptr<PluginFold> fold =
        std::make_unique<PluginFold>(plugin, intern);
    dftu_plugin_host& host() { return fold->host(); }
    ~HostFixture() {
        fold.reset();
        if (plugin && plugin->destroy) plugin->destroy(plugin->self);
    }
};

std::uint64_t file_size(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0
               ? static_cast<std::uint64_t>(st.st_size)
               : 0;
}

std::string read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

// Drive a plugin Task to completion on a fresh Runtime, exactly as the fuse
// worker drives a plugin coroutine.
template <class MakeTask>
void drive_task(dftu_plugin_host& host, int workers, MakeTask make) {
    Runtime rt(workers);
    rt.scope("caller", [&](CoroScope&) -> coro::CoroTask<void> {
          dftu_task* driver =
              dftracer::utils::plugins::detail::drive_coro<int>(&host, make());
          co_await *reinterpret_cast<coro::CoroTask<void>*>(driver);
      }).wait();
    rt.shutdown();
}

// Vectored write then vectored positional read back, with an lseek in between.
dftracer::utils::plugins::Task vectored_roundtrip(
    dftracer::utils::plugins::Host h, const char* path, std::int64_t* wrote,
    std::int64_t* off, std::string* got) {
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

    int rc = 0;
    co_await io.close(fd, &rc);
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
    int rc = 0;
    co_await io.close(in, &rc);
}

// pwrite a payload, then pwritev-append after it, and pread the whole back.
dftracer::utils::plugins::Task positional_vectored(
    dftracer::utils::plugins::Host h, const char* path, std::string* got) {
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
    int rc = 0;
    co_await io.close(fd, &rc);
}

// Two spawns joined with all(), then a then() runs after a spawn completes.
dftracer::utils::plugins::Task spawn_then(dftracer::utils::plugins::Host h,
                                          int* order) {
    int a_done = 0, b_done = 0, after = 0;
    auto fa = [&] { a_done = 1; };
    auto fb = [&] { b_done = 1; };
    dftu_task* ta = h.spawn(fa);
    dftu_task* tb = h.spawn(fb);
    co_await h.all({ta, tb});
    *order = (a_done && b_done) ? 1 : 0;

    auto fc = [&] { after = a_done + b_done; };  // sees both prior spawns
    dftu_task* tc = h.spawn(fc);
    auto seal = [&] { *order += (after == 2) ? 10 : 0; };
    co_await h.then(tc, seal);
}

// Typed compose: a piped op, an op-level when_all (concatenated outputs), and
// an op-level when_any (first to finish).
dftracer::utils::plugins::Task compose_ops(dftracer::utils::plugins::Host h,
                                           std::int64_t* piped,
                                           std::int64_t* all0,
                                           std::int64_t* all1,
                                           std::int64_t* raced) {
    using dftracer::utils::plugins::make_op;
    using dftracer::utils::plugins::Op;
    auto a = make_op<std::int64_t, std::int64_t>(
        h, [](std::int64_t x) { return x * 2; });
    auto b = make_op<std::int64_t, std::int64_t>(
        h, [](std::int64_t x) { return x + 1; });

    std::int64_t in = 5;
    int rc = 0;
    std::int64_t p = 0;
    co_await dftracer::utils::plugins::run(a | b, in, p, rc);  // (5*2)+1 == 11
    *piped = p;

    std::array<std::int64_t, 2> both{};
    co_await dftracer::utils::plugins::run(
        dftracer::utils::plugins::when_all(a, b), in, both, rc);
    *all0 = both[0];  // 10
    *all1 = both[1];  // 6

    std::int64_t one = 0;
    co_await dftracer::utils::plugins::run(
        dftracer::utils::plugins::when_any(a, b), in, one, rc);
    *raced = one;  // 10 or 6
}

// Free function (not a coroutine lambda) so the frame's captures cannot dangle.
dftracer::utils::plugins::Task write_via_writer(
    dftracer::utils::plugins::Host h, const char* path) {
    dftracer::utils::plugins::Writer w = h.writer(path, 1, /*gzip=*/false);
    if (!w) co_return;
    co_await w.open();
    co_await w.chunk(0, std::string_view{"writer-chunk-payload\n"});
    co_await w.close();
}

}  // namespace

TEST_CASE("plugin cxx: vectored io writev/preadv and lseek round-trip") {
    dftu_utils_test::TestEnvironment env(0);
    std::string path = env.get_dir() + "/vec.bin";

    HostFixture fx;
    std::int64_t wrote = 0, off = 0;
    std::string got;
    drive_task(fx.host(), 1, [&] {
        return vectored_roundtrip(dftracer::utils::plugins::Host{&fx.host()},
                                  path.c_str(), &wrote, &off, &got);
    });

    CHECK(wrote == 20);
    CHECK(off == 20);  // lseek SEEK_CUR reported the post-write offset
    CHECK(got == "hello vectored world");
}

TEST_CASE("plugin cxx: positional vectored pwritev/pwrite round-trip") {
    dftu_utils_test::TestEnvironment env(0);
    std::string path = env.get_dir() + "/posvec.bin";
    HostFixture fx;
    std::string got;
    drive_task(fx.host(), 1, [&] {
        return positional_vectored(dftracer::utils::plugins::Host{&fx.host()},
                                   path.c_str(), &got);
    });
    CHECK(got == "AAAABBCC");
}

TEST_CASE("plugin cxx: sendfile transfers a file into a socket") {
    dftu_utils_test::TestEnvironment env(0);
    std::string src = env.get_dir() + "/src.bin";
    const std::string body = "zero-copy transfer payload\n";
    {
        std::ofstream f(src, std::ios::binary);
        f << body;
    }

    int sv[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    HostFixture fx;
    std::int64_t sent = 0;
    drive_task(fx.host(), 1, [&] {
        return sendfile_to_fd(dftracer::utils::plugins::Host{&fx.host()},
                              src.c_str(), sv[1], body.size(), &sent);
    });

    CHECK(sent == static_cast<std::int64_t>(body.size()));
    char buf[64] = {};
    ssize_t n = ::read(sv[0], buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(body.size()));
    CHECK(std::string(buf, body.size()) == body);
    ::close(sv[0]);
    ::close(sv[1]);
}

TEST_CASE("plugin cxx: spawn joined with all() and then() sequences work") {
    HostFixture fx;
    int order = 0;
    drive_task(fx.host(), 2, [&] {
        return spawn_then(dftracer::utils::plugins::Host{&fx.host()}, &order);
    });
    CHECK(order == 11);  // both spawns ran (1) and then() observed both (+10)
}

TEST_CASE("plugin cxx: Writer open/chunk/close writes a non-empty file") {
    dftu_utils_test::TestEnvironment env(0);
    std::string path = env.get_dir() + "/writer_out.dat";

    HostFixture fx;
    drive_task(fx.host(), 1, [&] {
        return write_via_writer(dftracer::utils::plugins::Host{&fx.host()},
                                path.c_str());
    });

    // The sharded writer emits per-worker shard files ("<path>.shard_<N>").
    std::string shard = path + ".shard_0";
    CHECK(file_size(shard) > 0);
    CHECK(read_all(shard) == "writer-chunk-payload\n");
}

static dftracer::utils::plugins::Task merge_two(
    dftracer::utils::plugins::Host h, const char* target, const char* a,
    const char* b) {
    co_await h.merge_shards(target, {a, b});
}

TEST_CASE("plugin cxx: typed compose pipe/when_all/when_any run") {
    HostFixture fx;
    std::int64_t piped = 0, all0 = 0, all1 = 0, raced = 0;
    drive_task(fx.host(), 2, [&] {
        return compose_ops(dftracer::utils::plugins::Host{&fx.host()}, &piped,
                           &all0, &all1, &raced);
    });
    CHECK(piped == 11);  // (5*2)+1
    CHECK(all0 == 10);   // 5*2
    CHECK(all1 == 6);    // 5+1
    CHECK((raced == 10 || raced == 6));
}

TEST_CASE("plugin cxx: merge_shards concatenates shard files into a target") {
    dftu_utils_test::TestEnvironment env(0);
    std::string a = env.get_dir() + "/a.shard";
    std::string b = env.get_dir() + "/b.shard";
    std::string target = env.get_dir() + "/merged.out";
    {
        std::ofstream fa(a, std::ios::binary);
        fa << "AAAA";
        std::ofstream fb(b, std::ios::binary);
        fb << "BBBB";
    }

    HostFixture fx;
    drive_task(fx.host(), 1, [&] {
        return merge_two(dftracer::utils::plugins::Host{&fx.host()},
                         target.c_str(), a.c_str(), b.c_str());
    });

    CHECK(read_all(target) == "AAAABBBB");
}
