// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstdint>
#include <cstdlib>
#include <utility>

using namespace dftracer::utils;

namespace {

dstd::Path in(const dftu_utils_test::ScopedTestDir& dir, const char* name) {
    return dstd::Path(dir.file(name));
}

dstd::OpenOptions options(dstd::Access access, dstd::Creation creation) {
    dstd::OpenOptions o;
    o.access = access;
    o.creation = creation;
    return o;
}

}  // namespace

TEST_CASE("bytes written at an offset read back at that offset") {
    dftu_utils_test::ScopedTestDir dir("dstd_file_roundtrip");
    auto f = dstd::File::open(
        in(dir, "a.bin"),
        options(dstd::Access::ReadWrite, dstd::Creation::AlwaysNew));
    REQUIRE(f);
    REQUIRE(f->write_all_at(0, dstd::ByteView("hello world", 11)));
    REQUIRE(f->write_all_at(6, dstd::ByteView("there", 5)));
    CHECK(f->size().value() == 11);

    dstd::Vector<std::uint8_t> out(5);
    REQUIRE(f->read_exact_at(6, out));
    CHECK(dstd::String(out.begin(), out.end()) == "there");
    REQUIRE(f->read_exact_at(0, out));
    CHECK(dstd::String(out.begin(), out.end()) == "hello");
}

TEST_CASE("a read at the end returns 0 and an exact read fails") {
    dftu_utils_test::ScopedTestDir dir("dstd_file_eof");
    auto f = dstd::File::open(
        in(dir, "a.bin"),
        options(dstd::Access::ReadWrite, dstd::Creation::AlwaysNew));
    REQUIRE(f);
    REQUIRE(f->write_all_at(0, dstd::ByteView("abc", 3)));

    dstd::Vector<std::uint8_t> out(8);
    auto n = f->read_at(1, out);
    REQUIRE(n);
    CHECK(*n == 2);
    CHECK(f->read_at(3, out).value() == 0);
    auto exact = f->read_exact_at(0, out);
    REQUIRE_FALSE(exact);
    CHECK(exact.error().is(dstd::Condition::Io));
}

TEST_CASE("a write past the end fills the gap with zeros") {
    dftu_utils_test::ScopedTestDir dir("dstd_file_gap");
    auto f = dstd::File::open(
        in(dir, "a.bin"),
        options(dstd::Access::ReadWrite, dstd::Creation::AlwaysNew));
    REQUIRE(f);
    REQUIRE(f->write_all_at(4, dstd::ByteView("x", 1)));
    dstd::Vector<std::uint8_t> out(5);
    REQUIRE(f->read_exact_at(0, out));
    CHECK(out == dstd::Vector<std::uint8_t>{0, 0, 0, 0, 'x'});

    REQUIRE(f->resize(2));
    CHECK(f->size().value() == 2);
    REQUIRE(f->resize(3));
    REQUIRE(f->read_exact_at(0, dstd::Span<std::uint8_t>(out.data(), 3)));
    CHECK(out[2] == 0);
    REQUIRE(f->sync(dstd::SyncMode::Data));
    REQUIRE(f->sync(dstd::SyncMode::All));
}

TEST_CASE("creation rules decide whether open succeeds") {
    dftu_utils_test::ScopedTestDir dir("dstd_file_creation");
    const auto path = in(dir, "a.bin");

    auto missing = dstd::File::open(
        path, options(dstd::Access::Read, dstd::Creation::OpenExisting));
    REQUIRE_FALSE(missing);
    CHECK(missing.error().is(dstd::Condition::NotFound));
    auto truncate_missing = dstd::File::open(
        path, options(dstd::Access::Write, dstd::Creation::TruncateExisting));
    REQUIRE_FALSE(truncate_missing);
    CHECK(truncate_missing.error().is(dstd::Condition::NotFound));

    auto created = dstd::File::open(
        path, options(dstd::Access::Write, dstd::Creation::OnlyIfNew));
    REQUIRE(created);
    REQUIRE(created->write_all_at(0, dstd::ByteView("abc", 3)));
    REQUIRE(created->close());

    CHECK_FALSE(dstd::File::open(
        path, options(dstd::Access::Write, dstd::Creation::OnlyIfNew)));
    auto kept = dstd::File::open(
        path, options(dstd::Access::Write, dstd::Creation::IfNeeded));
    REQUIRE(kept);
    CHECK(kept->size().value() == 3);
    auto cut = dstd::File::open(
        path, options(dstd::Access::Write, dstd::Creation::TruncateExisting));
    REQUIRE(cut);
    CHECK(cut->size().value() == 0);
}

TEST_CASE("read access cannot create or truncate") {
    dftu_utils_test::ScopedTestDir dir("dstd_file_read_create");
    auto f = dstd::File::open(
        in(dir, "a.bin"),
        options(dstd::Access::Read, dstd::Creation::AlwaysNew));
    REQUIRE_FALSE(f);
    CHECK(f.error().is(dstd::Condition::InvalidArgument));
    CHECK_FALSE(dstd::fs::exists(in(dir, "a.bin")).value());
}

TEST_CASE("close is idempotent and a moved-from file is closed") {
    dftu_utils_test::ScopedTestDir dir("dstd_file_close");
    auto f = dstd::File::open(
        in(dir, "a.bin"),
        options(dstd::Access::Write, dstd::Creation::AlwaysNew));
    REQUIRE(f);
    dstd::File moved = std::move(*f);
    CHECK_FALSE(f->is_open());
    CHECK(moved.is_open());
    REQUIRE(moved.close());
    CHECK_FALSE(moved.is_open());
    CHECK(moved.close());
}

TEST_CASE("append writes at the end whatever the other handles do") {
    dftu_utils_test::ScopedTestDir dir("dstd_file_append");
    const auto path = in(dir, "log.bin");
    auto a = dstd::File::open(
        path, options(dstd::Access::Append, dstd::Creation::IfNeeded));
    auto b = dstd::File::open(
        path, options(dstd::Access::Append, dstd::Creation::IfNeeded));
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->append(dstd::ByteView("one,", 4)).value() == 4);
    CHECK(b->append(dstd::ByteView("two,", 4)).value() == 4);
    CHECK(a->append(dstd::ByteView("three", 5)).value() == 5);

    auto r = dstd::File::open(path, {});
    REQUIRE(r);
    dstd::Vector<std::uint8_t> out(13);
    REQUIRE(r->read_exact_at(0, out));
    CHECK(dstd::String(out.begin(), out.end()) == "one,two,three");
}

TEST_CASE("a vectored read fills the buffers in order") {
    dftu_utils_test::ScopedTestDir dir("dstd_file_vectored");
    auto f = dstd::File::open(
        in(dir, "a.bin"),
        options(dstd::Access::ReadWrite, dstd::Creation::AlwaysNew));
    REQUIRE(f);
    REQUIRE(f->write_all_at(0, dstd::ByteView("abcdefghij", 10)));
    dstd::Vector<std::uint8_t> first(3);
    dstd::Vector<std::uint8_t> second(4);
    const dstd::Span<std::uint8_t> parts[] = {first, second};
    CHECK(f->read_vectored_at(2, parts).value() == 7);
    CHECK(dstd::String(first.begin(), first.end()) == "cde");
    CHECK(dstd::String(second.begin(), second.end()) == "fghi");
    CHECK(f->read_vectored_at(8, parts).value() == 2);
    CHECK(dstd::String(first.begin(), first.begin() + 2) == "ij");
}

TEST_CASE("stat of an open file matches the path stat") {
    dftu_utils_test::ScopedTestDir dir("dstd_file_stat");
    const auto path = in(dir, "a.bin");
    auto f = dstd::File::open(
        path, options(dstd::Access::Write, dstd::Creation::AlwaysNew));
    REQUIRE(f);
    REQUIRE(f->write_all_at(0, dstd::ByteView("12345", 5)));
    REQUIRE(f->advise(dstd::Advice::Sequential));
    const auto by_file = f->stat();
    const auto by_path = dstd::fs::stat(path);
    REQUIRE(by_file);
    REQUIRE(by_path);
    CHECK(by_file->size == 5);
    CHECK(by_file->kind == dstd::fs::FileKind::File);
    CHECK(by_file->ino == by_path->ino);
    CHECK(by_file->mtime_ns == by_path->mtime_ns);
}

TEST_CASE("one open file holds the lock until it unlocks") {
    dftu_utils_test::ScopedTestDir dir("dstd_file_lock");
    const auto path = in(dir, ".lock");
    auto a = dstd::File::open(
        path, options(dstd::Access::ReadWrite, dstd::Creation::IfNeeded));
    auto b = dstd::File::open(
        path, options(dstd::Access::ReadWrite, dstd::Creation::IfNeeded));
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->try_lock().value());
    CHECK_FALSE(b->try_lock().value());
    REQUIRE(a->unlock());
    CHECK(b->try_lock().value());
    REQUIRE(b->close());
    CHECK(a->try_lock().value());
}

TEST_CASE("bypass caching opens or reports Unsupported") {
    dftu_utils_test::ScopedTestDir dir("dstd_file_bypass");
    const auto path = in(dir, "a.bin");
    {
        auto w = dstd::File::open(
            path, options(dstd::Access::Write, dstd::Creation::AlwaysNew));
        REQUIRE(w);
        dstd::Vector<std::uint8_t> block(8192, std::uint8_t{7});
        REQUIRE(w->write_all_at(0, block));
    }
    const std::uint32_t align = dstd::File::direct_alignment(path);
    dstd::OpenOptions o;
    o.caching = dstd::Caching::Bypass;
    auto f = dstd::File::open(path, o);
    if (!f) {
        CHECK(f.error().is(dstd::Condition::Unsupported));
        MESSAGE("direct I/O refused here: ", f.error().message());
        return;
    }
    REQUIRE(align != 0);
    REQUIRE(align <= 4096);
    void* raw = nullptr;
    REQUIRE(::posix_memalign(&raw, align, 4096) == 0);
    auto* buf = static_cast<std::uint8_t*>(raw);
    auto n = f->read_at(4096, dstd::Span<std::uint8_t>(buf, 4096));
    CHECK(n.value() == 4096);
    CHECK(buf[0] == 7);
    CHECK(buf[4095] == 7);
    std::free(raw);
}
