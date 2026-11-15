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
    void step(dataframe::FrameView, dftracer::utils::plugins::Host) {}
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
    co_await io.writev(fd, dstd::Span<const struct iovec>(wv, 2), wrote);

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
    co_await io.preadv(fd, dstd::Span<const struct iovec>(rv, 2), 0, &rn);
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
    co_await io.pwrite(fd, dstd::ByteView(head, 4), 0, &n);
    const char t0[] = "BB";
    const char t1[] = "CC";
    struct iovec wv[2];
    wv[0].iov_base = const_cast<char*>(t0);
    wv[0].iov_len = 2;
    wv[1].iov_base = const_cast<char*>(t1);
    wv[1].iov_len = 2;
    co_await io.pwritev(fd, dstd::Span<const struct iovec>(wv, 2), 4, &n);
    char buf[8] = {};
    co_await io.pread(fd, dstd::MutableByteView(buf, 8), 0, &n);
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
    auto both = dftracer::utils::plugins::detail::all_raw(
        h, {dftracer::utils::plugins::detail::release_op(ta),
            dftracer::utils::plugins::detail::release_op(tb)});
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
    auto a = co_await plugins::make_job<std::int64_t, std::int64_t>(
        h, [](std::int64_t x) { return x * 2; });
    auto b = co_await plugins::make_job<std::int64_t, std::int64_t>(
        h, [](std::int64_t x) { return x + 1; });

    std::int64_t in = 5;
    std::int64_t p = 0;
    auto piped_job = co_await (a | b);
    co_await plugins::run(piped_job, in, p);  // (5*2)+1 == 11
    *piped = p;

    std::array<std::int64_t, 2> both{};
    auto all_job = co_await plugins::when_all(a, b);
    co_await plugins::run(all_job, in, both);
    *all0 = both[0];  // 10
    *all1 = both[1];  // 6

    std::int64_t one = 0;
    auto any_job = co_await plugins::when_any(a, b);
    co_await plugins::run(any_job, in, one);
    *raced = one;  // 10 or 6
}

// Free function (not a coroutine lambda) so the frame's captures cannot dangle.
dftracer::utils::plugins::Task write_via_writer(
    dftracer::utils::plugins::Host h, const char* path) {
    dftracer::utils::plugins::Writer w =
        co_await h.writer(path, 1, /*gzip=*/false);
    co_await w.open();
    co_await w.chunk(0, dstd::StringView{"writer-chunk-payload\n"});
    co_await w.close();
}

using dftracer::utils::plugins::Host;
using dftracer::utils::plugins::Task;

Task read_lines_sdk(Host h, const char* path, std::uint64_t start,
                    std::uint64_t end, dstd::Vector<dstd::String>* out,
                    dstd::Vector<std::uint64_t>* firsts) {
    auto reader = co_await h.fileio().lines(path, start, end);
    for (;;) {
        auto batch = co_await reader.next(2);
        if (!batch) break;
        firsts->push_back(batch->first_line());
        for (dstd::StringView line : *batch) out->push_back(dstd::String(line));
    }
}

Task read_chunks_sdk(Host h, const char* path, std::uint64_t chunk_size,
                     dstd::String* out) {
    auto reader = co_await h.fileio().chunks(path, chunk_size);
    for (;;) {
        auto chunk = co_await reader.next();
        if (!chunk) break;
        out->append(chunk->data(), chunk->size());
    }
}

Task gz_roundtrip_sdk(Host h, const char* path,
                      dftracer::utils::plugins::GzSummary* summary,
                      dstd::Vector<dstd::String>* back) {
    auto fio = h.fileio();
    auto writer = co_await fio.gz(path);
    co_await writer.open();
    co_await writer.append("alpha\nbeta\n");
    co_await writer.append("gamma\n");
    *summary = co_await writer.close();
    auto reader = co_await fio.lines(path);
    for (;;) {
        auto batch = co_await reader.next();
        if (!batch) break;
        for (dstd::StringView line : *batch)
            back->push_back(dstd::String(line));
    }
}

Task gz_bad_append_sdk(Host h, const char* path) {
    auto writer = co_await h.fileio().gz(path);
    co_await writer.open();
    co_await writer.append("no newline");
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
    auto reader = co_await h.fileio().lines(path);
}

Task helpers_sdk(Host h, const char* path, dstd::String* logical,
                 std::int64_t* mtime, std::uint64_t* size,
                 std::uint64_t* hash) {
    auto fio = h.fileio();
    const dstd::CStringView given(path);
    *logical = co_await fio.logical_path(given);
    *mtime = co_await fio.mtime(given);
    *size = co_await fio.size(given);
    *hash = co_await fio.sampled_hash(given);
}

Task size_missing_sdk(Host h, const char* path) {
    (void)co_await h.fileio().size(path);
}

// Write a payload with io.write and read it back with io.read, both through a
// view, then fstat the file and report its size.
Task buffer_roundtrip_sdk(Host h, dstd::CStringView path, dstd::ByteView in,
                          std::uint64_t* written, std::uint64_t* seen,
                          std::uint64_t* stat_size, dstd::String* got) {
    dftracer::utils::plugins::Io io = h.io();
    int fd = -1;
    co_await io.open(path, O_RDWR | O_CREAT | O_TRUNC, 0644, &fd);
    if (fd < 0) co_return;
    std::int64_t n = 0;
    co_await io.write(fd, in, &n);
    *written = static_cast<std::uint64_t>(n);
    co_await io.lseek(fd, 0, SEEK_SET, &n);
    char back[16] = {};
    co_await io.read(fd, dstd::MutableByteView(back, in.size()), &n);
    *seen = static_cast<std::uint64_t>(n);
    got->assign(back, static_cast<std::size_t>(n));
    dftracer::utils::plugins::Stat st;
    co_await io.fstat(fd, st);
    *stat_size = st.size();
    co_await io.close(fd);
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

TEST_CASE("plugin cxx: io.write and io.read round-trip a ByteView") {
    dftu_utils_test::TestEnvironment env(0);
    const dstd::String path = env.dir() + "/sdk_view.bin";
    const dstd::String body = "view-bytes";

    HostFixture fx;
    std::uint64_t written = 0, seen = 0, stat_size = 0;
    dstd::String got;
    drive_task(fx.host(), [&](Host h) {
        return buffer_roundtrip_sdk(h, dstd::CStringView(path),
                                    dstd::ByteView(body.data(), body.size()),
                                    &written, &seen, &stat_size, &got);
    });
    CHECK(written == body.size());
    CHECK(seen == body.size());
    CHECK(got == body);
    CHECK(stat_size == body.size());
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
dftracer::utils::plugins::Task sdk_dir_ops(
    dftracer::utils::plugins::Host h, const char* dir, const char* a,
    const char* b, const char* missing, dftu_status* unlink_status,
    dftracer::utils::plugins::Statx* sx) {
    dftracer::utils::plugins::Io io = h.io();
    co_await io.mkdir(dir, 0755);
    co_await io.rename(dir, a);
    co_await io.symlinkat(a, AT_FDCWD, b);
    co_await io.unlink(b);
    auto bad = io.unlink(missing);
    auto outcome = co_await bad.expected();
    *unlink_status =
        outcome.has_value() ? DFTU_OK : plugins::status_of(outcome.error());
    co_await io.statx(AT_FDCWD, a, 0, 0xfff, *sx);
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
    dftracer::utils::plugins::Statx sx;
    dstd::String message;
    const dftu_status st = driver.run(
        sdk_dir_ops(driver.host(), dir.c_str(), renamed.c_str(), link.c_str(),
                    missing.c_str(), &unlink_status, &sx),
        &message);
    CHECK(unlink_status == DFTU_E_NOT_FOUND);
#ifdef __linux__
    CHECK(st == DFTU_OK);
    CHECK(sx.mask() != 0);
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
    std::uint64_t first_size = 0;
    bool created = false;
    bool removed = false;
    dftu_status missing_status = DFTU_OK;
    dftracer::utils::plugins::StatStatus stat_status[2] = {};
    std::uint64_t stat_size = 0;
    std::uint64_t stat_ino = 0;
};

// write_file, read_file, positioned I/O, listing, find_all and stat_batch
// through the SDK, and a failing read_file read as a status.
dftracer::utils::plugins::Task sdk_fs_ops(dftracer::utils::plugins::Host h,
                                          const char* dir, const char* file,
                                          const char* missing,
                                          FsSdkResult* out) {
    using plugins::FileStat;
    using plugins::FindOptions;
    using plugins::Flag;
    using plugins::StatStatus;

    dftracer::utils::plugins::Fs fs = h.fs();
    Flag created;
    co_await fs.create_directories(dir, created);
    co_await fs.write_file(file, dstd::ByteView("sdk file", 8));
    out->created = created.set();

    dftracer::utils::plugins::FileBytes bytes;
    co_await fs.read_file(file, bytes);
    out->read_back.assign(bytes.view().data(), bytes.view().size());

    std::int64_t n = 0;
    co_await fs.write_at(file, 4, dstd::ByteView("FILE", 4), &n);
    char buf[8] = {};
    co_await fs.read_at(file, 0, dstd::MutableByteView(buf, 8), &n);
    out->at_back.assign(buf, static_cast<std::size_t>(n));

    dftracer::utils::plugins::PathList entries = fs.new_list();
    co_await fs.list_dir(dir, entries);
    out->listed = entries.size();

    FindOptions options;
    options.limit = 4;
    dftracer::utils::plugins::PathList files = fs.new_list();
    co_await fs.find_all(dir, &options, files);
    out->found = files.size();
    if (files.size() > 0) {
        auto first = co_await files.entry(0);
        out->first_found.assign(first.path.data(), first.path.size());
        out->first_size = first.size;
    }

    const dstd::CStringView paths[2] = {file, missing};
    FileStat stats[2];
    co_await fs.stat_batch(dstd::Span<const dstd::CStringView>(paths, 2),
                           dstd::Span<FileStat>(stats, 2),
                           dstd::Span<StatStatus>(out->stat_status, 2));
    out->stat_size = stats[0].size();
    out->stat_ino = stats[0].ino();

    dftracer::utils::plugins::FileBytes none;
    auto bad = fs.read_file(missing, none);
    auto outcome = co_await bad.expected();
    out->missing_status =
        outcome.has_value() ? DFTU_OK : plugins::status_of(outcome.error());

    Flag removed;
    co_await fs.remove(file, removed);
    out->removed = removed.set();
    co_await fs.remove(dir, removed);
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
    CHECK(r.first_size == 8);
    CHECK(r.created);
    CHECK(r.removed);
    CHECK(r.missing_status == DFTU_E_NOT_FOUND);
    CHECK(r.stat_status[0].ok());
    CHECK(r.stat_size == 8);
    CHECK(r.stat_ino != 0);
    CHECK(!r.stat_status[1].ok());
    CHECK(r.stat_status[1].condition() == dstd::Condition::NotFound);
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

TEST_CASE("plugin cxx: LineReader on a missing file fails with NOT_FOUND") {
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
    dftracer::utils::plugins::GzSummary summary;
    dstd::Vector<dstd::String> back;
    drive_task(fx.host(), [&](Host h) {
        return gz_roundtrip_sdk(h, path.c_str(), &summary, &back);
    });
    CHECK(summary.lines() == 3);
    CHECK(summary.uc_bytes() == 17);
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

namespace {

using namespace dftracer::utils::plugins;

// stat_batch takes one stat and one status slot per path; a shorter span is
// refused before the host writes into it.
Task stat_batch_short_sdk(Host h, dstd::Span<const dstd::CStringView> paths,
                          dstd::Span<FileStat> out,
                          dstd::Span<StatStatus> statuses) {
    co_await h.fs().stat_batch(paths, out, statuses);
}

struct FoundSdk {
    dstd::Vector<dstd::String> paths;
    dstd::Vector<std::uint64_t> sizes;
    dstd::Vector<FileKind> kinds;
};

Task find_patterns_sdk(Host h, dstd::CStringView root,
                       dstd::Span<const dstd::CStringView> patterns,
                       dstd::Span<const dstd::CStringView> skip,
                       FoundSdk* out) {
    dftracer::utils::plugins::Fs fs = h.fs();
    FindOptions options;
    options.patterns = patterns;
    options.skip_dirs = skip;
    PathList found = fs.new_list();
    co_await fs.find_all(root, &options, found);
    const std::uint64_t n = found.size();
    for (std::uint64_t i = 0; i < n; ++i) {
        const PathEntry entry = co_await found.entry(i);
        out->paths.push_back(dstd::String(entry.path));
        out->sizes.push_back(entry.size);
        out->kinds.push_back(entry.kind);
    }
}

Task list_kinds_sdk(Host h, dstd::CStringView root, FoundSdk* out) {
    dftracer::utils::plugins::Fs fs = h.fs();
    PathList listed = fs.new_list();
    co_await fs.list_dir(root, listed);
    const std::uint64_t n = listed.size();
    for (std::uint64_t i = 0; i < n; ++i) {
        const PathEntry entry = co_await listed.entry(i);
        out->paths.push_back(dstd::String(entry.path));
        out->kinds.push_back(entry.kind);
    }
}

Task openat2_sdk(Host h, dstd::CStringView path, std::uint64_t* size) {
    dftracer::utils::plugins::Io io = h.io();
    int fd = -1;
    OpenHow how;
    how.flags = O_RDONLY;
    co_await io.openat2(AT_FDCWD, path, how, &fd);
    if (fd < 0) co_return;
    dftracer::utils::plugins::Stat st;
    co_await io.fstat(fd, st);
    *size = st.size();
    co_await io.close(fd);
}

Task xattr_roundtrip_sdk(Host h, dstd::CStringView path, dstd::String* got,
                         std::int64_t* out_n) {
    dftracer::utils::plugins::Io io = h.io();
    const dstd::CStringView name("user.sdk");
    co_await io.setxattr(path, name, dstd::ByteView("view-xattr", 10), 0);
    char buf[32] = {};
    co_await io.getxattr(path, name, dstd::MutableByteView(buf, sizeof(buf)),
                         out_n);
    got->assign(buf, static_cast<std::size_t>(*out_n));
}

}  // namespace

TEST_CASE("plugin cxx: Fs stat_batch refuses a span shorter than the paths") {
    dftu_utils_test::TestEnvironment env(0);
    const dstd::String a = env.dir() + "/short_a.txt";
    const dstd::String b = env.dir() + "/short_b.txt";
    const dstd::CStringView paths[2] = {a, b};
    HostFixture fx;
    test_plugin_common::TaskDriver driver(fx.host());
    FileStat stats[2];
    StatStatus statuses[2];
    dstd::String message;
    // Two paths, two stats, one status slot: the host would write two entries
    // into a one-entry span.
    const dftu_status st = driver.run(
        stat_batch_short_sdk(driver.host(),
                             dstd::Span<const dstd::CStringView>(paths, 2),
                             dstd::Span<FileStat>(stats, 2),
                             dstd::Span<StatStatus>(statuses, 1)),
        &message);
    CHECK(st == DFTU_E_INVALID);
    CHECK(message.find("stat_batch") != dstd::String::npos);
    CHECK(message.find("2 paths") != dstd::String::npos);
    CHECK(message.find("got 2 and 1") != dstd::String::npos);
}

TEST_CASE("plugin cxx: Fs find_all takes patterns and skip_dirs") {
    dftu_utils_test::TestEnvironment env(0);
    const dstd::String root = env.dir() + "/sdk_find";
    dstd::fs::create_directories(dstd::PathView(root + "/sub")).discard();
    dstd::fs::create_directories(dstd::PathView(root + "/skipme")).discard();
    for (const dstd::String& f :
         {root + "/top.txt", root + "/keep.log", root + "/sub/a.txt",
          root + "/sub/b.log", root + "/skipme/deep.txt"}) {
        dstd::fs::write_file(dstd::PathView(f), dstd::String("x")).discard();
    }

    const dstd::CStringView patterns[2] = {"*.txt", "*/*.txt"};
    const dstd::CStringView skip[1] = {"skipme"};
    HostFixture fx;
    FoundSdk found;
    drive_task(fx.host(), [&](Host h) {
        return find_patterns_sdk(
            h, root, dstd::Span<const dstd::CStringView>(patterns, 2),
            dstd::Span<const dstd::CStringView>(skip, 1), &found);
    });

    REQUIRE(found.paths.size() == 2);
    bool saw_top = false, saw_sub = false;
    for (std::size_t i = 0; i < found.paths.size(); ++i) {
        CHECK(found.kinds[i] == FileKind::File);
        CHECK(found.paths[i].find("skipme") == dstd::String::npos);
        CHECK(found.paths[i].find(".log") == dstd::String::npos);
        if (found.paths[i] == root + "/top.txt") {
            saw_top = true;
            CHECK(found.sizes[i] == 1);
        }
        if (found.paths[i] == root + "/sub/a.txt") saw_sub = true;
    }
    CHECK(saw_top);
    CHECK(saw_sub);
}

TEST_CASE("plugin cxx: Fs list_dir reports the kind of a file and a dir") {
    dftu_utils_test::TestEnvironment env(0);
    const dstd::String root = env.dir() + "/sdk_kinds";
    dstd::fs::create_directories(dstd::PathView(root + "/sub")).discard();
    dstd::fs::write_file(dstd::PathView(root + "/top.txt"), dstd::String("xy"))
        .discard();

    HostFixture fx;
    FoundSdk listed;
    drive_task(fx.host(),
               [&](Host h) { return list_kinds_sdk(h, root, &listed); });

    REQUIRE(listed.paths.size() == 2);
    bool saw_file = false, saw_dir = false;
    for (std::size_t i = 0; i < listed.paths.size(); ++i) {
        if (listed.paths[i] == root + "/top.txt") {
            saw_file = true;
            CHECK(listed.kinds[i] == FileKind::File);
        }
        if (listed.paths[i] == root + "/sub") {
            saw_dir = true;
            CHECK(listed.kinds[i] == FileKind::Directory);
        }
    }
    CHECK(saw_file);
    CHECK(saw_dir);
}

TEST_CASE("plugin cxx: Io openat2 opens through an OpenHow") {
    dftu_utils_test::TestEnvironment env(0);
    const dstd::String path = env.dir() + "/sdk_openat2.bin";
    dstd::fs::write_file(dstd::PathView(path), dstd::String("how")).discard();

    HostFixture fx;
    std::uint64_t size = 0;
    dstd::String message;
    const dftu_status st = drive_status(
        fx.host(), [&](Host h) { return openat2_sdk(h, path, &size); },
        &message);
#ifdef __linux__
    CHECK(st == DFTU_OK);
    CHECK(size == 3);
#else
    CHECK(st == DFTU_E_UNSUPPORTED);
    MESSAGE("openat2 needs Linux: " << message);
#endif
}

TEST_CASE("plugin cxx: Io xattr round-trips through the byte views") {
    dftu_utils_test::TestEnvironment env(0);
    const dstd::String path = env.dir() + "/sdk_xattr.bin";
    dstd::fs::write_file(dstd::PathView(path), dstd::String("xattr")).discard();

    HostFixture fx;
    dstd::String got;
    std::int64_t out_n = 0;
    dstd::String message;
    const dftu_status st = drive_status(
        fx.host(),
        [&](Host h) { return xattr_roundtrip_sdk(h, path, &got, &out_n); },
        &message);
    if (st == DFTU_E_UNSUPPORTED ||
        message.find("not supported") != dstd::String::npos) {
        MESSAGE("no xattr on this file system: " << message);
        return;
    }
    CHECK(st == DFTU_OK);
    CHECK(out_n == 10);
    CHECK(got == "view-xattr");
}

TEST_CASE("plugin cxx: Fileio helpers answer and fail on a missing file") {
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
