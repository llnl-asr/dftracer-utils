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
#include <utility>

using namespace dftracer::utils;
using dftu_utils_test::ScopedTestDir;

namespace {

dstd::Path in(const ScopedTestDir& dir, const char* name) {
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
    ScopedTestDir dir("dstd_file_roundtrip");
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
    ScopedTestDir dir("dstd_file_eof");
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
    ScopedTestDir dir("dstd_file_gap");
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
    ScopedTestDir dir("dstd_file_creation");
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
    ScopedTestDir dir("dstd_file_read_create");
    auto f = dstd::File::open(
        in(dir, "a.bin"),
        options(dstd::Access::Read, dstd::Creation::AlwaysNew));
    REQUIRE_FALSE(f);
    CHECK(f.error().is(dstd::Condition::InvalidArgument));
    CHECK_FALSE(dstd::fs::exists(in(dir, "a.bin")).value());
}

TEST_CASE("close is idempotent and a moved-from file is closed") {
    ScopedTestDir dir("dstd_file_close");
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
