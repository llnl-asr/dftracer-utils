// SPDX-License-Identifier: MIT
// Exercises the C++ SDK wrappers added over the plugin C ABI's fuller io
// surface (vectored readv/writev/preadv/pwritev, lseek, sendfile), the
// ergonomic Writer over the writer extension, and the coro spawn/then
// combinators. Each drives a real PluginFold-wired dftu_plugin_host, the way a
// loaded plugin would.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <plugins/test_abi_ctx.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <unistd.h>

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
    dstd::UniquePtr<plugins::host::PluginFold> fold =
        dstd::make_unique<plugins::host::PluginFold>(plugin, intern);
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

// Like drive_task, for a task that is meant to fail: its status and message.
template <class MakeTask>
dftu_status drive_status(dftu_plugin_host& host, MakeTask make,
                         dstd::String* message) {
    test_plugin_common::TaskDriver driver(host);
    return driver.run(make(driver.host()), message);
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

using dftracer::utils::plugins::Host;
using dftracer::utils::plugins::Task;

Task read_lines_sdk(Host h, const char* path, std::uint64_t start,
                    std::uint64_t end, dstd::Vector<dstd::String>* out,
                    dstd::Vector<std::uint64_t>* firsts) {
    auto reader = h.fileio().lines(path, start, end);
    for (;;) {
        auto batch = co_await reader.next(2);
        if (!batch) break;
        firsts->push_back(batch->first_line());
        for (dstd::StringView line : *batch) out->push_back(dstd::String(line));
    }
}

Task read_chunks_sdk(Host h, const char* path, std::uint64_t chunk_size,
                     dstd::String* out) {
    auto reader = h.fileio().chunks(path, chunk_size);
    for (;;) {
        auto chunk = co_await reader.next();
        if (!chunk) break;
        out->append(chunk->data(), chunk->size());
    }
}

Task gz_roundtrip_sdk(Host h, const char* path, dftu_gz_summary* summary,
                      dstd::Vector<dstd::String>* back) {
    auto fio = h.fileio();
    auto writer = fio.gz(path);
    co_await writer.open();
    writer.append("alpha\nbeta\n");
    writer.append("gamma\n");
    *summary = co_await writer.close();
    auto reader = fio.lines(path);
    for (;;) {
        auto batch = co_await reader.next();
        if (!batch) break;
        for (dstd::StringView line : *batch)
            back->push_back(dstd::String(line));
    }
}

Task gz_bad_append_sdk(Host h, const char* path) {
    auto writer = h.fileio().gz(path);
    co_await writer.open();
    writer.append("no newline");
}

Task compress_roundtrip_sdk(Host h, const char* in, const char* gz,
                            const char* back, std::uint64_t* gz_size,
                            std::uint64_t* back_size) {
    auto fio = h.fileio();
    co_await fio.compress(in, gz, gz_size);
    co_await fio.decompress(gz, back, back_size);
}

Task compress_missing_sdk(Host h, const char* in, const char* out) {
    std::uint64_t size = 0;
    co_await h.fileio().compress(in, out, &size);
}

Task lines_missing_sdk(Host h, const char* path) {
    auto reader = h.fileio().lines(path);
    co_return;
}

Task helpers_sdk(Host h, const char* path, dstd::String* logical,
                 std::int64_t* mtime, std::uint64_t* size,
                 std::uint64_t* hash) {
    auto fio = h.fileio();
    *logical = fio.logical_path(path);
    *mtime = fio.mtime(path);
    *size = fio.size(path);
    *hash = fio.sampled_hash(path);
    co_return;
}

Task size_missing_sdk(Host h, const char* path) {
    (void)h.fileio().size(path);
    co_return;
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

namespace {

// mkdir, rename and rmdir through the SDK, then a failing unlink read as a
// status and a Linux-only op, which refuses off Linux.
dftracer::utils::plugins::Task sdk_dir_ops(dftracer::utils::plugins::Host h,
                                           const char* dir, const char* a,
                                           const char* b, const char* missing,
                                           dftu_status* unlink_status,
                                           dftu_statx* sx) {
    dftracer::utils::plugins::Io io = h.io();
    co_await io.mkdir(dir, 0755);
    co_await io.rename(dir, a);
    co_await io.symlinkat(a, AT_FDCWD, b);
    co_await io.unlink(b);
    auto bad = io.unlink(missing);
    *unlink_status = co_await bad.nothrow();
    co_await io.statx(AT_FDCWD, a, 0, 0xfff, sx);
    co_await io.rmdir(a);
}

// One loopback round trip with the socket calls of the SDK.
dftracer::utils::plugins::Task sdk_loopback(dftracer::utils::plugins::Host h,
                                            std::int64_t* got) {
    dftracer::utils::plugins::Io io = h.io();
    int srv = -1, cli = -1, conn = -1;
    co_await io.socket(AF_INET, SOCK_STREAM, 0, &srv);
    co_await io.socket(AF_INET, SOCK_STREAM, 0, &cli);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    co_await io.bind(srv, reinterpret_cast<const sockaddr*>(&addr),
                     sizeof(addr));
    co_await io.listen(srv, 1);
    socklen_t len = sizeof(addr);
    ::getsockname(srv, reinterpret_cast<sockaddr*>(&addr), &len);
    co_await io.connect(cli, reinterpret_cast<const sockaddr*>(&addr),
                        sizeof(addr));
    co_await io.accept(srv, nullptr, nullptr, &conn);
    char out[] = "sdk";
    struct iovec oi = {out, 3};
    struct msghdr om{};
    om.msg_iov = &oi;
    om.msg_iovlen = 1;
    std::int64_t sent = 0;
    co_await io.sendmsg(cli, &om, 0, &sent);
    std::uint32_t events = 0;
    co_await io.poll_add(conn, POLLIN, &events);
    char in[8] = {};
    struct iovec ii = {in, sizeof(in)};
    struct msghdr im{};
    im.msg_iov = &ii;
    im.msg_iovlen = 1;
    co_await io.recvmsg(conn, &im, 0, got);
    co_await io.shutdown(cli, SHUT_WR);
    co_await io.close(conn);
    co_await io.close(cli);
    co_await io.close(srv);
}

}  // namespace

TEST_CASE("plugin cxx: Io directory and link ops, a failed op, a platform op") {
    dftu_utils_test::TestEnvironment env(0);
    const dstd::String dir = env.dir() + "/sdk_dir";
    const dstd::String renamed = env.dir() + "/sdk_renamed";
    const dstd::String link = env.dir() + "/sdk_link";
    const dstd::String missing = env.dir() + "/sdk_missing";
    HostFixture fx;
    test_plugin_common::TaskDriver driver(fx.host());
    dftu_status unlink_status = DFTU_OK;
    dftu_statx sx{};
    dstd::String message;
    const dftu_status st = driver.run(
        sdk_dir_ops(driver.host(), dir.c_str(), renamed.c_str(), link.c_str(),
                    missing.c_str(), &unlink_status, &sx),
        &message);
    CHECK(unlink_status == DFTU_E_NOT_FOUND);
#ifdef __linux__
    CHECK(st == DFTU_OK);
    CHECK(sx.mask != 0);
#else
    CHECK(st == DFTU_E_UNSUPPORTED);
    CHECK(message.find("statx") != dstd::String::npos);
#endif
}

TEST_CASE("plugin cxx: Io socket ops carry a message over loopback") {
    HostFixture fx;
    std::int64_t got = 0;
    drive_task(fx.host(), [&](dftracer::utils::plugins::Host h) {
        return sdk_loopback(h, &got);
    });
    CHECK(got == 3);
}

namespace {

struct FsSdkResult {
    dstd::String read_back;
    dstd::String at_back;
    std::uint64_t listed = 0;
    std::uint64_t found = 0;
    dstd::String first_found;
    dftu_status missing_status = DFTU_OK;
    dftu_status stat_status[2] = {DFTU_OK, DFTU_OK};
    std::uint64_t stat_size = 0;
};

// write_file, read_file, positioned I/O, listing, find_all and stat_batch
// through the SDK, and a failing read_file read as a status.
dftracer::utils::plugins::Task sdk_fs_ops(dftracer::utils::plugins::Host h,
                                          const char* dir, const char* file,
                                          const char* missing,
                                          FsSdkResult* out) {
    dftracer::utils::plugins::Fs fs = h.fs();
    std::int32_t created = 0;
    co_await fs.create_directories(dir, &created);
    co_await fs.write_file(file, "sdk file", 8);

    dftracer::utils::plugins::FileBytes bytes;
    co_await fs.read_file(file, bytes);
    out->read_back.assign(bytes.view().data(), bytes.view().size());

    std::int64_t n = 0;
    co_await fs.write_at(file, 4, "FILE", 4, &n);
    char buf[8] = {};
    co_await fs.read_at(file, 0, buf, 8, &n);
    out->at_back.assign(buf, static_cast<std::size_t>(n));

    dftracer::utils::plugins::PathList entries = fs.new_list();
    co_await fs.list_dir(dir, entries);
    out->listed = entries.size();

    dftracer::utils::plugins::PathList files = fs.new_list();
    co_await fs.find_all(dir, nullptr, files);
    out->found = files.size();
    if (files.size() > 0) {
        auto first = files.entry(0);
        out->first_found.assign(first.path.data(), first.path.size());
    }

    const char* paths[2] = {file, missing};
    dftu_fs_stat stats[2] = {};
    co_await fs.stat_batch(paths, 2, stats, out->stat_status);
    out->stat_size = stats[0].size;

    dftracer::utils::plugins::FileBytes none;
    auto bad = fs.read_file(missing, none);
    out->missing_status = co_await bad.nothrow();

    std::int32_t removed = 0;
    co_await fs.remove(file, &removed);
    co_await fs.remove(dir, &removed);
}

}  // namespace

TEST_CASE("plugin cxx: Fs reads, writes, lists, finds and stats files") {
    dftu_utils_test::TestEnvironment env(0);
    const dstd::String dir = env.dir() + "/sdk_fs";
    const dstd::String file = dir + "/f.txt";
    const dstd::String missing = dir + "/missing.txt";
    HostFixture fx;
    FsSdkResult r;
    drive_task(fx.host(), [&](dftracer::utils::plugins::Host h) {
        return sdk_fs_ops(h, dir.c_str(), file.c_str(), missing.c_str(), &r);
    });
    CHECK(r.read_back == "sdk file");
    CHECK(r.at_back == "sdk FILE");
    CHECK(r.listed == 1);
    CHECK(r.found == 1);
    CHECK(r.first_found == file);
    CHECK(r.missing_status == DFTU_E_NOT_FOUND);
    CHECK(r.stat_status[0] == DFTU_OK);
    CHECK(r.stat_status[1] == DFTU_E_NOT_FOUND);
    CHECK(r.stat_size == 8);
}

TEST_CASE("plugin cxx: LineReader batches a plain file by line range") {
    dftu_utils_test::TestEnvironment env(0);
    dstd::String path = env.dir() + "/sdk_lines.txt";
    dstd::fs::write_file(dstd::PathView(path), dstd::String("a\nb\nc\nd\ne"))
        .discard();

    HostFixture fx;
    dstd::Vector<dstd::String> lines;
    dstd::Vector<std::uint64_t> firsts;
    drive_task(fx.host(), [&](Host h) {
        return read_lines_sdk(h, path.c_str(), 2, 0, &lines, &firsts);
    });
    REQUIRE(lines.size() == 4);
    CHECK(lines[0] == "b");
    CHECK(lines[3] == "e");
    REQUIRE(firsts.size() == 2);
    CHECK(firsts[0] == 2);
    CHECK(firsts[1] == 4);
}

TEST_CASE("plugin cxx: LineReader on a missing file throws NOT_FOUND") {
    dftu_utils_test::TestEnvironment env(0);
    dstd::String path = env.dir() + "/absent.txt";
    HostFixture fx;
    dstd::String message;
    const dftu_status status = drive_status(
        fx.host(), [&](Host h) { return lines_missing_sdk(h, path.c_str()); },
        &message);
    CHECK(status == DFTU_E_NOT_FOUND);
    CHECK(message.find("lines_open") != dstd::String::npos);
}

TEST_CASE("plugin cxx: ChunkReader reads a file in chunks") {
    dftu_utils_test::TestEnvironment env(0);
    dstd::String path = env.dir() + "/sdk_chunks.bin";
    const dstd::String body = "0123456789abcdef0123";
    dstd::fs::write_file(dstd::PathView(path), body).discard();

    HostFixture fx;
    dstd::String got;
    drive_task(fx.host(), [&](Host h) {
        return read_chunks_sdk(h, path.c_str(), 7, &got);
    });
    CHECK(got == body);
}

TEST_CASE("plugin cxx: GzWriter writes lines that LineReader reads back") {
    dftu_utils_test::TestEnvironment env(0);
    dstd::String path = env.dir() + "/sdk_gz.pfw.gz";
    HostFixture fx;
    dftu_gz_summary summary{};
    dstd::Vector<dstd::String> back;
    drive_task(fx.host(), [&](Host h) {
        return gz_roundtrip_sdk(h, path.c_str(), &summary, &back);
    });
    CHECK(summary.lines == 3);
    CHECK(summary.uc_bytes == 17);
    REQUIRE(back.size() == 3);
    CHECK(back[0] == "alpha");
    CHECK(back[2] == "gamma");
}

TEST_CASE("plugin cxx: GzWriter append without a final newline is INVALID") {
    dftu_utils_test::TestEnvironment env(0);
    dstd::String path = env.dir() + "/sdk_bad.pfw.gz";
    HostFixture fx;
    dstd::String message;
    const dftu_status status = drive_status(
        fx.host(), [&](Host h) { return gz_bad_append_sdk(h, path.c_str()); },
        &message);
    CHECK(status == DFTU_E_INVALID);
}

TEST_CASE("plugin cxx: Fileio compress then decompress round-trips") {
    dftu_utils_test::TestEnvironment env(0);
    dstd::String in = env.dir() + "/sdk_in.txt";
    dstd::String gz = env.dir() + "/sdk_in.txt.gz";
    dstd::String back = env.dir() + "/sdk_back.txt";
    dstd::String text;
    for (int i = 0; i < 100; ++i) text += dstd::fmt<"line {}\n">(i);
    dstd::fs::write_file(dstd::PathView(in), text).discard();

    HostFixture fx;
    std::uint64_t gz_size = 0, back_size = 0;
    drive_task(fx.host(), [&](Host h) {
        return compress_roundtrip_sdk(h, in.c_str(), gz.c_str(), back.c_str(),
                                      &gz_size, &back_size);
    });
    CHECK(gz_size > 0);
    CHECK(back_size == text.size());
    CHECK(read_all(back) == text);
}

TEST_CASE("plugin cxx: Fileio compress of a missing input is NOT_FOUND") {
    dftu_utils_test::TestEnvironment env(0);
    dstd::String in = env.dir() + "/absent.txt";
    dstd::String out = env.dir() + "/absent.gz";
    HostFixture fx;
    dstd::String message;
    const dftu_status status = drive_status(
        fx.host(),
        [&](Host h) {
            return compress_missing_sdk(h, in.c_str(), out.c_str());
        },
        &message);
    CHECK(status == DFTU_E_NOT_FOUND);
    CHECK(message.find("compress") != dstd::String::npos);
}

TEST_CASE("plugin cxx: Fileio helpers answer and throw on a missing file") {
    dftu_utils_test::TestEnvironment env(0);
    dstd::String path = env.dir() + "/sdk_helper.txt";
    dstd::fs::write_file(dstd::PathView(path), dstd::String("abc")).discard();

    HostFixture fx;
    dstd::String logical;
    std::int64_t mtime = 0;
    std::uint64_t size = 0, hash = 0;
    drive_task(fx.host(), [&](Host h) {
        return helpers_sdk(h, path.c_str(), &logical, &mtime, &size, &hash);
    });
    CHECK(logical.find("sdk_helper.txt") != dstd::String::npos);
    CHECK(mtime > 0);
    CHECK(size == 3);
    CHECK(hash != 0);

    dstd::String missing = env.dir() + "/absent.txt";
    dstd::String message;
    const dftu_status status = drive_status(
        fx.host(), [&](Host h) { return size_missing_sdk(h, missing.c_str()); },
        &message);
    CHECK(status == DFTU_E_NOT_FOUND);
}
