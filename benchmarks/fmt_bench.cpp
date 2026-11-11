// SPDX-License-Identifier: MIT
// Benchmark: dstd::fmt against snprintf, str_cat and a "a" + s + "b" chain.
//
// Every format is taken from an error message or a line in the tree. Each is
// built five ways with the same arguments: dstd::fmt<S> (form B), dstd::fmt
// with a "..."_f value (form C), snprintf into a stack buffer, str_cat, and a
// chain of operator+ on dstd::String. The table has instructions retired per
// call and ns per call (best of the rounds). The chain and str_cat floats use
// to_string, which prints six decimals, so only the instruction cost of that
// row compares. The second table counts allocations per call: fmt holds one
// block (the result, counted through the live blocks of the memory manager
// while 256 results are held); the chain and the appends are run on std::string
// and counted at the global operator new, since the memory manager keeps no
// running call count.
//
//   fmt_bench            (table)
//   fmt_bench --smoke    (few iterations; the five outputs must agree)

#define DFTRACER_UTILS_BENCH_COUNT_NEW
#include <bench_counters.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/manager.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using namespace dftracer::utils;

namespace {

using Clock = std::chrono::steady_clock;

template <class T>
inline void keep(T& v) {
    asm volatile("" : "+r"(v) : : "memory");
}
inline void sink(const dstd::String& s) {
    const char* p = s.data();
    asm volatile("" : : "r"(p) : "memory");
}

struct Metric {
    double instructions = 0;
    double ns = 0;
};

constexpr int ROUNDS = 5;

template <class F>
Metric measure(std::size_t iterations, F&& f) {
    for (std::size_t i = 0; i < iterations / 10 + 1; ++i) f();
    Metric best;
    best.ns = 1e300;
    for (int r = 0; r < ROUNDS; ++r) {
        const std::uint64_t i0 = dftu::bench::instructions_retired();
        const auto t0 = Clock::now();
        for (std::size_t i = 0; i < iterations; ++i) f();
        const auto t1 = Clock::now();
        const std::uint64_t i1 = dftu::bench::instructions_retired();
        const double ns =
            std::chrono::duration<double, std::nano>(t1 - t0).count() /
            static_cast<double>(iterations);
        best.ns = std::min(best.ns, ns);
        best.instructions =
            static_cast<double>(i1 - i0) / static_cast<double>(iterations);
    }
    return best;
}

struct Row {
    const char* name;
    Metric fmt_b, fmt_c, snprintf_m, cat, chain;
    double fmt_blocks, chain_new, cat_new;
};

struct Inputs {
    dstd::String path_a = "/p/lustre1/traces/app-0042/rank-0007.pfw.gz";
    dstd::String path_b = "/usr/WS2/index/app-0042/rank-0007.idx";
    dstd::String reason = "No space left on device";
    dstd::String name = "DFTRACER_UTILS_STORE_EXTENT_BYTES";
    dstd::String raw = "12x";
    std::int64_t offset = 9876543210;
    std::size_t bytes = 65536;
    std::uint64_t id = 0xdeadbeefcafe1234ULL;
    std::int64_t ts = 1699999999123456;
    std::int64_t dur = 1523;
    double seconds = 12.3456789;
    long limit_ms = 30000;
    long elapsed_ms = 30011;
};

Inputs make_inputs() {
    Inputs in;
    keep(in.offset);
    keep(in.bytes);
    keep(in.id);
    keep(in.ts);
    keep(in.dur);
    keep(in.seconds);
    keep(in.limit_ms);
    keep(in.elapsed_ms);
    return in;
}

dstd::Vector<std::string> g_mismatch;

void same(const char* what, const dstd::String& a, const dstd::String& b) {
    if (a != b) {
        g_mismatch.push_back(std::string(what) + ": '" +
                             std::string(a.data(), a.size()) + "' vs '" +
                             std::string(b.data(), b.size()) + "'");
    }
}

template <class MakeFmt, class MakeSnprintf, class MakeCat, class MakeChain,
          class CountChain, class CountCat>
Row run_row(const char* name, std::size_t iterations, MakeFmt&& make_fmt,
            MakeSnprintf&& make_snprintf, MakeCat&& make_cat,
            MakeChain&& make_chain, CountChain&& count_chain,
            CountCat&& count_cat, bool compare_text) {
    Row row{};
    row.name = name;
    const auto form_b = [&] { sink(make_fmt(std::true_type{})); };
    const auto form_c = [&] { sink(make_fmt(std::false_type{})); };
    row.fmt_b = measure(iterations, form_b);
    row.fmt_c = measure(iterations, form_c);
    row.snprintf_m = measure(iterations, [&] { sink(make_snprintf()); });
    row.cat = measure(iterations, [&] { sink(make_cat()); });
    row.chain = measure(iterations, [&] { sink(make_chain()); });
    if (compare_text) {
        const dstd::String b = make_fmt(std::true_type{});
        same(name, b, make_fmt(std::false_type{}));
        same(name, b, make_snprintf());
        same(name, b, make_cat());
        same(name, b, make_chain());
    } else {
        same(name, make_fmt(std::true_type{}), make_fmt(std::false_type{}));
        same(name, make_fmt(std::true_type{}), make_snprintf());
    }

    constexpr std::size_t HELD = 256;
    dstd::Vector<dstd::String> held;
    held.reserve(HELD);
    const std::size_t before = memory::detail::Manager::get().stats().blocks;
    for (std::size_t i = 0; i < HELD; ++i)
        held.push_back(make_fmt(std::true_type{}));
    const std::size_t after = memory::detail::Manager::get().stats().blocks;
    row.fmt_blocks =
        static_cast<double>(after - before) / static_cast<double>(HELD);

    const std::uint64_t n0 = dftu::bench::allocations();
    for (std::size_t i = 0; i < HELD; ++i) count_chain();
    row.chain_new = static_cast<double>(dftu::bench::allocations() - n0) /
                    static_cast<double>(HELD);
    const std::uint64_t c0 = dftu::bench::allocations();
    for (std::size_t i = 0; i < HELD; ++i) count_cat();
    row.cat_new = static_cast<double>(dftu::bench::allocations() - c0) /
                  static_cast<double>(HELD);
    return row;
}

template <class F>
dstd::String via_snprintf(F&& f) {
    char buf[512];
    const int n = f(buf, sizeof(buf));
    return dstd::String(buf, static_cast<std::size_t>(n));
}

void print(const dstd::Vector<Row>& rows) {
    std::printf("%-26s | %-13s | %-13s | %-13s | %-13s | %-13s\n",
                "format (instr, ns)", "fmt<S>", "fmt(_f)", "snprintf",
                "str_cat", "chain");
    for (const Row& r : rows) {
        std::printf(
            "%-26s | %5.0f %6.1f | %5.0f %6.1f | %5.0f %6.1f | %5.0f %6.1f | "
            "%5.0f %6.1f\n",
            r.name, r.fmt_b.instructions, r.fmt_b.ns, r.fmt_c.instructions,
            r.fmt_c.ns, r.snprintf_m.instructions, r.snprintf_m.ns,
            r.cat.instructions, r.cat.ns, r.chain.instructions, r.chain.ns);
    }
    std::printf("\n%-26s | %-12s | %-14s | %-14s\n", "allocations per call",
                "fmt<S>", "chain", "str_cat");
    for (const Row& r : rows)
        std::printf("%-26s | %12.2f | %14.2f | %14.2f\n", r.name, r.fmt_blocks,
                    r.chain_new, r.cat_new);
    std::printf(
        "\nfmt<S>: live memory-manager blocks per result. chain, str_cat: "
        "operator new calls per call on std::string.\n");
}

}  // namespace

int main(int argc, char** argv) {
    dftu::bench::init_counters();
    const bool smoke = argc > 1 && std::strcmp(argv[1], "--smoke") == 0;
    const std::size_t iterations = smoke ? 200 : 200000;
    const Inputs in = make_inputs();
    dstd::Vector<Row> rows;

    {
        const dstd::String& a = in.path_a;
        const dstd::String& b = in.path_b;
        const dstd::String& why = in.reason;
        rows.push_back(run_row(
            "1 string", iterations,
            [&](auto isb) {
                if constexpr (decltype(isb)::value)
                    return dstd::fmt<
                        "spill: write failed ({}); the spill "
                        "directory may be full">(why);
                else
                    return dstd::fmt(
                        "spill: write failed ({}); the spill "
                        "directory may be full"_f,
                        why);
            },
            [&] {
                return via_snprintf([&](char* buf, std::size_t n) {
                    return std::snprintf(
                        buf, n,
                        "spill: write failed (%.*s); the spill directory may "
                        "be full",
                        static_cast<int>(why.size()), why.data());
                });
            },
            [&] {
                return common::str_cat("spill: write failed (", why,
                                       "); the spill directory may be full");
            },
            [&] {
                return dstd::String("spill: write failed (") + why +
                       "); the spill directory may be full";
            },
            [&] {
                std::string w(why.data(), why.size());
                std::string r = std::string("spill: write failed (") + w +
                                "); the spill directory may be full";
                asm volatile("" : : "r"(r.data()) : "memory");
            },
            [&] {
                std::string r;
                r.append("spill: write failed (");
                r.append(why.data(), why.size());
                r.append("); the spill directory may be full");
                asm volatile("" : : "r"(r.data()) : "memory");
            },
            true));

        rows.push_back(run_row(
            "3 strings", iterations,
            [&](auto isb) {
                if constexpr (decltype(isb)::value)
                    return dstd::fmt<"failed to publish {} to {}: {}">(a, b,
                                                                       why);
                else
                    return dstd::fmt("failed to publish {} to {}: {}"_f, a, b,
                                     why);
            },
            [&] {
                return via_snprintf([&](char* buf, std::size_t n) {
                    return std::snprintf(
                        buf, n, "failed to publish %.*s to %.*s: %.*s",
                        static_cast<int>(a.size()), a.data(),
                        static_cast<int>(b.size()), b.data(),
                        static_cast<int>(why.size()), why.data());
                });
            },
            [&] {
                return common::str_cat("failed to publish ", a, " to ", b, ": ",
                                       why);
            },
            [&] {
                return dstd::String("failed to publish ") + a + " to " + b +
                       ": " + why;
            },
            [&] {
                std::string r = std::string("failed to publish ") +
                                std::string(a.data(), a.size()) + " to " +
                                std::string(b.data(), b.size()) + ": " +
                                std::string(why.data(), why.size());
                asm volatile("" : : "r"(r.data()) : "memory");
            },
            [&] {
                std::string r;
                r.append("failed to publish ");
                r.append(a.data(), a.size());
                r.append(" to ");
                r.append(b.data(), b.size());
                r.append(": ");
                r.append(why.data(), why.size());
                asm volatile("" : : "r"(r.data()) : "memory");
            },
            true));
    }

    {
        const dstd::String& why = in.reason;
        const std::size_t n = in.bytes;
        const std::int64_t off = in.offset;
        rows.push_back(run_row(
            "2 ints + string", iterations,
            [&](auto isb) {
                if constexpr (decltype(isb)::value)
                    return dstd::fmt<
                        "spill: read of {} bytes at offset {} failed ({})">(
                        n, off, why);
                else
                    return dstd::fmt(
                        "spill: read of {} bytes at offset {} failed ({})"_f, n,
                        off, why);
            },
            [&] {
                return via_snprintf([&](char* buf, std::size_t cap) {
                    return std::snprintf(
                        buf, cap,
                        "spill: read of %zu bytes at offset %lld failed (%.*s)",
                        n, static_cast<long long>(off),
                        static_cast<int>(why.size()), why.data());
                });
            },
            [&] {
                return common::str_cat("spill: read of ", n,
                                       " bytes at offset ", off, " failed (",
                                       why, ")");
            },
            [&] {
                return dstd::String("spill: read of ") + dstd::to_string(n) +
                       " bytes at offset " + dstd::to_string(off) +
                       " failed (" + why + ")";
            },
            [&] {
                std::string r = std::string("spill: read of ") +
                                std::to_string(n) + " bytes at offset " +
                                std::to_string(off) + " failed (" +
                                std::string(why.data(), why.size()) + ")";
                asm volatile("" : : "r"(r.data()) : "memory");
            },
            [&] {
                std::string r;
                r.append("spill: read of ");
                r.append(std::to_string(n));
                r.append(" bytes at offset ");
                r.append(std::to_string(off));
                r.append(" failed (");
                r.append(why.data(), why.size());
                r.append(")");
                asm volatile("" : : "r"(r.data()) : "memory");
            },
            true));
    }

    {
        const long el = in.elapsed_ms;
        const long lim = in.limit_ms;
        rows.push_back(run_row(
            "2 ints", iterations,
            [&](auto isb) {
                if constexpr (decltype(isb)::value)
                    return dstd::fmt<
                        "Global timeout after {} ms (limit: {} "
                        "ms)">(el, lim);
                else
                    return dstd::fmt(
                        "Global timeout after {} ms (limit: {} ms)"_f, el, lim);
            },
            [&] {
                return via_snprintf([&](char* buf, std::size_t cap) {
                    return std::snprintf(
                        buf, cap, "Global timeout after %ld ms (limit: %ld ms)",
                        el, lim);
                });
            },
            [&] {
                return common::str_cat("Global timeout after ", el,
                                       " ms (limit: ", lim, " ms)");
            },
            [&] {
                return "Global timeout after " + dstd::to_string(el) +
                       " ms (limit: " + dstd::to_string(lim) + " ms)";
            },
            [&] {
                std::string r = "Global timeout after " + std::to_string(el) +
                                " ms (limit: " + std::to_string(lim) + " ms)";
                asm volatile("" : : "r"(r.data()) : "memory");
            },
            [&] {
                std::string r;
                r.append("Global timeout after ");
                r.append(std::to_string(el));
                r.append(" ms (limit: ");
                r.append(std::to_string(lim));
                r.append(" ms)");
                asm volatile("" : : "r"(r.data()) : "memory");
            },
            true));
    }

    {
        const std::uint64_t id = in.id;
        const dstd::String& a = in.path_a;
        rows.push_back(run_row(
            "hex id + string", iterations,
            [&](auto isb) {
                if constexpr (decltype(isb)::value)
                    return dstd::fmt<"chunk {:016x} of {}">(id, a);
                else
                    return dstd::fmt("chunk {:016x} of {}"_f, id, a);
            },
            [&] {
                return via_snprintf([&](char* buf, std::size_t cap) {
                    return std::snprintf(buf, cap, "chunk %016llx of %.*s",
                                         static_cast<unsigned long long>(id),
                                         static_cast<int>(a.size()), a.data());
                });
            },
            [&] {
                char hex[24];
                const auto r = dstd::to_chars(hex, hex + sizeof(hex), id, 16);
                const std::size_t len = static_cast<std::size_t>(r.ptr - hex);
                return common::str_cat("chunk ", dstd::String(16 - len, '0'),
                                       dstd::StringView(hex, len), " of ", a);
            },
            [&] {
                char hex[24];
                const auto r = dstd::to_chars(hex, hex + sizeof(hex), id, 16);
                const std::size_t len = static_cast<std::size_t>(r.ptr - hex);
                return dstd::String("chunk ") + dstd::String(16 - len, '0') +
                       dstd::String(hex, len) + " of " + a;
            },
            [&] {
                char hex[24];
                const auto r = dstd::to_chars(hex, hex + sizeof(hex), id, 16);
                const std::size_t len = static_cast<std::size_t>(r.ptr - hex);
                std::string s = std::string("chunk ") +
                                std::string(16 - len, '0') +
                                std::string(hex, len) + " of " +
                                std::string(a.data(), a.size());
                asm volatile("" : : "r"(s.data()) : "memory");
            },
            [&] {
                char hex[24];
                const auto r = dstd::to_chars(hex, hex + sizeof(hex), id, 16);
                const std::size_t len = static_cast<std::size_t>(r.ptr - hex);
                std::string s;
                s.append("chunk ");
                s.append(16 - len, '0');
                s.append(hex, len);
                s.append(" of ");
                s.append(a.data(), a.size());
                asm volatile("" : : "r"(s.data()) : "memory");
            },
            true));
    }

    {
        const double sec = in.seconds;
        const dstd::String& a = in.name;
        rows.push_back(run_row(
            "float .3f + string", iterations,
            [&](auto isb) {
                if constexpr (decltype(isb)::value)
                    return dstd::fmt<"{} took {:.3f} s">(a, sec);
                else
                    return dstd::fmt("{} took {:.3f} s"_f, a, sec);
            },
            [&] {
                return via_snprintf([&](char* buf, std::size_t cap) {
                    return std::snprintf(buf, cap, "%.*s took %.3f s",
                                         static_cast<int>(a.size()), a.data(),
                                         sec);
                });
            },
            [&] {
                return common::str_cat(a, " took ", dstd::fixed_text(sec),
                                       " s");
            },
            [&] { return a + " took " + dstd::to_string(sec) + " s"; },
            [&] {
                std::string s = std::string(a.data(), a.size()) + " took " +
                                std::to_string(sec) + " s";
                asm volatile("" : : "r"(s.data()) : "memory");
            },
            [&] {
                std::string s;
                s.append(a.data(), a.size());
                s.append(" took ");
                s.append(std::to_string(sec));
                s.append(" s");
                asm volatile("" : : "r"(s.data()) : "memory");
            },
            false));
    }

    {
        const dstd::String& nm = in.name;
        const dstd::String& raw = in.raw;
        rows.push_back(run_row(
            "2 strings (quoted)", iterations,
            [&](auto isb) {
                if constexpr (decltype(isb)::value)
                    return dstd::fmt<
                        "{}: '{}' is not a byte size (use e.g. "
                        "4294967296, 512MB, 4GB)">(nm, raw);
                else
                    return dstd::fmt(
                        "{}: '{}' is not a byte size (use e.g. "
                        "4294967296, 512MB, 4GB)"_f,
                        nm, raw);
            },
            [&] {
                return via_snprintf([&](char* buf, std::size_t cap) {
                    return std::snprintf(
                        buf, cap,
                        "%.*s: '%.*s' is not a byte size (use e.g. 4294967296, "
                        "512MB, 4GB)",
                        static_cast<int>(nm.size()), nm.data(),
                        static_cast<int>(raw.size()), raw.data());
                });
            },
            [&] {
                return common::str_cat(nm, ": '", raw,
                                       "' is not a byte size (use e.g. "
                                       "4294967296, 512MB, 4GB)");
            },
            [&] {
                return nm + ": '" + raw +
                       "' is not a byte size (use e.g. 4294967296, 512MB, "
                       "4GB)";
            },
            [&] {
                std::string s = std::string(nm.data(), nm.size()) + ": '" +
                                std::string(raw.data(), raw.size()) +
                                "' is not a byte size (use e.g. 4294967296, "
                                "512MB, 4GB)";
                asm volatile("" : : "r"(s.data()) : "memory");
            },
            [&] {
                std::string s;
                s.append(nm.data(), nm.size());
                s.append(": '");
                s.append(raw.data(), raw.size());
                s.append(
                    "' is not a byte size (use e.g. 4294967296, 512MB, "
                    "4GB)");
                asm volatile("" : : "r"(s.data()) : "memory");
            },
            true));
    }

    {
        const std::int64_t ts = in.ts;
        const std::int64_t dur = in.dur;
        const dstd::String& nm = in.name;
        const std::int64_t pid = in.offset;
        rows.push_back(run_row(
            "JSON line (4 holes)", iterations,
            [&](auto isb) {
                if constexpr (decltype(isb)::value)
                    return dstd::fmt<
                        R"({"id":{},"name":"{}","ts":{},"dur":{}})">(pid, nm,
                                                                     ts, dur);
                else
                    return dstd::fmt(
                        R"({"id":{},"name":"{}","ts":{},"dur":{}})"_f, pid, nm,
                        ts, dur);
            },
            [&] {
                return via_snprintf([&](char* buf, std::size_t cap) {
                    return std::snprintf(
                        buf, cap,
                        R"({"id":%lld,"name":"%.*s","ts":%lld,"dur":%lld})",
                        static_cast<long long>(pid),
                        static_cast<int>(nm.size()), nm.data(),
                        static_cast<long long>(ts),
                        static_cast<long long>(dur));
                });
            },
            [&] {
                return common::str_cat(R"({"id":)", pid, R"(,"name":")", nm,
                                       R"(","ts":)", ts, R"(,"dur":)", dur,
                                       "}");
            },
            [&] {
                return dstd::String(R"({"id":)") + dstd::to_string(pid) +
                       R"(,"name":")" + nm + R"(","ts":)" +
                       dstd::to_string(ts) + R"(,"dur":)" +
                       dstd::to_string(dur) + "}";
            },
            [&] {
                std::string s = std::string(R"({"id":)") + std::to_string(pid) +
                                R"(,"name":")" +
                                std::string(nm.data(), nm.size()) +
                                R"(","ts":)" + std::to_string(ts) +
                                R"(,"dur":)" + std::to_string(dur) + "}";
                asm volatile("" : : "r"(s.data()) : "memory");
            },
            [&] {
                std::string s;
                s.append(R"({"id":)");
                s.append(std::to_string(pid));
                s.append(R"(,"name":")");
                s.append(nm.data(), nm.size());
                s.append(R"(","ts":)");
                s.append(std::to_string(ts));
                s.append(R"(,"dur":)");
                s.append(std::to_string(dur));
                s.append("}");
                asm volatile("" : : "r"(s.data()) : "memory");
            },
            true));
    }

    print(rows);
    if (!g_mismatch.empty()) {
        for (const std::string& m : g_mismatch)
            std::fprintf(stderr, "mismatch: %s\n", m.c_str());
        return 1;
    }
    return 0;
}
