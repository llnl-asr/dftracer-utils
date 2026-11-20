// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/internal/float_key.h>
#include <dftracer/utils/dataframe/internal/string_reader.h>
#include <dftracer/utils/dataframe/internal/value_ids.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <cstring>

namespace dftracer::utils::dataframe {

namespace {

constexpr char BOOL_BYTES[2] = {0, 1};

// Open-addressing table from a value of at most 8 bytes to its id; an empty
// slot has id -1. Doubles at half full.
struct FixedTable {
    struct Slot {
        std::uint64_t key;
        std::int32_t id;
    };
    dstd::Vector<Slot> slots = dstd::Vector<Slot>(1024, Slot{0, -1});
    std::size_t mask = 1023;
    std::size_t used = 0;

    void grow() { rehash(slots.size() * 2); }

    /// Makes room for `distinct` values without another rehash.
    void reserve(std::size_t distinct) {
        std::size_t cap = slots.size();
        while (cap < distinct * 2) cap <<= 1;
        if (cap > slots.size()) rehash(cap);
    }

    void rehash(std::size_t cap) {
        dstd::Vector<Slot> old = std::move(slots);
        slots = dstd::Vector<Slot>(cap, Slot{0, -1});
        mask = slots.size() - 1;
        for (const Slot& o : old) {
            if (o.id < 0) continue;
            std::size_t h = dstd::hash::splitmix64(o.key) & mask;
            while (slots[h].id >= 0) h = (h + 1) & mask;
            slots[h] = o;
        }
    }

    /// The id of `key`, or -1 when it is not in the table.
    std::int32_t find(std::uint64_t key) const {
        std::size_t h = dstd::hash::splitmix64(key) & mask;
        for (;;) {
            const Slot& s = slots[h];
            if (s.id < 0) return -1;
            if (s.key == key) return s.id;
            h = (h + 1) & mask;
        }
    }

    /// The id of `key`, or -1 with the slot taken by `fresh_id`.
    std::int32_t find_or_add(std::uint64_t key, std::int32_t fresh_id) {
        std::size_t h = dstd::hash::splitmix64(key) & mask;
        for (;;) {
            Slot& s = slots[h];
            if (s.id < 0) {
                s.key = key;
                s.id = fresh_id;
                if (++used * 2 > slots.size()) grow();
                return -1;
            }
            if (s.key == key) return s.id;
            h = (h + 1) & mask;
        }
    }
};

struct Builder {
    dstd::HashMap<dstd::StringView, std::int32_t> map;
    // A value of 1 to 8 bytes that is not text keys by its bytes as a number,
    // a float by its canonical bits (-0.0 is +0.0, every NaN is one value).
    FixedTable table;
    ValueIds& out;
    bool is_string;
    bool is_bool;
    bool is_float;
    std::size_t width;

    bool uses_table() const { return !is_string && width >= 1 && width <= 8; }

    std::int32_t intern_key(std::uint64_t key, std::int64_t row) {
        const auto fresh_id = static_cast<std::int32_t>(out.first.size());
        const std::int32_t id = table.find_or_add(key, fresh_id);
        if (id >= 0) return id;
        out.first.push_back(row);
        return fresh_id;
    }

    std::int32_t intern(dstd::StringView s, std::int64_t row) {
        if (uses_table()) {
            std::uint64_t key = 0;
            std::memcpy(&key, s.data(), s.size());
            if (is_float)
                key = canonical_float_bits(key, static_cast<unsigned>(width));
            return intern_key(key, row);
        }
        auto [it, fresh] =
            map.try_emplace(s, static_cast<std::int32_t>(out.first.size()));
        if (fresh) out.first.push_back(row);
        return it->second;
    }

    std::int32_t null(std::int64_t row) {
        if (out.null_id < 0) {
            out.null_id = static_cast<std::int32_t>(out.first.size());
            out.first.push_back(row);
        }
        return out.null_id;
    }

    bool key_at(const dftu_series* c, std::int64_t i, dstd::StringView& s) {
        if (is_string) return str_at(c, i, s);
        for (;;) {
            if (c->validity.block() &&
                !((c->validity.data()[i >> 3] >> (i & 7)) & 1))
                return false;
            switch (c->encoding) {
                case Encoding::Dictionary:
                    i = reinterpret_cast<const std::int32_t*>(
                        c->data.data())[i];
                    c = c->child().get();
                    break;
                case Encoding::Selection: {
                    const std::int64_t idx =
                        reinterpret_cast<const std::int64_t*>(
                            c->data.data())[i];
                    if (idx < 0) return false;
                    i = idx;
                    c = c->child().get();
                    break;
                }
                case Encoding::Chunked: {
                    std::int64_t local = 0;
                    c = &c->chunk_at(i, local);
                    i = local;
                    break;
                }
                default: {
                    const auto* d = c->data.data();
                    if (is_bool)
                        s = dstd::StringView(
                            BOOL_BYTES + ((d[i >> 3] >> (i & 7)) & 1), 1);
                    else
                        s = dstd::StringView(
                            reinterpret_cast<const char*>(d) +
                                static_cast<std::size_t>(i) * width,
                            width);
                    return true;
                }
            }
        }
    }

    std::uint64_t key_of(const std::uint8_t* d, std::int64_t i) const {
        std::uint64_t key = 0;
        std::memcpy(&key, d + static_cast<std::size_t>(i) * width, width);
        return is_float
                   ? canonical_float_bits(key, static_cast<unsigned>(width))
                   : key;
    }

    // Rows `from` .. n-1 of a FLAT column with no nulls whose first rows were
    // mostly new, the whole column of this call: the values are split into
    // partitions by hash, each partition (small enough to stay in cache) is
    // numbered on its own table in row order, and the numbers are then put in
    // first-seen order by one scan over the rows that first hold a value that
    // rows before `from` did not. The shared table is not updated: nothing
    // reads it after this column.
    void fill_partitioned(const std::uint8_t* d, std::int64_t from,
                          std::int64_t n) {
        constexpr unsigned PART_BITS = 6;
        constexpr std::size_t PARTS = std::size_t{1} << PART_BITS;
        std::int32_t* ids = out.ids.data();
        const std::int64_t rest = n - from;
        const std::int64_t chunks =
            std::min<std::int64_t>(64, (rest + (1 << 16) - 1) >> 16);
        const std::int64_t chunk_rows = (rest + chunks - 1) / chunks;
        auto chunk_range = [&](std::int64_t c, std::int64_t& lo,
                               std::int64_t& hi) {
            lo = from + c * chunk_rows;
            hi = std::min(n, lo + chunk_rows);
        };
        dstd::Vector<std::uint8_t> part_of(static_cast<std::size_t>(n));
        dstd::Vector<std::size_t> cursor(
            static_cast<std::size_t>(chunks) * PARTS, 0);
        parallel_for(chunks, 1, [&](std::int64_t c0, std::int64_t c1) {
            for (std::int64_t c = c0; c < c1; ++c) {
                std::int64_t lo, hi;
                chunk_range(c, lo, hi);
                std::size_t* hist =
                    cursor.data() + static_cast<std::size_t>(c) * PARTS;
                for (std::int64_t i = lo; i < hi; ++i) {
                    const auto p = static_cast<std::uint8_t>(
                        dstd::hash::splitmix64(key_of(d, i)) >>
                        (64 - PART_BITS));
                    part_of[static_cast<std::size_t>(i)] = p;
                    ++hist[p];
                }
            }
        });
        dstd::Vector<std::size_t> start(PARTS + 1, 0);
        for (std::size_t p = 0; p < PARTS; ++p) {
            std::size_t total = start[p];
            for (std::int64_t c = 0; c < chunks; ++c) {
                std::size_t& at =
                    cursor[static_cast<std::size_t>(c) * PARTS + p];
                const std::size_t count = at;
                at = total;
                total += count;
            }
            start[p + 1] = total;
        }
        dstd::Vector<std::uint32_t> rows(static_cast<std::size_t>(rest));
        parallel_for(chunks, 1, [&](std::int64_t c0, std::int64_t c1) {
            for (std::int64_t c = c0; c < c1; ++c) {
                std::int64_t lo, hi;
                chunk_range(c, lo, hi);
                std::size_t* at =
                    cursor.data() + static_cast<std::size_t>(c) * PARTS;
                for (std::int64_t i = lo; i < hi; ++i)
                    rows[at[part_of[static_cast<std::size_t>(i)]]++] =
                        static_cast<std::uint32_t>(i);
            }
        });
        struct Part {
            dstd::Vector<std::uint64_t> keys;
            dstd::Vector<std::int64_t> first;
            dstd::Vector<std::int32_t> global;
        };
        dstd::Vector<Part> parts(PARTS);
        dstd::Vector<std::uint8_t> opens(static_cast<std::size_t>(n), 0);
        parallel_for(
            static_cast<std::int64_t>(PARTS), 1,
            [&](std::int64_t p0, std::int64_t p1) {
                for (std::int64_t p = p0; p < p1; ++p) {
                    Part& part = parts[static_cast<std::size_t>(p)];
                    FixedTable local;
                    local.reserve(start[static_cast<std::size_t>(p) + 1] -
                                  start[static_cast<std::size_t>(p)]);
                    for (std::size_t k = start[static_cast<std::size_t>(p)];
                         k < start[static_cast<std::size_t>(p) + 1]; ++k) {
                        const std::int64_t i = rows[k];
                        const std::uint64_t key = key_of(d, i);
                        const auto fresh_id =
                            static_cast<std::int32_t>(part.keys.size());
                        std::int32_t id = local.find_or_add(key, fresh_id);
                        if (id < 0) {
                            id = fresh_id;
                            part.keys.push_back(key);
                            part.first.push_back(i);
                        }
                        ids[i] = id;
                    }
                    part.global.resize(part.keys.size());
                    for (std::size_t j = 0; j < part.keys.size(); ++j) {
                        part.global[j] = table.find(part.keys[j]);
                        if (part.global[j] < 0)
                            opens[static_cast<std::size_t>(part.first[j])] = 1;
                    }
                }
            });
        dstd::Vector<std::int32_t> row_id(static_cast<std::size_t>(n), -1);
        for (std::int64_t i = from; i < n; ++i)
            if (opens[static_cast<std::size_t>(i)]) {
                row_id[static_cast<std::size_t>(i)] =
                    static_cast<std::int32_t>(out.first.size());
                out.first.push_back(i);
            }
        parallel_for(
            static_cast<std::int64_t>(PARTS), 1,
            [&](std::int64_t p0, std::int64_t p1) {
                for (std::int64_t p = p0; p < p1; ++p) {
                    Part& part = parts[static_cast<std::size_t>(p)];
                    for (std::size_t j = 0; j < part.keys.size(); ++j)
                        if (part.global[j] < 0)
                            part.global[j] =
                                row_id[static_cast<std::size_t>(part.first[j])];
                }
            });
        parallel_for(chunks, 1, [&](std::int64_t c0, std::int64_t c1) {
            for (std::int64_t c = c0; c < c1; ++c) {
                std::int64_t lo, hi;
                chunk_range(c, lo, hi);
                for (std::int64_t i = lo; i < hi; ++i)
                    ids[i] = parts[part_of[static_cast<std::size_t>(i)]]
                                 .global[static_cast<std::size_t>(ids[i])];
            }
        });
    }

    // A FLAT column of values of 1 to 8 bytes. The first rows go one at a time
    // into the table. A column whose first rows are nearly all new is sized for
    // the rest at once, as growing the table row by row costs more than the
    // lookups (and a long one with no nulls goes to fill_partitioned). A column
    // whose first rows repeat a few values is split into runs, each numbered on
    // its own table, and the runs merged in row order, so the ids stay in
    // first-seen order.
    void fill_fixed(const dftu_series* c, std::int64_t base) {
        std::int32_t* ids = out.ids.data() + base;
        const std::uint8_t* d = c->data.data();
        const std::uint8_t* valid =
            c->validity.block() ? c->validity.data() : nullptr;
        constexpr std::int64_t PROBE_ROWS = 1 << 16;
        const std::int64_t n = c->length;
        const std::int64_t serial = std::min(n, PROBE_ROWS);
        for (std::int64_t i = 0; i < serial; ++i) {
            if (valid && !((valid[i >> 3] >> (i & 7)) & 1))
                ids[i] = null(base + i);
            else
                ids[i] = intern_key(key_of(d, i), base + i);
        }
        if (serial == n) return;
        const bool repeats =
            table.used * 8 < static_cast<std::size_t>(serial) * 7;
        constexpr std::int64_t PARTITION_ROWS = std::int64_t{1} << 18;
        if (!repeats && !valid && base == 0 &&
            static_cast<std::size_t>(n) == out.ids.size() &&
            n - serial >= PARTITION_ROWS && n < (std::int64_t{1} << 31) &&
            parallel_backend_installed()) {
            fill_partitioned(d, serial, n);
            return;
        }
        if (!repeats || !parallel_backend_installed()) {
            if (!repeats) table.reserve(static_cast<std::size_t>(n));
            for (std::int64_t i = serial; i < n; ++i) {
                if (valid && !((valid[i >> 3] >> (i & 7)) & 1))
                    ids[i] = null(base + i);
                else
                    ids[i] = intern_key(key_of(d, i), base + i);
            }
            return;
        }
        struct Run {
            FixedTable table;
            dstd::Vector<std::uint64_t> keys;
            dstd::Vector<std::int64_t> first;
            std::int32_t null_local = -1;
            dstd::Vector<std::int32_t> global;
        };
        const std::int64_t rest = n - serial;
        const std::int64_t run_rows =
            std::max<std::int64_t>(PROBE_ROWS, (rest + 63) / 64);
        const std::int64_t runs = (rest + run_rows - 1) / run_rows;
        dstd::Vector<Run> state(static_cast<std::size_t>(runs));
        parallel_for(runs, 1, [&](std::int64_t r0, std::int64_t r1) {
            for (std::int64_t r = r0; r < r1; ++r) {
                Run& run = state[static_cast<std::size_t>(r)];
                const std::int64_t lo = serial + r * run_rows;
                const std::int64_t hi = std::min(n, lo + run_rows);
                for (std::int64_t i = lo; i < hi; ++i) {
                    std::int32_t id;
                    const auto fresh_id =
                        static_cast<std::int32_t>(run.keys.size());
                    if (valid && !((valid[i >> 3] >> (i & 7)) & 1)) {
                        if (run.null_local < 0) {
                            run.null_local = fresh_id;
                            run.keys.push_back(0);
                            run.first.push_back(i);
                        }
                        id = run.null_local;
                    } else {
                        const std::uint64_t key = key_of(d, i);
                        id = run.table.find_or_add(key, fresh_id);
                        if (id < 0) {
                            run.keys.push_back(key);
                            run.first.push_back(i);
                            id = fresh_id;
                        }
                    }
                    ids[i] = id;
                }
            }
        });
        for (Run& run : state) {
            run.global.resize(run.keys.size());
            for (std::size_t j = 0; j < run.keys.size(); ++j) {
                const std::int64_t row = base + run.first[j];
                run.global[j] = static_cast<std::int32_t>(j) == run.null_local
                                    ? null(row)
                                    : intern_key(run.keys[j], row);
            }
        }
        parallel_for(runs, 1, [&](std::int64_t r0, std::int64_t r1) {
            for (std::int64_t r = r0; r < r1; ++r) {
                const Run& run = state[static_cast<std::size_t>(r)];
                const std::int64_t lo = serial + r * run_rows;
                const std::int64_t hi = std::min(n, lo + run_rows);
                for (std::int64_t i = lo; i < hi; ++i)
                    ids[i] = run.global[static_cast<std::size_t>(ids[i])];
            }
        });
    }

    void fill(const dftu_series* c, std::int64_t base) {
        std::int32_t* ids = out.ids.data() + base;
        dstd::StringView s;
        if (c->is_chunked()) {
            const auto* starts =
                reinterpret_cast<const std::int64_t*>(c->data.data());
            for (std::size_t k = 0; k < c->nested.size(); ++k)
                fill(c->nested[k].series.get(), base + starts[k]);
            return;
        }
        if (c->encoding == Encoding::Dictionary && c->child()) {
            const dftu_series* dict = c->child().get();
            const auto k = static_cast<std::uint32_t>(dict->length);
            const auto* codes =
                reinterpret_cast<const std::int32_t*>(c->data.data());
            dstd::Vector<std::int32_t> entry(k, -2);
            for (std::int64_t i = 0; i < c->length; ++i) {
                const std::int64_t row = base + i;
                if (c->validity.block() &&
                    !((c->validity.data()[i >> 3] >> (i & 7)) & 1)) {
                    ids[i] = null(row);
                    continue;
                }
                const auto code = static_cast<std::uint32_t>(codes[i]);
                if (code >= k) {
                    ids[i] = null(row);
                    continue;
                }
                std::int32_t& e = entry[code];
                if (e == -2)
                    e = key_at(dict, code, s) ? intern(s, row) : null(row);
                ids[i] = e;
            }
            return;
        }
        if (c->encoding == Encoding::Flat && !is_bool && uses_table()) {
            fill_fixed(c, base);
            return;
        }
        for (std::int64_t i = 0; i < c->length; ++i)
            ids[i] = key_at(c, i, s) ? intern(s, base + i) : null(base + i);
    }
};

}  // namespace

void combine_value_ids(dstd::Vector<std::int32_t>& comb,
                       dstd::Span<const std::int32_t> next,
                       dstd::Vector<std::int64_t>& first) {
    FixedTable table;
    table.reserve(first.size());
    first.clear();
    const std::size_t n = comb.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t key = (static_cast<std::uint64_t>(comb[i]) << 32) |
                                  static_cast<std::uint32_t>(next[i]);
        const auto fresh_id = static_cast<std::int32_t>(first.size());
        const std::int32_t id = table.find_or_add(key, fresh_id);
        if (id < 0) {
            first.push_back(static_cast<std::int64_t>(i));
            comb[i] = fresh_id;
        } else {
            comb[i] = id;
        }
    }
}

bool value_ids(const dftu_series& v, ValueIds& out) {
    if (value_domain(v.type) == ValueDomain::None) return false;
    out.ids.assign(static_cast<std::size_t>(v.length), 0);
    out.first.clear();
    out.null_id = -1;
    const TypeId narrow = narrow_varwidth_type(v.type);
    Builder b{{},
              {},
              out,
              narrow == TypeId::String || narrow == TypeId::Binary,
              v.type == TypeId::Bool,
              v.type == TypeId::Float16 || v.type == TypeId::Float32 ||
                  v.type == TypeId::Float64,
              0};
    if (v.type == TypeId::FixedSizeBinary)
        b.width = static_cast<std::size_t>(v.fixed_size());
    else
        b.width = byte_width(v.type).value_or(0);
    b.map.reserve(64);
    b.fill(&v, 0);
    return true;
}

}  // namespace dftracer::utils::dataframe
