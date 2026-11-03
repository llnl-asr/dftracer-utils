// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <instrumentation.h>
#include <sys/resource.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace dftracer::utils;
using dftracer::utils::index::IndexedFile;
using dftracer::utils::index::Indexer;
using dftracer::utils::index::IndexerOptions;
using dftracer::utils::index::IndexStatus;
using dftu_utils_test::TestEnvironment;

namespace {

IndexerOptions with_runtime(Runtime& rt) {
    IndexerOptions o;
    o.runtime = &rt;
    return o;
}

std::string index_of(const std::string& trace) {
    return trace::internal::determine_index_path(trace, "");
}

std::vector<std::string> sorted(std::vector<std::string> v) {
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
        TestEnvironment env(10);
        const dstd::Path dir = dstd::Path(env.get_dir()) / "traces";
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        std::ofstream((dir / "a.pfw.gz").c_str()) << "x";
        std::ofstream((dir / "b.pfw").c_str()) << "x";
        std::ofstream((dir / "notes.txt").c_str()) << "x";

        auto ix = Indexer::open({std::string(dir.str())}, with_runtime(rt));
        std::vector<std::string> names;
        for (const auto& p : ix.paths())
            names.push_back(std::string(dstd::PathView(p).filename()));
        CHECK(sorted(names) == std::vector<std::string>{"a.pfw.gz", "b.pfw"});
        rt.shutdown();
    }

    TEST_CASE("open rejects an empty list and a missing path") {
        try {
            Indexer::open({});
            FAIL("expected an error");
        } catch (const common::DFTUtilsException& e) {
            CHECK(e.error().is_code(common::ErrorCode::InvalidArgument));
        }
        const std::string missing = "/no/such/dir/trace.pfw.gz";
        try {
            Indexer::open({missing});
            FAIL("expected an error");
        } catch (const common::DFTUtilsException& e) {
            CHECK(e.error().is_code(common::ErrorCode::NotFound));
            CHECK(std::string(e.what()).find(missing) != std::string::npos);
        }
    }

    TEST_CASE("open and status write nothing") {
        Runtime rt(2);
        TestEnvironment env(10);
        const auto trace = env.create_dft_test_gzip_file(20);
        auto ix = Indexer::open({trace}, with_runtime(rt));
        auto st = ix.status();
        CHECK(st.total == 1);
        CHECK(st.ready.empty());
        CHECK(st.needs_work == std::vector<std::string>{trace});
        CHECK(st.indexed == 0);
        CHECK_FALSE(dstd::fs::exists(index_of(trace)).value());
        rt.shutdown();
    }

    TEST_CASE("a root holding only caches is no index") {
        Runtime rt(2);
        TestEnvironment env(10);
        const auto trace = env.create_dft_test_gzip_file(30);
        CHECK(Indexer::open({trace}, with_runtime(rt)).build().indexed == 1);

        // A cache survives where the store does not, so the root stays.
        const dstd::Path root(index_of(trace));
        REQUIRE(dstd::fs::create_directories(root / "rollups" / "store")
                    .has_value());
        std::ofstream((root / "rollups" / "marker").c_str()) << "r";
        (void)dstd::fs::remove_all(root / "store");
        REQUIRE(dstd::fs::exists(root).value());
        REQUIRE_FALSE(dstd::fs::exists(root / "store").value());

        auto ix = Indexer::open({trace}, with_runtime(rt));
        CHECK(ix.status().needs_work == std::vector<std::string>{trace});
        CHECK(ix.build().indexed == 1);
        CHECK(dstd::fs::exists(root / "store").value());
        CHECK(dstd::fs::exists(root / "rollups" / "marker").value());
        rt.shutdown();
    }

    TEST_CASE("build indexes only what is missing or stale") {
        Runtime rt(4);
        TestEnvironment env(10);
        const auto a = env.create_dft_test_gzip_file(30);
        const auto b = env.create_dft_test_gzip_file(40);
        auto ix = Indexer::open({a, b}, with_runtime(rt));

        auto first = ix.build();
        CHECK(first.indexed == 2);
        CHECK(first.needs_work.empty());
        CHECK(sorted(first.ready) == sorted({a, b}));
        CHECK(dstd::fs::exists(first.index_path).value());

        auto second = ix.build();
        CHECK(second.indexed == 0);
        CHECK(sorted(second.ready) == sorted({a, b}));

        const auto replacement = env.create_dft_test_gzip_file(55);
        REQUIRE(
            dstd::fs::copy_file(replacement, a, dstd::fs::CopyMode::Overwrite)
                .has_value());
        touch_later(a);
        auto third = ix.build();
        CHECK(third.indexed == 1);
        CHECK(third.needs_work.empty());

        auto files = ix.files();
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
        TestEnvironment env(10);
        const auto a = env.create_dft_test_gzip_file(30);
        CHECK(Indexer::open({a}, with_runtime(rt)).build().indexed == 1);

        auto ix = Indexer::open({a}, with_runtime(rt));
        const auto replacement = env.create_dft_test_gzip_file(55);
        REQUIRE(
            dstd::fs::copy_file(replacement, a, dstd::fs::CopyMode::Overwrite)
                .has_value());
        touch_later(a);
        CHECK(ix.status().needs_work == std::vector<std::string>{a});
        CHECK(ix.build().indexed == 1);
        CHECK(ix.status().needs_work.empty());
        rt.shutdown();
    }

    TEST_CASE("a missing bloom tier is added") {
        Runtime rt(4);
        TestEnvironment env(10);
        const auto a = env.create_dft_test_gzip_file(30);
        auto no_bloom = with_runtime(rt);
        no_bloom.bloom.reset();
        CHECK(Indexer::open({a}, no_bloom).build().indexed == 1);

        auto ix = Indexer::open({a}, with_runtime(rt));
        CHECK(ix.status().needs_work == std::vector<std::string>{a});
        auto st = ix.build();
        CHECK(st.indexed == 1);
        CHECK(st.needs_work.empty());
        rt.shutdown();
    }

    TEST_CASE("a changed bloom setting rebuilds the tier") {
        Runtime rt(4);
        TestEnvironment env(10);
        const auto a = env.create_dft_test_gzip_file(30);
        auto first = with_runtime(rt);
        first.bloom->fields = {"size"};
        CHECK(Indexer::open({a}, first).build().indexed == 1);

        auto fewer = with_runtime(rt);
        fewer.bloom->fields = {};
        CHECK(Indexer::open({a}, fewer).status().needs_work.empty());

        auto other_rate = first;
        other_rate.bloom->false_positive_rate = 0.05;
        auto ix = Indexer::open({a}, other_rate);
        CHECK(ix.status().needs_work == std::vector<std::string>{a});
        CHECK(ix.build().indexed == 1);
        CHECK(ix.status().needs_work.empty());
        rt.shutdown();
    }

    TEST_CASE("rebuild re-indexes every file") {
        Runtime rt(4);
        TestEnvironment env(10);
        const auto a = env.create_dft_test_gzip_file(30);
        const auto b = env.create_dft_test_gzip_file(30);
        auto ix = Indexer::open({a, b}, with_runtime(rt));
        ix.build();
        auto st = ix.rebuild();
        CHECK(st.indexed == 2);
        CHECK(sorted(st.ready) == sorted({a, b}));
        rt.shutdown();
    }

    TEST_CASE("async and blocking forms agree") {
        Runtime rt(4);
        TestEnvironment env_a(10);
        TestEnvironment env_b(10);
        const auto a = env_a.create_dft_test_gzip_file(25);
        const auto b =
            dstd::Path(env_b.get_dir()) / dstd::PathView(a).filename();
        REQUIRE(dstd::fs::copy_file(a, b, dstd::fs::CopyMode::FailIfExists)
                    .has_value());
        REQUIRE(dstd::fs::set_mtime_ns(b, dstd::fs::mtime_ns(a).value())
                    .has_value());

        auto blocking = Indexer::open({a}, with_runtime(rt));
        const IndexStatus by_block = blocking.build();
        const auto files_block = blocking.files();

        auto async = Indexer::open({std::string(b.str())}, with_runtime(rt));
        IndexStatus by_async;
        std::vector<IndexedFile> files_async;
        rt.block_on(coro::named(
            "indexer-test",
            coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
                by_async = co_await async.build(scope);
                files_async = co_await async.files(scope);
            })));

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
        TestEnvironment env(10);
        const auto good = env.create_dft_test_gzip_file(30);
        const auto locked = env.create_dft_test_gzip_file(30);
        REQUIRE(dstd::fs::set_permissions(locked, 0).has_value());

        auto ix = Indexer::open({good, locked}, with_runtime(rt));
        bool threw = false;
        try {
            ix.build();
        } catch (const common::DFTUtilsException& e) {
            threw = true;
            CHECK(std::string(e.what()).find(locked) != std::string::npos);
        }
        REQUIRE(dstd::fs::set_permissions(locked, 0700).has_value());
        CHECK(threw);
        auto st = Indexer::open({good}, with_runtime(rt)).status();
        CHECK(st.ready == std::vector<std::string>{good});
        rt.shutdown();
    }

    TEST_CASE("the indexing guide example runs") {
        TestEnvironment env(10);
        const auto trace = env.create_dft_test_gzip_file(20);
        using namespace dftracer::utils::index;

        IndexerOptions options;
        options.bloom->fields = {"io.off"};

        Indexer indexer = Indexer::open({trace}, options);
        IndexStatus status = indexer.build();
        CHECK(status.indexed == 1);
        CHECK(indexer.build().indexed == 0);
    }
}

namespace {

// Independent reference: zlib decodes member after member and keeps the
// prefix of a member cut at end of file.
struct ZlibPrefix {
    std::size_t lines = 0;
    std::size_t events = 0;  // complete lines that start with '{'
    std::size_t complete_members = 0;
    std::vector<std::size_t> member_starts;
};

ZlibPrefix zlib_prefix(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    ZlibPrefix r;
    std::size_t off = 0;
    std::string out;
    std::vector<unsigned char> buf(1 << 16);
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
    for (std::size_t nl; (nl = out.find('\n', pos)) != std::string::npos;
         pos = nl + 1)
        if (out[pos] == '{') ++r.events;
    return r;
}

std::string write_prefix(const std::string& src, const std::string& dst,
                         double frac) {
    std::ifstream in(src.c_str(), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    std::ofstream(dst.c_str(), std::ios::binary) << bytes.substr(
        0, static_cast<std::size_t>(static_cast<double>(bytes.size()) * frac));
    return dst;
}

struct Indexed {
    std::uint64_t lines = 0;
    bool truncated = false;
    std::uint64_t members = 0;
    std::uint64_t members_end = 0;
};

Indexed indexed(const std::string& trace) {
    const auto snapshot = dftu_utils_test::index_snapshot(index_of(trace));
    const auto file = dftu_utils_test::require_indexed_file(snapshot, trace);
    Indexed r;
    const auto meta = dftu_utils_test::index_take(
        dftracer::utils::index::file_metadata(snapshot, file));
    REQUIRE(meta.has_value());
    r.lines = meta->num_lines;
    r.truncated = meta->truncated;
    auto members = dftu_utils_test::index_take(
        dftracer::utils::index::members(snapshot, file));
    r.members = members.size();
    if (!members.empty())
        r.members_end = members.back().c_offset + members.back().c_size;
    return r;
}

std::string multimember_trace(TestEnvironment& env, int events,
                              const std::string& name) {
    const auto plain = env.create_dft_test_file(events);
    const std::string gz((dstd::Path(env.get_dir()) / name).str());
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               16 * 1024));
    (void)dstd::fs::remove(plain);
    return gz;
}

}  // namespace

TEST_SUITE("Truncated") {
    TEST_CASE("a cut trace keeps the complete lines of its cut member") {
        Runtime rt(4);
        TestEnvironment env(10);
        const auto full = multimember_trace(env, 4000, "full.pfw.gz");
        for (double frac : {0.05, 0.5, 0.9, 0.99}) {
            CAPTURE(frac);
            const auto cut = write_prefix(
                full,
                std::string(
                    (dstd::Path(env.get_dir()) /
                     ("cut" + std::to_string(static_cast<int>(frac * 100)) +
                      ".pfw.gz"))
                        .str()),
                frac);
            auto st = Indexer::open({cut}, with_runtime(rt)).build();
            CHECK(st.ready == std::vector<std::string>{cut});
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
        TestEnvironment env(10);
        const auto full = multimember_trace(env, 4000, "full.pfw.gz");
        const auto cut = write_prefix(
            full, std::string((dstd::Path(env.get_dir()) / "cut.pfw.gz").str()),
            0.5);
        {
            std::fstream f(cut,
                           std::ios::in | std::ios::out | std::ios::binary);
            f.seekp(-4, std::ios::end);
            f.write("\xfe\xff\xff\xff", 4);
        }
        Indexer::open({cut}, with_runtime(rt)).build();
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
        TestEnvironment env(10);
        const auto full = env.create_dft_test_gzip_file(3000);
        const auto cut = write_prefix(
            full, std::string((dstd::Path(env.get_dir()) / "cut.pfw.gz").str()),
            0.5);
        Indexer::open({cut}, with_runtime(rt)).build();
        const auto got = indexed(cut);
        CHECK(got.lines == zlib_prefix(cut).lines);
        CHECK(got.lines > 0);
        CHECK(got.truncated);
        rt.shutdown();
    }

    TEST_CASE("a complete trace is unchanged") {
        Runtime rt(4);
        TestEnvironment env(10);
        const auto full = multimember_trace(env, 4000, "full.pfw.gz");
        Indexer::open({full}, with_runtime(rt)).build();
        const auto got = indexed(full);
        const auto ref = zlib_prefix(full);
        CHECK(got.lines == ref.lines);
        CHECK(got.members == ref.complete_members);
        CHECK_FALSE(got.truncated);
        rt.shutdown();
    }

    TEST_CASE("a corrupt last member is neither recovered nor reported") {
        Runtime rt(4);
        TestEnvironment env(10);
        const auto full = multimember_trace(env, 4000, "full.pfw.gz");
        const auto ref = zlib_prefix(full);
        REQUIRE(ref.member_starts.size() > 1);
        const std::size_t last = ref.member_starts.back();
        {
            std::fstream f(full,
                           std::ios::in | std::ios::out | std::ios::binary);
            f.seekp(static_cast<std::streamoff>(last + 10));
            const std::size_t n = dstd::fs::file_size(full).value() - last - 10;
            std::string noise(n, static_cast<char>(0xFF));
            f.write(noise.data(), static_cast<std::streamsize>(n));
        }
        Indexer::open({full}, with_runtime(rt)).build();
        const auto got = indexed(full);
        CHECK(got.lines == zlib_prefix(full).lines);
        CHECK(got.members == ref.complete_members - 1);
        CHECK_FALSE(got.truncated);
        rt.shutdown();
    }

    TEST_CASE("a cut trace completed later is re-indexed and cleared") {
        Runtime rt(4);
        TestEnvironment env(10);
        const auto full = multimember_trace(env, 4000, "full.pfw.gz");
        const std::string path(
            (dstd::Path(env.get_dir()) / "live.pfw.gz").str());
        write_prefix(full, path, 0.6);
        auto ix = Indexer::open({path}, with_runtime(rt));
        ix.build();
        REQUIRE(indexed(path).truncated);
        REQUIRE(dstd::fs::copy_file(full, path, dstd::fs::CopyMode::Overwrite)
                    .has_value());
        touch_later(path);
        CHECK(ix.build().indexed == 1);
        const auto got = indexed(path);
        CHECK_FALSE(got.truncated);
        CHECK(got.lines == zlib_prefix(full).lines);
        rt.shutdown();
    }

    TEST_CASE("readers return the recovered lines of a cut trace") {
        Runtime rt(4);
        TestEnvironment env(10);
        const auto full = multimember_trace(env, 4000, "full.pfw.gz");
        const auto cut = write_prefix(
            full,
            std::string((dstd::Path(env.get_dir()) / "cut90.pfw.gz").str()),
            0.9);
        const auto ref = zlib_prefix(cut);
        REQUIRE(ref.events > 0);

        const auto count_lines = [](const std::string& path)
            -> dftracer::utils::coro::CoroTask<std::size_t> {
            dftracer::utils::utilities::reader::TraceReader reader(
                {.file_path = dstd::String(path)});
            auto gen = reader.read_lines();
            std::size_t n = 0;
            while (auto line = co_await gen.next()) ++n;
            co_return n;
        };
        CHECK(count_lines(cut).get() == ref.lines);  // sequential, no index

        Indexer::open({cut}, with_runtime(rt)).build();
        CHECK(count_lines(cut).get() == ref.lines);  // indexed

        auto df =
            dftracer::utils::trace::views::View::from_file(cut).collect().get();
        CHECK(static_cast<std::size_t>(df.num_rows()) == ref.events);
        rt.shutdown();
    }

    TEST_CASE("status and files report the cut trace") {
        Runtime rt(4);
        TestEnvironment env(10);
        const auto full = multimember_trace(env, 4000, "full.pfw.gz");
        const auto cut = write_prefix(
            full, std::string((dstd::Path(env.get_dir()) / "cut.pfw.gz").str()),
            0.7);
        auto ix = Indexer::open({full, cut}, with_runtime(rt));
        ix.build();
        auto st = ix.status();
        CHECK(sorted(st.ready) == sorted({full, cut}));
        CHECK(st.truncated == std::vector<std::string>{cut});
        for (const auto& f : ix.files()) CHECK(f.truncated == (f.path == cut));
        rt.shutdown();
    }
}
