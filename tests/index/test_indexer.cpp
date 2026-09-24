#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
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

void touch_later(const std::string& path) {
    fs::last_write_time(path,
                        fs::last_write_time(path) + std::chrono::seconds(10));
}

}  // namespace

TEST_SUITE("Indexer") {
    TEST_CASE("open expands a directory to its trace files") {
        Runtime rt(2);
        TestEnvironment env(10);
        const fs::path dir = fs::path(env.get_dir()) / "traces";
        fs::create_directories(dir);
        std::ofstream(dir / "a.pfw.gz") << "x";
        std::ofstream(dir / "b.pfw") << "x";
        std::ofstream(dir / "notes.txt") << "x";

        auto ix = Indexer::open({dir.string()}, with_runtime(rt));
        std::vector<std::string> names;
        for (const auto& p : ix.paths())
            names.push_back(fs::path(p).filename().string());
        CHECK(sorted(names) == std::vector<std::string>{"a.pfw.gz", "b.pfw"});
        rt.shutdown();
    }

    TEST_CASE("open rejects an empty list and a missing path") {
        try {
            Indexer::open({});
            FAIL("expected an error");
        } catch (const DFTUtilsException& e) {
            CHECK(e.code() == ErrorCode::INVALID_ARGUMENT);
        }
        const std::string missing = "/no/such/dir/trace.pfw.gz";
        try {
            Indexer::open({missing});
            FAIL("expected an error");
        } catch (const DFTUtilsException& e) {
            CHECK(e.code() == ErrorCode::NOT_FOUND);
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
        CHECK_FALSE(fs::exists(index_of(trace)));
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
        CHECK(fs::exists(first.index_path));

        auto second = ix.build();
        CHECK(second.indexed == 0);
        CHECK(sorted(second.ready) == sorted({a, b}));

        const auto replacement = env.create_dft_test_gzip_file(55);
        fs::copy_file(replacement, a, fs::copy_options::overwrite_existing);
        touch_later(a);
        auto third = ix.build();
        CHECK(third.indexed == 1);
        CHECK(third.needs_work.empty());

        auto files = ix.files();
        REQUIRE(files.size() == 2);
        for (const auto& f : files) {
            CHECK(f.size_bytes == fs::file_size(f.path));
            CHECK(f.file_id >= 0);
            CHECK(f.index_path == index_of(f.path));
        }
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
        const auto b = fs::path(env_b.get_dir()) / fs::path(a).filename();
        fs::copy_file(a, b);
        fs::last_write_time(b, fs::last_write_time(a));

        auto blocking = Indexer::open({a}, with_runtime(rt));
        const IndexStatus by_block = blocking.build();
        const auto files_block = blocking.files();

        auto async = Indexer::open({b.string()}, with_runtime(rt));
        IndexStatus by_async;
        std::vector<IndexedFile> files_async;
        rt.run_blocking("indexer-test",
                        [&](CoroScope& scope) -> coro::CoroTask<void> {
                            by_async = co_await async.build(scope);
                            files_async = co_await async.files(scope);
                        });

        CHECK(by_async.total == by_block.total);
        CHECK(by_async.indexed == by_block.indexed);
        CHECK(by_async.ready.size() == by_block.ready.size());
        CHECK(by_async.needs_work == by_block.needs_work);
        REQUIRE(files_async.size() == files_block.size());
        CHECK(files_async[0].file_id == files_block[0].file_id);
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
        fs::permissions(locked, fs::perms::none);

        auto ix = Indexer::open({good, locked}, with_runtime(rt));
        bool threw = false;
        try {
            ix.build();
        } catch (const DFTUtilsException& e) {
            threw = true;
            CHECK(std::string(e.what()).find(locked) != std::string::npos);
        }
        fs::permissions(locked, fs::perms::owner_all);
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
    std::ifstream in(path, std::ios::binary);
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
    std::ifstream in(src, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    std::ofstream(dst, std::ios::binary) << bytes.substr(
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
    dftracer::utils::index::store::IndexDatabase db(
        index_of(trace),
        dftracer::utils::index::store::IndexOpenMode::ReadOnly);
    const int fid = db.get_file_info_id(
        dftracer::utils::index::store::internal::get_logical_path(trace));
    REQUIRE(fid >= 0);
    Indexed r;
    auto meta = db.query_file_metadata_batch({fid});
    r.lines = meta[fid].num_lines;
    r.truncated = meta[fid].truncated;
    auto members = db.query_gzip_members(fid);
    r.members = members.size();
    if (!members.empty())
        r.members_end = members.back().c_offset + members.back().c_size;
    return r;
}

std::string multimember_trace(TestEnvironment& env, int events,
                              const std::string& name) {
    const auto plain = env.create_dft_test_file(events);
    const auto gz = (fs::path(env.get_dir()) / name).string();
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               16 * 1024));
    fs::remove(plain);
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
                (fs::path(env.get_dir()) /
                 ("cut" + std::to_string(static_cast<int>(frac * 100)) +
                  ".pfw.gz"))
                    .string(),
                frac);
            auto st = Indexer::open({cut}, with_runtime(rt)).build();
            CHECK(st.ready == std::vector<std::string>{cut});
            const auto got = indexed(cut);
            const auto ref = zlib_prefix(cut);
            CHECK(got.lines == ref.lines);
            CHECK(got.truncated);
            CHECK(got.members_end == fs::file_size(cut));
        }
        rt.shutdown();
    }

    TEST_CASE("a cut single-member trace is not empty") {
        Runtime rt(2);
        TestEnvironment env(10);
        const auto full = env.create_dft_test_gzip_file(3000);
        const auto cut = write_prefix(
            full, (fs::path(env.get_dir()) / "cut.pfw.gz").string(), 0.5);
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
            const std::size_t n = fs::file_size(full) - last - 10;
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
        const auto path = (fs::path(env.get_dir()) / "live.pfw.gz").string();
        write_prefix(full, path, 0.6);
        auto ix = Indexer::open({path}, with_runtime(rt));
        ix.build();
        REQUIRE(indexed(path).truncated);
        fs::copy_file(full, path, fs::copy_options::overwrite_existing);
        touch_later(path);
        CHECK(ix.build().indexed == 1);
        const auto got = indexed(path);
        CHECK_FALSE(got.truncated);
        CHECK(got.lines == zlib_prefix(full).lines);
        rt.shutdown();
    }

    TEST_CASE("an index in another format is rebuilt") {
        Runtime rt(4);
        TestEnvironment env(10);
        const auto trace = env.create_dft_test_gzip_file(50);
        auto ix = Indexer::open({trace}, with_runtime(rt));
        ix.build();
        dftracer::utils::index::store::RocksDBManager::instance().reset(
            index_of(trace));
        {
            dftracer::utils::index::store::RocksDatabase db(index_of(trace));
            namespace layout = dftracer::utils::index::store::layout;
            REQUIRE(db.del(layout::format_key()).ok());
        }
        CHECK(ix.status().needs_work == std::vector<std::string>{trace});
        CHECK(ix.build().indexed == 1);
        CHECK(ix.status().needs_work.empty());
        rt.shutdown();
    }

    TEST_CASE("readers return the recovered lines of a cut trace") {
        Runtime rt(4);
        TestEnvironment env(10);
        const auto full = multimember_trace(env, 4000, "full.pfw.gz");
        const auto cut = write_prefix(
            full, (fs::path(env.get_dir()) / "cut90.pfw.gz").string(), 0.9);
        const auto ref = zlib_prefix(cut);
        REQUIRE(ref.events > 0);

        const auto count_lines = [](const std::string& path)
            -> dftracer::utils::coro::CoroTask<std::size_t> {
            dftracer::utils::utilities::reader::TraceReader reader(
                {.file_path = path});
            auto gen = reader.read_lines();
            std::size_t n = 0;
            while (auto line = co_await gen.next()) ++n;
            co_return n;
        };
        CHECK(count_lines(cut).get() == ref.lines);  // sequential, no index

        Indexer::open({cut}, with_runtime(rt)).build();
        CHECK(count_lines(cut).get() == ref.lines);  // indexed

        auto df = dftracer::utils::trace::views::View::from_file(cut)
                      .metadata(false)
                      .collect()
                      .get();
        CHECK(static_cast<std::size_t>(df.num_rows()) == ref.events);
        rt.shutdown();
    }

    TEST_CASE("status and files report the cut trace") {
        Runtime rt(4);
        TestEnvironment env(10);
        const auto full = multimember_trace(env, 4000, "full.pfw.gz");
        const auto cut = write_prefix(
            full, (fs::path(env.get_dir()) / "cut.pfw.gz").string(), 0.7);
        auto ix = Indexer::open({full, cut}, with_runtime(rt));
        ix.build();
        auto st = ix.status();
        CHECK(sorted(st.ready) == sorted({full, cut}));
        CHECK(st.truncated == std::vector<std::string>{cut});
        for (const auto& f : ix.files()) CHECK(f.truncated == (f.path == cut));
        rt.shutdown();
    }
}
