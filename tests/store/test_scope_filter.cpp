#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/store/scope_filter.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

namespace dstd = dftracer::utils::dstd;
namespace store = dftracer::utils::store;

using dftracer::utils::dstd::ByteView;
using store::ScopeKind;

ByteView view_of(const dstd::Vector<std::uint8_t>& bytes) {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

std::uint64_t file_hash(const std::string& path) {
    const dstd::String held(path.data(), path.size());
    return store::scope_hash("dftu.test", 1, ScopeKind::File,
                             dstd::Span<const dstd::String>(&held, 1));
}

std::vector<std::uint64_t> hashes_of(std::size_t first, std::size_t count) {
    std::vector<std::uint64_t> hashes;
    for (std::size_t i = first; i < first + count; ++i) {
        hashes.push_back(file_hash("/traces/" + std::to_string(i) + ".pfw"));
    }
    return hashes;
}

dstd::Vector<std::uint8_t> filter_of(const std::vector<std::uint64_t>& hashes) {
    return store::build_scope_filter(
        dstd::Span<const std::uint64_t>(hashes.data(), hashes.size()));
}

}  // namespace

TEST_SUITE("index store scope filter") {
    TEST_CASE("a filter holds every scope it was built over") {
        const std::vector<std::uint64_t> held = hashes_of(0, 5000);
        const dstd::Vector<std::uint8_t> filter = filter_of(held);
        CHECK(filter.size() % 8 == 0);
        for (const std::uint64_t hash : held) {
            CHECK(store::scope_filter_may_hold(view_of(filter), hash));
        }
    }

    TEST_CASE("a filter rules out almost every scope it was not built over") {
        const dstd::Vector<std::uint8_t> filter = filter_of(hashes_of(0, 1000));
        std::size_t passed = 0;
        const std::vector<std::uint64_t> absent = hashes_of(1000000, 100000);
        for (const std::uint64_t hash : absent) {
            if (store::scope_filter_may_hold(view_of(filter), hash)) ++passed;
        }
        CHECK(passed < absent.size() / 50);
    }

    TEST_CASE("a filter stays within its most bytes") {
        const dstd::Vector<std::uint8_t> filter =
            filter_of(hashes_of(0, 100000));
        CHECK(filter.size() == store::SCOPE_FILTER_MAX_BYTES);
        CHECK(store::scope_filter_may_hold(view_of(filter),
                                           file_hash("/traces/7.pfw")));
    }

    TEST_CASE("an empty or malformed filter holds every scope") {
        CHECK(filter_of({}).empty());
        const dstd::Vector<std::uint8_t> empty;
        CHECK(store::scope_filter_may_hold(view_of(empty), file_hash("/a")));
        const dstd::Vector<std::uint8_t> odd(7, std::uint8_t{0});
        CHECK(store::scope_filter_may_hold(view_of(odd), file_hash("/a")));
    }

    TEST_CASE("every part of a directory key changes its hash") {
        const dstd::String ab("ab");
        const dstd::String c("c");
        const dstd::String a("a");
        const dstd::String bc("bc");
        const dstd::String split_one[] = {ab, c};
        const dstd::String split_two[] = {a, bc};
        const auto hash = [](const char* name, std::uint32_t version,
                             ScopeKind kind,
                             dstd::Span<const dstd::String> paths) {
            return store::scope_hash(name, version, kind, paths);
        };
        const dstd::Span<const dstd::String> one(split_one, 2);
        const std::uint64_t base = hash("ns", 1, ScopeKind::Files, one);
        CHECK(base != hash("ns", 1, ScopeKind::Files,
                           dstd::Span<const dstd::String>(split_two, 2)));
        CHECK(base != hash("ns", 2, ScopeKind::Files, one));
        CHECK(base != hash("nt", 1, ScopeKind::Files, one));
        CHECK(base != hash("ns", 1, ScopeKind::File, one));
        const dstd::StringView views[] = {dstd::StringView("ab"),
                                          dstd::StringView("c")};
        CHECK(base ==
              store::scope_hash("ns", 1, ScopeKind::Files,
                                dstd::Span<const dstd::StringView>(views, 2)));
    }
}
