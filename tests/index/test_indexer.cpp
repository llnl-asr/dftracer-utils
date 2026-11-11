// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/reader/trace_reader.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <generator_ok.h>
#include <index_test_helpers.h>
#include <instrumentation.h>
#include <sys/resource.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <iterator>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::index;

namespace {

constexpr dstd::OpenOptions WRITE_EXISTING{dstd::Access::Write,
                                           dstd::Creation::OpenExisting};

IndexerOptions with_runtime(Runtime& rt) {
    IndexerOptions o;
    o.runtime = &rt;
    return o;
}

dstd::String index_of(dstd::StringView trace) {
    return trace::internal::determine_index_path(dstd::String(trace), "");
}

dstd::Vector<dstd::String> names_of(
    std::initializer_list<dstd::StringView> names) {
    dstd::Vector<dstd::String> out;
    for (const auto& name : names) out.emplace_back(name);
    return out;
}

dstd::Vector<dstd::String> sorted(dstd::Vector<dstd::String> v) {
    std::sort(v.begin(), v.end());
    return v;
}

void touch_later(dstd::PathView path) {
    constexpr std::uint64_t TEN_SECONDS_NS = 10'000'000'000ULL;
    REQUIRE(dstd::fs::set_mtime_ns(
                path, dstd::fs::mtime_ns(path).value() + TEN_SECONDS_NS)
                .has_value());
}

}  // namespace

TEST_SUITE("Indexer") {
    TEST_CASE("open expands a directory to its trace files") {
        Runtime rt(2);
        dftu_utils_test::TestEnvironment env(10);
        const dstd::Path dir = dstd::Path(env.dir()) / "traces";
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        dstd::fs::write_file(dstd::PathView(dir / "a.pfw.gz"), "x").discard();
        dstd::fs::write_file(dstd::PathView(dir / "b.ndjson.gz"), "x")
            .discard();
        dstd::fs::write_file(dstd::PathView(dir / "plain.pfw"), "x").discard();
        dstd::fs::write_file(dstd::PathView(dir / "notes.txt"), "x").discard();

        auto ix = dftu_utils_test::ok(
            Indexer::open({dstd::String(dir.str())}, with_runtime(rt)));
        dstd::Vector<dstd::String> names;
        for (const auto& p : ix.paths())
            names.emplace_back(dstd::PathView(p).filename());
        CHECK(sorted(names) == names_of({"a.pfw.gz", "b.ndjson.gz"}));
        rt.shutdown();
    }

    TEST_CASE("open rejects an empty list and a missing path") {
        dftu_utils_test::expect_error(Indexer::open({}),
                                      common::ErrorCode::InvalidArgument);
        const dstd::String missing = "/no/such/dir/trace.pfw.gz";
        const auto absent =
            Indexer::open({dstd::String(dstd::StringView(missing))});
        dftu_utils_test::expect_error(absent, common::ErrorCode::NotFound);
        CHECK(dstd::String(absent.error().message()).find(missing) !=
              dstd::String::npos);
    }

    TEST_CASE("open and status write nothing") {
        Runtime rt(2);
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = env.create_dft_test_gzip_file(20);
        auto ix = dftu_utils_test::ok(Indexer::open(
            {dstd::String(dstd::StringView(trace))}, with_runtime(rt)));
        auto st = dftu_utils_test::ok(ix.status());
        CHECK(st.total == 1);
        CHECK(st.ready.empty());
        CHECK(st.needs_work == names_of({trace}));
        CHECK(st.indexed == 0);
        CHECK_FALSE(dstd::fs::exists(index_of(trace)).value());
        rt.shutdown();
    }

    TEST_CASE("a root holding only caches is no index") {
        Runtime rt(2);
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = env.create_dft_test_gzip_file(30);
        CHECK(dftu_utils_test::ok(
                  dftu_utils_test::ok(
                      Indexer::open({dstd::String(dstd::StringView(trace))},
                                    with_runtime(rt)))
                      .build())
                  .indexed == 1);

        // A cache survives where the store does not, so the root stays.
        const dstd::Path root(index_of(trace));
        REQUIRE(dstd::fs::create_directories(root / "rollups" / "store")
                    .has_value());
        dstd::fs::write_file(dstd::PathView(root / "rollups" / "marker"), "r")
            .discard();
        dstd::fs::remove_all(root / "store").discard();
        REQUIRE(dstd::fs::exists(root).value());
        REQUIRE_FALSE(dstd::fs::exists(root / "store").value());

        auto ix = dftu_utils_test::ok(Indexer::open(
            {dstd::String(dstd::StringView(trace))}, with_runtime(rt)));
        CHECK(dftu_utils_test::ok(ix.status()).needs_work == names_of({trace}));
        CHECK(dftu_utils_test::ok(ix.build()).indexed == 1);
        CHECK(dstd::fs::exists(root / "store").value());
        CHECK(dstd::fs::exists(root / "rollups" / "marker").value());
        rt.shutdown();
    }

    TEST_CASE("build indexes only what is missing or stale") {
        Runtime rt(4);
        dftu_utils_test::TestEnvironment env(10);
        const auto a = env.create_dft_test_gzip_file(30);
        const auto b = env.create_dft_test_gzip_file(40);
        auto ix = dftu_utils_test::ok(
            Indexer::open({dstd::String(dstd::StringView(a)),
                           dstd::String(dstd::StringView(b))},
                          with_runtime(rt)));

        auto first = dftu_utils_test::ok(ix.build());
        CHECK(first.indexed == 2);
        CHECK(first.needs_work.empty());
        CHECK(sorted(first.ready) == sorted(names_of({a, b})));
        CHECK(dstd::fs::exists(first.index_path).value());

        auto second = dftu_utils_test::ok(ix.build());
        CHECK(second.indexed == 0);
        CHECK(sorted(second.ready) == sorted(names_of({a, b})));

        const auto replacement = env.create_dft_test_gzip_file(55);
        REQUIRE(
            dstd::fs::copy_file(replacement, a, dstd::fs::CopyMode::Overwrite)
                .has_value());
        touch_later(a);
        auto third = dftu_utils_test::ok(ix.build());
        CHECK(third.indexed == 1);
        CHECK(third.needs_work.empty());

        auto files = dftu_utils_test::ok(ix.files());
        REQUIRE(files.size() == 2);
        for (const auto& f : files) {
            CHECK(f.size_bytes == dstd::fs::file_size(f.path).value());
            CHECK(f.size_bytes > 0);
            CHECK(f.index_path == index_of(f.path));
        }
        rt.shutdown();
    }

    TEST_CASE("status stats the files when it runs, not when open ran") {
        Runtime rt(4);
        dftu_utils_test::TestEnvironment env(10);
        const auto a = env.create_dft_test_gzip_file(30);
        CHECK(dftu_utils_test::ok(
                  dftu_utils_test::ok(
                      Indexer::open({dstd::String(dstd::StringView(a))},
                                    with_runtime(rt)))
                      .build())
                  .indexed == 1);

        auto ix = dftu_utils_test::ok(Indexer::open(
            {dstd::String(dstd::StringView(a))}, with_runtime(rt)));
        const auto replacement = env.create_dft_test_gzip_file(55);
        REQUIRE(
            dstd::fs::copy_file(replacement, a, dstd::fs::CopyMode::Overwrite)
                .has_value());
        touch_later(a);
        CHECK(dftu_utils_test::ok(ix.status()).needs_work == names_of({a}));
        CHECK(dftu_utils_test::ok(ix.build()).indexed == 1);
        CHECK(dftu_utils_test::ok(ix.status()).needs_work.empty());
        rt.shutdown();
    }

    TEST_CASE("a missing bloom tier is added") {
        Runtime rt(4);
        dftu_utils_test::TestEnvironment env(10);
        const auto a = env.create_dft_test_gzip_file(30);
        auto no_bloom = with_runtime(rt);
        no_bloom.bloom.reset();
        CHECK(dftu_utils_test::ok(
                  dftu_utils_test::ok(
                      Indexer::open({dstd::String(dstd::StringView(a))},
                                    no_bloom))
                      .build())
                  .indexed == 1);

        auto ix = dftu_utils_test::ok(Indexer::open(
            {dstd::String(dstd::StringView(a))}, with_runtime(rt)));
        CHECK(dftu_utils_test::ok(ix.status()).needs_work == names_of({a}));
        auto st = dftu_utils_test::ok(ix.build());
        CHECK(st.indexed == 1);
        CHECK(st.needs_work.empty());
        rt.shutdown();
    }

    TEST_CASE("a changed bloom setting rebuilds the tier") {
        Runtime rt(4);
        dftu_utils_test::TestEnvironment env(10);
        const auto a = env.create_dft_test_gzip_file(30);
        auto first = with_runtime(rt);
        first.bloom->fields = {"size"};
        CHECK(dftu_utils_test::ok(
                  dftu_utils_test::ok(
                      Indexer::open({dstd::String(dstd::StringView(a))}, first))
                      .build())
                  .indexed == 1);

        auto fewer = with_runtime(rt);
        fewer.bloom->fields = {};
        CHECK(dftu_utils_test::ok(
                  dftu_utils_test::ok(
                      Indexer::open({dstd::String(dstd::StringView(a))}, fewer))
                      .status())
                  .needs_work.empty());

        auto other_rate = first;
        other_rate.bloom->false_positive_rate = 0.05;
        auto ix = dftu_utils_test::ok(
            Indexer::open({dstd::String(dstd::StringView(a))}, other_rate));
        CHECK(dftu_utils_test::ok(ix.status()).needs_work == names_of({a}));
        CHECK(dftu_utils_test::ok(ix.build()).indexed == 1);
        CHECK(dftu_utils_test::ok(ix.status()).needs_work.empty());
        rt.shutdown();
    }

    TEST_CASE("rebuild re-indexes every file") {
        Runtime rt(4);
        dftu_utils_test::TestEnvironment env(10);
        const auto a = env.create_dft_test_gzip_file(30);
        const auto b = env.create_dft_test_gzip_file(30);
        auto ix = dftu_utils_test::ok(
            Indexer::open({dstd::String(dstd::StringView(a)),
                           dstd::String(dstd::StringView(b))},
                          with_runtime(rt)));
        dftu_utils_test::ok(ix.build());
        auto st = dftu_utils_test::ok(ix.rebuild());
        CHECK(st.indexed == 2);
        CHECK(sorted(st.ready) == sorted(names_of({a, b})));
        rt.shutdown();
    }

    TEST_CASE("async and blocking forms agree") {
        Runtime rt(4);
        dftu_utils_test::TestEnvironment env_a(10);
        dftu_utils_test::TestEnvironment env_b(10);
        const auto a = env_a.create_dft_test_gzip_file(25);
        const auto b = dstd::Path(env_b.dir()) / dstd::PathView(a).filename();
        REQUIRE(dstd::fs::copy_file(a, b, dstd::fs::CopyMode::FailIfExists)
                    .has_value());
        REQUIRE(dstd::fs::set_mtime_ns(b, dstd::fs::mtime_ns(a).value())
                    .has_value());

        auto blocking = dftu_utils_test::ok(Indexer::open(
            {dstd::String(dstd::StringView(a))}, with_runtime(rt)));
        const IndexStatus by_block = dftu_utils_test::ok(blocking.build());
        const auto files_block = dftu_utils_test::ok(blocking.files());

        auto async = dftu_utils_test::ok(Indexer::open(
            {dstd::String(dstd::StringView(dstd::String(b.str())))},
            with_runtime(rt)));
        IndexStatus by_async;
        dstd::Vector<IndexedFile> files_async;
        dftu_utils_test::ok(rt.block_on(coro::named(
            "indexer-test",
            coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
                by_async = dftu_utils_test::ok(co_await async.build(scope));
                files_async = dftu_utils_test::ok(co_await async.files(scope));
            }))));

        CHECK(by_async.total == by_block.total);
        CHECK(by_async.indexed == by_block.indexed);
        CHECK(by_async.ready.size() == by_block.ready.size());
        CHECK(by_async.needs_work == by_block.needs_work);
        REQUIRE(files_async.size() == files_block.size());
        CHECK(dstd::Path(files_async[0].path).view().filename() ==
              dstd::Path(files_block[0].path).view().filename());
        CHECK(files_async[0].size_bytes == files_block[0].size_bytes);
        CHECK(files_async[0].mtime == files_block[0].mtime);
        rt.shutdown();
    }

    TEST_CASE("an unreadable trace fails the build and names the file") {
        if (geteuid() == 0) return;  // root reads every file
        Runtime rt(4);
        dftu_utils_test::TestEnvironment env(10);
        const auto good = env.create_dft_test_gzip_file(30);
        const auto locked = env.create_dft_test_gzip_file(30);
        REQUIRE(dstd::fs::set_permissions(locked, 0).has_value());

        auto ix = dftu_utils_test::ok(
            Indexer::open({dstd::String(dstd::StringView(good)),
                           dstd::String(dstd::StringView(locked))},
                          with_runtime(rt)));
        const auto built = ix.build();
        REQUIRE(dstd::fs::set_permissions(locked, 0700).has_value());
        REQUIRE_FALSE(built.has_value());
        CHECK(dstd::String(built.error().message()).find(locked) !=
              dstd::String::npos);
        auto st = dftu_utils_test::ok(
            dftu_utils_test::ok(
                Indexer::open({dstd::String(dstd::StringView(good))},
                              with_runtime(rt)))
                .status());
        CHECK(st.ready == names_of({good}));
        rt.shutdown();
    }

    TEST_CASE("the indexing guide example runs") {
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = env.create_dft_test_gzip_file(20);
        using namespace dftracer::utils::trace::index;

        IndexerOptions options;
        options.bloom->fields = {"io.off"};

        Indexer indexer = dftu_utils_test::ok(
            Indexer::open({dstd::String(dstd::StringView(trace))}, options));
        IndexStatus status = dftu_utils_test::ok(indexer.build());
        CHECK(status.indexed == 1);
        CHECK(dftu_utils_test::ok(indexer.build()).indexed == 0);
    }
}

namespace {

// Independent reference: zlib decodes member after member and keeps the
// prefix of a member cut at end of file.
struct ZlibPrefix {
    std::size_t lines = 0;
    std::size_t events = 0;  // complete lines that start with '{'
    std::size_t complete_members = 0;
    dstd::Vector<std::size_t> member_starts;
};

ZlibPrefix zlib_prefix(const dstd::String& path) {
    const dstd::String data_raw =
        dstd::fs::read_file(dstd::PathView(path)).value_or(dstd::String());
    dstd::Vector<unsigned char> data(data_raw.begin(), data_raw.end());
    ZlibPrefix r;
    std::size_t off = 0;
    dstd::String out;
    dstd::Vector<unsigned char> buf(1 << 16);
    while (off < data.size()) {
        r.member_starts.push_back(off);
        z_stream zs{};
        REQUIRE(inflateInit2(&zs, 16 + MAX_WBITS) == Z_OK);
        zs.next_in = data.data() + off;
        zs.avail_in = static_cast<uInt>(data.size() - off);
        int rc = Z_OK;
        do {
            zs.next_out = buf.data();
            zs.avail_out = static_cast<uInt>(buf.size());
            rc = inflate(&zs, Z_NO_FLUSH);
            out.append(reinterpret_cast<char*>(buf.data()),
                       buf.size() - zs.avail_out);
        } while (rc == Z_OK);
        const std::size_t used = (data.size() - off) - zs.avail_in;
        inflateEnd(&zs);
        if (rc != Z_STREAM_END) break;
        ++r.complete_members;
        off += used;
    }
    r.lines =
        static_cast<std::size_t>(std::count(out.begin(), out.end(), '\n'));
    std::size_t pos = 0;
    for (std::size_t nl; (nl = out.find('\n', pos)) != dstd::String::npos;
         pos = nl + 1)
        if (out[pos] == '{') ++r.events;
    return r;
}

dstd::String write_prefix(const dstd::String& src, const dstd::String& dst,
                          double frac) {
    dstd::String bytes =
        dstd::fs::read_file(dstd::PathView(src)).value_or(dstd::String());
    dstd::fs::write_file(dstd::PathView(dst),
                         common::str_cat(bytes.substr(
                             0, static_cast<std::size_t>(
                                    static_cast<double>(bytes.size()) * frac))))
        .discard();
    return dst;
}

struct Indexed {
    std::uint64_t lines = 0;
    bool truncated = false;
    std::uint64_t members = 0;
    std::uint64_t members_end = 0;
};

Indexed indexed(const dstd::String& trace) {
    const auto snapshot = dftu_utils_test::index_snapshot(index_of(trace));
    const auto file = dftu_utils_test::require_indexed_file(snapshot, trace);
    Indexed r;
    const auto meta = dftu_utils_test::index_take(
        dftracer::utils::trace::index::file_metadata(snapshot, file));
    REQUIRE(meta.has_value());
    r.lines = meta->line_count;
    r.truncated = meta->truncated;
    auto members = dftu_utils_test::index_take(
        dftracer::utils::trace::index::members(snapshot, file));
    r.members = members.size();
    if (!members.empty())
        r.members_end = members.back().c_offset + members.back().c_size;
    return r;
}

dstd::String multimember_trace(dftu_utils_test::TestEnvironment& env,
                               int events, const dstd::String& name) {
    const auto plain = env.create_dft_test_file(events);
    const dstd::String gz((dstd::Path(env.dir()) / name).str());
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               16 * 1024));
    dstd::fs::remove(plain).discard();
    return gz;
}

}  // namespace

TEST_SUITE("Truncated") {
    TEST_CASE("a cut trace keeps the complete lines of its cut member") {
        Runtime rt(4);
        dftu_utils_test::TestEnvironment env(10);
        const auto full = multimember_trace(env, 4000, "full.pfw.gz");
        for (double frac : {0.05, 0.5, 0.9, 0.99}) {
            CAPTURE(frac);
            const auto cut = write_prefix(
                full,
                dstd::String(
                    (dstd::Path(env.dir()) /
                     ("cut" + dstd::to_string(static_cast<int>(frac * 100)) +
                      ".pfw.gz"))
                        .str()),
                frac);
            auto st = dftu_utils_test::ok(
                dftu_utils_test::ok(
                    Indexer::open({dstd::String(dstd::StringView(cut))},
                                  with_runtime(rt)))
                    .build());
            CHECK(st.ready == names_of({cut}));
            const auto got = indexed(cut);
            const auto ref = zlib_prefix(cut);
            CHECK(got.lines == ref.lines);
            CHECK(got.truncated);
            CHECK(got.members_end == dstd::fs::file_size(cut).value());
        }
        rt.shutdown();
    }

    TEST_CASE("a cut trace whose tail reads as a 4 GiB ISIZE stays small") {
        Runtime rt(4);
        dftu_utils_test::TestEnvironment env(10);
        const auto full = multimember_trace(env, 4000, "full.pfw.gz");
        const auto cut = write_prefix(
            full, dstd::String((dstd::Path(env.dir()) / "cut.pfw.gz").str()),
            0.5);
        {
            auto f = dstd::File::open(cut, WRITE_EXISTING);
            REQUIRE(f);
            REQUIRE(f->write_all_at(f->size().value() - 4,
                                    dstd::ByteView("\xfe\xff\xff\xff", 4)));
        }
        dftu_utils_test::ok(
            dftu_utils_test::ok(
                Indexer::open({dstd::String(dstd::StringView(cut))},
                              with_runtime(rt)))
                .build());
        rusage ru{};
        getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
        const auto peak = static_cast<std::uint64_t>(ru.ru_maxrss);
#else
        const auto peak = static_cast<std::uint64_t>(ru.ru_maxrss) * 1024;
#endif
        if (!instrumentation::active()) CHECK(peak < (std::uint64_t{1} << 30));
        const auto got = indexed(cut);
        CHECK(got.lines == zlib_prefix(cut).lines);
        CHECK(got.truncated);
        rt.shutdown();
    }

    TEST_CASE("a cut single-member trace is not empty") {
        Runtime rt(2);
        dftu_utils_test::TestEnvironment env(10);
        const auto full = env.create_dft_test_gzip_file(3000);
        const auto cut = write_prefix(
            full, dstd::String((dstd::Path(env.dir()) / "cut.pfw.gz").str()),
            0.5);
        dftu_utils_test::ok(
            dftu_utils_test::ok(
                Indexer::open({dstd::String(dstd::StringView(cut))},
                              with_runtime(rt)))
                .build());
        const auto got = indexed(cut);
        CHECK(got.lines == zlib_prefix(cut).lines);
        CHECK(got.lines > 0);
        CHECK(got.truncated);
        rt.shutdown();
    }

    TEST_CASE("a complete trace is unchanged") {
        Runtime rt(4);
        dftu_utils_test::TestEnvironment env(10);
        const auto full = multimember_trace(env, 4000, "full.pfw.gz");
        dftu_utils_test::ok(
            dftu_utils_test::ok(
                Indexer::open({dstd::String(dstd::StringView(full))},
                              with_runtime(rt)))
                .build());
        const auto got = indexed(full);
        const auto ref = zlib_prefix(full);
        CHECK(got.lines == ref.lines);
        CHECK(got.members == ref.complete_members);
        CHECK_FALSE(got.truncated);
        rt.shutdown();
    }

    TEST_CASE("a corrupt last member is neither recovered nor reported") {
        Runtime rt(4);
        dftu_utils_test::TestEnvironment env(10);
        const auto full = multimember_trace(env, 4000, "full.pfw.gz");
        const auto ref = zlib_prefix(full);
        REQUIRE(ref.member_starts.size() > 1);
        const std::size_t last = ref.member_starts.back();
        {
            auto f = dstd::File::open(full, WRITE_EXISTING);
            REQUIRE(f);
            const std::size_t n = f->size().value() - last - 10;
            dstd::String noise(n, static_cast<char>(0xFF));
            REQUIRE(
                f->write_all_at(last + 10, dstd::ByteView(noise.data(), n)));
        }
        dftu_utils_test::ok(
            dftu_utils_test::ok(
                Indexer::open({dstd::String(dstd::StringView(full))},
                              with_runtime(rt)))
                .build());
        const auto got = indexed(full);
        CHECK(got.lines == zlib_prefix(full).lines);
        CHECK(got.members == ref.complete_members - 1);
        CHECK_FALSE(got.truncated);
        rt.shutdown();
    }

    TEST_CASE("a cut trace completed later is re-indexed and cleared") {
        Runtime rt(4);
        dftu_utils_test::TestEnvironment env(10);
        const auto full = multimember_trace(env, 4000, "full.pfw.gz");
        const dstd::String path((dstd::Path(env.dir()) / "live.pfw.gz").str());
        write_prefix(full, path, 0.6);
        auto ix = dftu_utils_test::ok(Indexer::open(
            {dstd::String(dstd::StringView(path))}, with_runtime(rt)));
        dftu_utils_test::ok(ix.build());
        REQUIRE(indexed(path).truncated);
        REQUIRE(dstd::fs::copy_file(full, path, dstd::fs::CopyMode::Overwrite)
                    .has_value());
        touch_later(path);
        CHECK(dftu_utils_test::ok(ix.build()).indexed == 1);
        const auto got = indexed(path);
        CHECK_FALSE(got.truncated);
        CHECK(got.lines == zlib_prefix(full).lines);
        rt.shutdown();
    }

    TEST_CASE("readers return the recovered lines of a cut trace") {
        Runtime rt(4);
        dftu_utils_test::TestEnvironment env(10);
        const auto full = multimember_trace(env, 4000, "full.pfw.gz");
        const auto cut = write_prefix(
            full, dstd::String((dstd::Path(env.dir()) / "cut90.pfw.gz").str()),
            0.9);
        const auto ref = zlib_prefix(cut);
        REQUIRE(ref.events > 0);

        const auto count_lines = [](const dstd::String& path)
            -> dftracer::utils::coro::CoroTask<std::size_t> {
            dftracer::utils::trace::reader::TraceReader reader(
                {.file_path = dstd::String(path)});
            auto gen = dftu_utils_test::ok_items(reader.read_lines());
            std::size_t n = 0;
            while (auto line = co_await gen.next()) ++n;
            co_return n;
        };
        CHECK(dftu_utils_test::ok(count_lines(cut).get()) ==
              ref.lines);  // sequential, no index

        dftu_utils_test::ok(
            dftu_utils_test::ok(
                Indexer::open({dstd::String(dstd::StringView(cut))},
                              with_runtime(rt)))
                .build());
        CHECK(dftu_utils_test::ok(count_lines(cut).get()) ==
              ref.lines);  // indexed

        auto df = dftu_utils_test::ok(
            dftracer::utils::trace::views::View::from_file(dstd::String(cut))
                .collect()
                .get());
        CHECK(static_cast<std::size_t>(df.num_rows()) == ref.events);
        rt.shutdown();
    }

    TEST_CASE("status and files report the cut trace") {
        Runtime rt(4);
        dftu_utils_test::TestEnvironment env(10);
        const auto full = multimember_trace(env, 4000, "full.pfw.gz");
        const auto cut = write_prefix(
            full, dstd::String((dstd::Path(env.dir()) / "cut.pfw.gz").str()),
            0.7);
        auto ix = dftu_utils_test::ok(
            Indexer::open({dstd::String(dstd::StringView(full)),
                           dstd::String(dstd::StringView(cut))},
                          with_runtime(rt)));
        dftu_utils_test::ok(ix.build());
        auto st = dftu_utils_test::ok(ix.status());
        CHECK(sorted(st.ready) == sorted(names_of({full, cut})));
        CHECK(st.truncated == names_of({cut}));
        for (const auto& f : dftu_utils_test::ok(ix.files()))
            CHECK(f.truncated ==
                  (dstd::StringView(f.path) == dstd::StringView(cut)));
        rt.shutdown();
    }
}
