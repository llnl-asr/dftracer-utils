// SPDX-License-Identifier: MIT
/* A real C consumer of the dftu_io ABI. Each call returns a dftu_task; since
 * C has no coroutines, the C++ harness co_awaits them in sequence (the fd from
 * open flows into write/pread/close). This proves even the async I/O vtable is
 * C-callable. */
#include <arpa/inet.h>
#include <dftracer/utils/plugins/abi.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <plugins/abi_c_consumers.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static const dftu_io* io_of(const dftu_plugin_host* h) {
    return (const dftu_io*)h->get_service(h->h, DFTU_SVC_IO);
}

dftu_status dftu_test_io_open(dftu_ctx* ctx, const dftu_plugin_host* h,
                              const char* path, int* fd, dftu_task** t) {
    const dftu_io* io = io_of(h);
    if (!io) {
        *fd = -1;
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no io service");
    }
    return io->open(ctx, h->h, path, O_CREAT | O_RDWR | O_TRUNC, 0644, fd, t);
}

/* Opens read-only, so a missing path fails the task with DFTU_E_NOT_FOUND. */
dftu_status dftu_test_io_open_existing(dftu_ctx* ctx, const dftu_plugin_host* h,
                                       const char* path, int* fd,
                                       dftu_task** t) {
    return io_of(h)->open(ctx, h->h, path, O_RDONLY, 0, fd, t);
}

dftu_status dftu_test_io_write(dftu_ctx* ctx, const dftu_plugin_host* h, int fd,
                               const void* buf, uint64_t len, int64_t* n,
                               dftu_task** t) {
    return io_of(h)->write(ctx, h->h, fd, buf, len, n, t);
}

dftu_status dftu_test_io_pread(dftu_ctx* ctx, const dftu_plugin_host* h, int fd,
                               void* buf, uint64_t len, uint64_t off,
                               int64_t* n, dftu_task** t) {
    return io_of(h)->pread(ctx, h->h, fd, buf, len, off, n, t);
}

dftu_status dftu_test_io_close(dftu_ctx* ctx, const dftu_plugin_host* h, int fd,
                               dftu_task** t) {
    return io_of(h)->close(ctx, h->h, fd, t);
}

/* The scenarios below start an op, ask the harness to run its task and check
 * the status and the message. A call that is refused up front (no task) is
 * checked the same way. */
#define MSG_CAP 256

typedef struct {
    dftu_ctx* ctx;
    const dftu_plugin_host* host;
    const dftu_io* io;
    const dftu_test_harness* hn;
    char msg[MSG_CAP];
} scn;

static dftu_status finish(scn* s, dftu_status st, dftu_task* t) {
    s->msg[0] = 0;
    if (st < 0) return st;
    return s->hn->run(t, s->msg, MSG_CAP);
}

static int expect_status(scn* s, const char* what, dftu_status got,
                         dftu_status want, const char* in_message) {
    s->hn->expect(what, got == want);
    if (in_message && got == want && got != DFTU_OK)
        s->hn->expect(what, strstr(s->msg, in_message) != NULL);
    return got == want;
}

void dftu_test_io_fs_ops(dftu_ctx* ctx, const dftu_plugin_host* h,
                         const dftu_test_harness* hn, const char* dir) {
    scn sc = {ctx, h, io_of(h), hn, {0}};
    scn* s = &sc;
    const dftu_io* io = s->io;
    char a[512], b[512], c[512], l[512], missing[512];
    dftu_task* t = NULL;
    snprintf(a, sizeof a, "%s/fs_a", dir);
    snprintf(b, sizeof b, "%s/fs_b", dir);
    snprintf(c, sizeof c, "%s/fs_c", dir);
    snprintf(l, sizeof l, "%s/fs_link", dir);
    snprintf(missing, sizeof missing, "%s/fs_missing", dir);
    struct stat st;

    expect_status(s, "mkdir", finish(s, io->mkdir(ctx, h->h, a, 0755, &t), t),
                  DFTU_OK, NULL);
    hn->expect("mkdir made a directory",
               stat(a, &st) == 0 && S_ISDIR(st.st_mode));
    expect_status(s, "mkdir twice fails",
                  finish(s, io->mkdir(ctx, h->h, a, 0755, &t), t), DFTU_E_IO,
                  "mkdir");

    expect_status(s, "rename", finish(s, io->rename(ctx, h->h, a, b, &t), t),
                  DFTU_OK, NULL);
    hn->expect("rename moved it", stat(a, &st) != 0 && stat(b, &st) == 0);
    expect_status(s, "rename of a missing path",
                  finish(s, io->rename(ctx, h->h, missing, c, &t), t),
                  DFTU_E_NOT_FOUND, "rename");

    expect_status(s, "symlinkat",
                  finish(s, io->symlinkat(ctx, h->h, b, AT_FDCWD, l, &t), t),
                  DFTU_OK, NULL);
    hn->expect("symlinkat made a link",
               lstat(l, &st) == 0 && S_ISLNK(st.st_mode));
    expect_status(s, "symlinkat over an existing path",
                  finish(s, io->symlinkat(ctx, h->h, b, AT_FDCWD, l, &t), t),
                  DFTU_E_IO, "symlinkat");

    expect_status(s, "unlink of the link",
                  finish(s, io->unlink(ctx, h->h, l, &t), t), DFTU_OK, NULL);
    expect_status(s, "unlink of a missing path",
                  finish(s, io->unlink(ctx, h->h, missing, &t), t),
                  DFTU_E_NOT_FOUND, "unlink");

    int fd = open(c, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    hn->expect("setup file", fd >= 0);
    if (fd >= 0) close(fd);
    snprintf(l, sizeof l, "%s/fs_hard", dir);
    expect_status(
        s, "linkat",
        finish(s, io->linkat(ctx, h->h, AT_FDCWD, c, AT_FDCWD, l, 0, &t), t),
        DFTU_OK, NULL);
    hn->expect("linkat made a second name",
               stat(l, &st) == 0 && st.st_nlink == 2);
    expect_status(
        s, "linkat of a missing path",
        finish(s, io->linkat(ctx, h->h, AT_FDCWD, missing, AT_FDCWD, a, 0, &t),
               t),
        DFTU_E_NOT_FOUND, "linkat");
    unlink(l);

    fd = open(c, O_RDWR);
    expect_status(s, "fadvise",
                  finish(s, io->fadvise(ctx, h->h, fd, 0, 0, 0, &t), t),
                  DFTU_OK, NULL);
#ifdef __linux__
    expect_status(s, "fadvise on a bad descriptor",
                  finish(s, io->fadvise(ctx, h->h, -1, 0, 0, 0, &t), t),
                  DFTU_E_INVALID, "fadvise");
#endif
    if (fd >= 0) close(fd);
    unlink(c);

    expect_status(s, "rmdir", finish(s, io->rmdir(ctx, h->h, b, &t), t),
                  DFTU_OK, NULL);
    expect_status(s, "rmdir of a missing path",
                  finish(s, io->rmdir(ctx, h->h, b, &t), t), DFTU_E_NOT_FOUND,
                  "rmdir");
    expect_status(s, "unlink with a NULL path", io->unlink(ctx, h->h, NULL, &t),
                  DFTU_E_INVALID, NULL);
}

void dftu_test_io_socket_ops(dftu_ctx* ctx, const dftu_plugin_host* h,
                             const dftu_test_harness* hn) {
    scn sc = {ctx, h, io_of(h), hn, {0}};
    scn* s = &sc;
    const dftu_io* io = s->io;
    dftu_task* t = NULL;
    int32_t srv = -1, cli = -1, conn = -1;

    expect_status(
        s, "socket",
        finish(s, io->socket(ctx, h->h, AF_INET, SOCK_STREAM, 0, &srv, &t), t),
        DFTU_OK, NULL);
    expect_status(
        s, "socket",
        finish(s, io->socket(ctx, h->h, AF_INET, SOCK_STREAM, 0, &cli, &t), t),
        DFTU_OK, NULL);
    expect_status(
        s, "socket with a bad domain",
        finish(s, io->socket(ctx, h->h, 9999, SOCK_STREAM, 0, &conn, &t), t),
        DFTU_E_IO, "socket");
    if (srv < 0 || cli < 0) return;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (!expect_status(s, "bind",
                       finish(s,
                              io->bind(ctx, h->h, srv, (struct sockaddr*)&addr,
                                       sizeof addr, &t),
                              t),
                       DFTU_OK, NULL))
        return;
    expect_status(s, "bind on a bad descriptor",
                  finish(s,
                         io->bind(ctx, h->h, -1, (struct sockaddr*)&addr,
                                  sizeof addr, &t),
                         t),
                  DFTU_E_INVALID, "bind");
    if (!expect_status(s, "listen",
                       finish(s, io->listen(ctx, h->h, srv, 4, &t), t), DFTU_OK,
                       NULL))
        return;
    expect_status(s, "listen on a bad descriptor",
                  finish(s, io->listen(ctx, h->h, -1, 4, &t), t),
                  DFTU_E_INVALID, "listen");

    socklen_t len = sizeof addr;
    hn->expect("getsockname",
               getsockname(srv, (struct sockaddr*)&addr, &len) == 0);
    if (!expect_status(
            s, "connect",
            finish(s,
                   io->connect(ctx, h->h, cli, (struct sockaddr*)&addr,
                               sizeof addr, &t),
                   t),
            DFTU_OK, NULL))
        return;
    expect_status(s, "connect on a bad descriptor",
                  finish(s,
                         io->connect(ctx, h->h, -1, (struct sockaddr*)&addr,
                                     sizeof addr, &t),
                         t),
                  DFTU_E_INVALID, "connect");
    expect_status(s, "connect with a NULL address",
                  io->connect(ctx, h->h, cli, NULL, 0, &t), DFTU_E_INVALID,
                  NULL);
    expect_status(
        s, "accept",
        finish(s, io->accept(ctx, h->h, srv, NULL, NULL, &conn, &t), t),
        DFTU_OK, NULL);
    if (conn < 0) return;

    char out_buf[] = "ping";
    struct iovec out_iov = {out_buf, 4};
    struct msghdr out_msg;
    memset(&out_msg, 0, sizeof out_msg);
    out_msg.msg_iov = &out_iov;
    out_msg.msg_iovlen = 1;
    int64_t sent = 0;
    expect_status(
        s, "sendmsg",
        finish(s, io->sendmsg(ctx, h->h, cli, &out_msg, 0, &sent, &t), t),
        DFTU_OK, NULL);
    hn->expect("sendmsg sent four bytes", sent == 4);
    expect_status(
        s, "sendmsg on a bad descriptor",
        finish(s, io->sendmsg(ctx, h->h, -1, &out_msg, 0, &sent, &t), t),
        DFTU_E_INVALID, "sendmsg");

    uint32_t happened = 0;
    expect_status(
        s, "poll_add",
        finish(s, io->poll_add(ctx, h->h, conn, POLLIN, &happened, &t), t),
        DFTU_OK, NULL);
    hn->expect("poll_add saw POLLIN", (happened & POLLIN) != 0);

    char in_buf[8] = {0};
    struct iovec in_iov = {in_buf, sizeof in_buf};
    struct msghdr in_msg;
    memset(&in_msg, 0, sizeof in_msg);
    in_msg.msg_iov = &in_iov;
    in_msg.msg_iovlen = 1;
    int64_t got = 0;
    expect_status(
        s, "recvmsg",
        finish(s, io->recvmsg(ctx, h->h, conn, &in_msg, 0, &got, &t), t),
        DFTU_OK, NULL);
    hn->expect("recvmsg got the message",
               got == 4 && memcmp(in_buf, "ping", 4) == 0);
    expect_status(
        s, "recvmsg on a bad descriptor",
        finish(s, io->recvmsg(ctx, h->h, -1, &in_msg, 0, &got, &t), t),
        DFTU_E_INVALID, "recvmsg");

    expect_status(s, "shutdown",
                  finish(s, io->shutdown(ctx, h->h, cli, SHUT_WR, &t), t),
                  DFTU_OK, NULL);
    expect_status(s, "shutdown on a bad descriptor",
                  finish(s, io->shutdown(ctx, h->h, -1, SHUT_WR, &t), t),
                  DFTU_E_INVALID, "shutdown");
    expect_status(
        s, "poll_add on a bad descriptor",
        finish(s, io->poll_add(ctx, h->h, -1, POLLIN, &happened, &t), t),
        DFTU_E_INVALID, "poll_add");

    close(conn);
    close(cli);
    close(srv);
}

/* Each Linux-only op: UNSUPPORTED with no task elsewhere, a real result on
 * Linux. `dir` is a directory the test owns. */
void dftu_test_io_linux_ops(dftu_ctx* ctx, const dftu_plugin_host* h,
                            const dftu_test_harness* hn, const char* dir) {
    scn sc = {ctx, h, io_of(h), hn, {0}};
    scn* s = &sc;
    const dftu_io* io = s->io;
    dftu_task* t = NULL;
    char path[512], missing[512];
    snprintf(path, sizeof path, "%s/linux_file", dir);
    snprintf(missing, sizeof missing, "%s/linux_missing", dir);
    int32_t fds[2] = {-1, -1};
    int64_t n = 0;
    dftu_statx sx;
    memset(&sx, 0, sizeof sx);
    dftu_open_how how = {O_RDONLY, 0, 0};
    int32_t fd = -1;
    char value[8] = {0};

#ifndef __linux__
    expect_status(s, "statx",
                  io->statx(ctx, h->h, AT_FDCWD, path, 0, 0xfff, &sx, &t),
                  DFTU_E_UNSUPPORTED, NULL);
    hn->ctx_message(ctx, sc.msg, MSG_CAP);
    hn->expect("statx message",
               strstr(sc.msg, "statx: not supported on this platform") != NULL);
    expect_status(s, "fallocate", io->fallocate(ctx, h->h, 0, 0, 0, 1, &t),
                  DFTU_E_UNSUPPORTED, NULL);
    expect_status(s, "sync_file_range",
                  io->sync_file_range(ctx, h->h, 0, 0, 1, 0, &t),
                  DFTU_E_UNSUPPORTED, NULL);
    expect_status(s, "splice",
                  io->splice(ctx, h->h, 0, NULL, 1, NULL, 1, 0, &n, &t),
                  DFTU_E_UNSUPPORTED, NULL);
    expect_status(s, "openat2",
                  io->openat2(ctx, h->h, AT_FDCWD, path, &how, &fd, &t),
                  DFTU_E_UNSUPPORTED, NULL);
    expect_status(s, "getxattr",
                  io->getxattr(ctx, h->h, path, "user.a", value, 8, &n, &t),
                  DFTU_E_UNSUPPORTED, NULL);
    expect_status(s, "setxattr",
                  io->setxattr(ctx, h->h, path, "user.a", "v", 1, 0, &t),
                  DFTU_E_UNSUPPORTED, NULL);
    expect_status(s, "fgetxattr",
                  io->fgetxattr(ctx, h->h, 0, "user.a", value, 8, &n, &t),
                  DFTU_E_UNSUPPORTED, NULL);
    expect_status(s, "fsetxattr",
                  io->fsetxattr(ctx, h->h, 0, "user.a", "v", 1, 0, &t),
                  DFTU_E_UNSUPPORTED, NULL);
    expect_status(s, "tee", io->tee(ctx, h->h, 0, 1, 1, 0, &n, &t),
                  DFTU_E_UNSUPPORTED, NULL);
    expect_status(s, "pipe", io->pipe(ctx, h->h, fds, 0, &t),
                  DFTU_E_UNSUPPORTED, NULL);
#else
    int f = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    hn->expect("setup file", f >= 0 && write(f, "0123456789", 10) == 10);

    expect_status(
        s, "statx",
        finish(s, io->statx(ctx, h->h, AT_FDCWD, path, 0, 0xfff, &sx, &t), t),
        DFTU_OK, NULL);
    hn->expect("statx size", sx.size == 10);
    expect_status(
        s, "statx of a missing path",
        finish(s, io->statx(ctx, h->h, AT_FDCWD, missing, 0, 0xfff, &sx, &t),
               t),
        DFTU_E_NOT_FOUND, "statx");

    expect_status(s, "fallocate",
                  finish(s, io->fallocate(ctx, h->h, f, 0, 0, 4096, &t), t),
                  DFTU_OK, NULL);
    expect_status(s, "fallocate on a bad descriptor",
                  finish(s, io->fallocate(ctx, h->h, -1, 0, 0, 4096, &t), t),
                  DFTU_E_INVALID, "fallocate");
    expect_status(s, "sync_file_range",
                  finish(s, io->sync_file_range(ctx, h->h, f, 0, 0, 0, &t), t),
                  DFTU_OK, NULL);
    expect_status(s, "sync_file_range on a bad descriptor",
                  finish(s, io->sync_file_range(ctx, h->h, -1, 0, 0, 0, &t), t),
                  DFTU_E_INVALID, "sync_file_range");

    expect_status(s, "pipe", finish(s, io->pipe(ctx, h->h, fds, 0, &t), t),
                  DFTU_OK, NULL);
    expect_status(s, "pipe with bad flags",
                  finish(s, io->pipe(ctx, h->h, fds, ~0, &t), t),
                  DFTU_E_INVALID, "pipe");
    int32_t fds2[2] = {-1, -1};
    expect_status(s, "second pipe",
                  finish(s, io->pipe(ctx, h->h, fds2, 0, &t), t), DFTU_OK,
                  NULL);
    if (fds[0] >= 0 && fds2[0] >= 0) {
        int64_t off = 2;
        expect_status(
            s, "splice file to pipe",
            finish(s,
                   io->splice(ctx, h->h, f, &off, fds[1], NULL, 4, 0, &n, &t),
                   t),
            DFTU_OK, NULL);
        hn->expect("splice moved four bytes and advanced the offset",
                   n == 4 && off == 6);
        expect_status(
            s, "tee",
            finish(s, io->tee(ctx, h->h, fds[0], fds2[1], 4, 0, &n, &t), t),
            DFTU_OK, NULL);
        hn->expect("tee copied four bytes", n == 4);
        char buf[8] = {0};
        hn->expect("tee left the bytes in the source",
                   read(fds[0], buf, 4) == 4 && memcmp(buf, "2345", 4) == 0);
        hn->expect("tee filled the target",
                   read(fds2[0], buf, 4) == 4 && memcmp(buf, "2345", 4) == 0);
    }
    expect_status(
        s, "splice on a bad descriptor",
        finish(s, io->splice(ctx, h->h, -1, NULL, -1, NULL, 4, 0, &n, &t), t),
        DFTU_E_INVALID, "splice");
    expect_status(s, "tee on a bad descriptor",
                  finish(s, io->tee(ctx, h->h, -1, -1, 4, 0, &n, &t), t),
                  DFTU_E_INVALID, "tee");

    expect_status(
        s, "openat2",
        finish(s, io->openat2(ctx, h->h, AT_FDCWD, path, &how, &fd, &t), t),
        DFTU_OK, NULL);
    expect_status(
        s, "openat2 of a missing path",
        finish(s, io->openat2(ctx, h->h, AT_FDCWD, missing, &how, &fd, &t), t),
        DFTU_E_NOT_FOUND, "openat2");
    if (fd >= 0) close(fd);

    /* A file system without user xattrs gives ENOTSUP, which is UNSUPPORTED. */
    dftu_status set = finish(
        s, io->setxattr(ctx, h->h, path, "user.dftu", "val", 3, 0, &t), t);
    hn->expect("setxattr", set == DFTU_OK || set == DFTU_E_UNSUPPORTED);
    if (set == DFTU_OK) {
        expect_status(
            s, "getxattr",
            finish(s,
                   io->getxattr(ctx, h->h, path, "user.dftu", value, 8, &n, &t),
                   t),
            DFTU_OK, NULL);
        hn->expect("getxattr value", n == 3 && memcmp(value, "val", 3) == 0);
        expect_status(
            s, "fsetxattr",
            finish(s, io->fsetxattr(ctx, h->h, f, "user.dftu2", "xy", 2, 0, &t),
                   t),
            DFTU_OK, NULL);
        expect_status(
            s, "fgetxattr",
            finish(s,
                   io->fgetxattr(ctx, h->h, f, "user.dftu2", value, 8, &n, &t),
                   t),
            DFTU_OK, NULL);
        hn->expect("fgetxattr value", n == 2 && memcmp(value, "xy", 2) == 0);
    }
    expect_status(
        s, "getxattr of a missing path",
        finish(s,
               io->getxattr(ctx, h->h, missing, "user.dftu", value, 8, &n, &t),
               t),
        DFTU_E_NOT_FOUND, "getxattr");
    expect_status(
        s, "setxattr of a missing path",
        finish(s, io->setxattr(ctx, h->h, missing, "user.dftu", "v", 1, 0, &t),
               t),
        DFTU_E_NOT_FOUND, "setxattr");
    expect_status(
        s, "fgetxattr on a bad descriptor",
        finish(s, io->fgetxattr(ctx, h->h, -1, "user.dftu", value, 8, &n, &t),
               t),
        DFTU_E_INVALID, "fgetxattr");
    expect_status(
        s, "fsetxattr on a bad descriptor",
        finish(s, io->fsetxattr(ctx, h->h, -1, "user.dftu", "v", 1, 0, &t), t),
        DFTU_E_INVALID, "fsetxattr");

    if (f >= 0) close(f);
    for (int i = 0; i < 2; ++i) {
        if (fds[i] >= 0) close(fds[i]);
        if (fds2[i] >= 0) close(fds2[i]);
    }
    unlink(path);
#endif
    (void)missing;
    (void)fds;
    (void)n;
    (void)sx;
    (void)how;
    (void)fd;
    (void)value;
}
