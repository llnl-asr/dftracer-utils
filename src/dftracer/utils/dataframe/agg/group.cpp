#include <concurrentqueue.h>
#include <dftracer/utils/core/common/platform_compat.h>  // hardware_concurrency
#include <dftracer/utils/dataframe/agg/detail.h>
#include <dftracer/utils/dataframe/batch_ops.h>          // concat, take
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/kernels/sort.h>       // argsort
#include <dftracer/utils/dataframe/parallel.h>           // parallel_for

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace dftracer::utils::dataframe {

namespace {
constexpr std::int64_t AGG_GRAIN = 1 << 16;
}  // namespace

AggStatePtr group_agg_state(const std::vector<const Series*>& in_keys,
                            const std::vector<const Series*>& in_values,
                            std::vector<AggSpec> specs) {
    // The accumulator reads FLAT columns; a view (a filter's selection, a
    // dictionary) is materialized once here, for every caller.
    std::vector<Series> owned;
    owned.reserve(in_keys.size() + in_values.size());
    auto flat = [&](const std::vector<const Series*>& cols) {
        std::vector<const Series*> out;
        out.reserve(cols.size());
        for (const Series* c : cols) {
            if (c->encoding() == Encoding::Flat) {
                out.push_back(c);
            } else {
                owned.push_back(c->materialize());
                out.push_back(&owned.back());
            }
        }
        return out;
    };
    const std::vector<const Series*> keys = flat(in_keys);
    const std::vector<const Series*> values = flat(in_values);
    const std::int64_t n = !keys.empty()     ? keys[0]->length()
                           : !values.empty() ? values[0]->length()
                                             : 0;
    if (n <= AGG_GRAIN) {
        auto st = agg_new(std::move(specs));
        agg_accumulate(*st, keys, values);
        return st;
    }
    // Each chunk folds into a partial taken from a pool and handed back, so
    // there is one partial per thread in flight (every partial re-discovers
    // every group) while a fast core still takes more chunks than a slow one.
    std::vector<AggStatePtr> pool;
    std::mutex pool_mutex;
    parallel_for(n, AGG_GRAIN, [&](std::int64_t b, std::int64_t e) {
        AggStatePtr st;
        {
            const std::lock_guard<std::mutex> lock(pool_mutex);
            if (!pool.empty()) {
                st = std::move(pool.back());
                pool.pop_back();
            }
        }
        if (!st) st = agg_new(specs);
        agg_accumulate(*st, keys, values, b, e);
        const std::lock_guard<std::mutex> lock(pool_mutex);
        pool.push_back(std::move(st));
    });
    // The pool hands chunks to partials in no fixed order, so the merged
    // groups go back into the order the frame first shows them.
    AggStatePtr merged = agg_in_first_seen_order(agg_merge_many(pool));
    return merged ? std::move(merged) : agg_new(std::move(specs));
}

namespace {

// Many groups on a string key: each chunk scatters its rows (row and
// hash, key words, values) into a page per partition by hash, and a full
// page goes to the partition's queue, where the thread that filled it (or
// whoever holds the partition) folds it into the partition's state, whose
// table fits the first-level cache; no barrier between packing and
// aggregating, and the pages in flight stay few. The groups go back into
// first-seen order. Nullopt when the shape does not pack or the groups
// are few, where the chunked partials are cheaper than the scatter.
std::optional<DataFrame> group_agg_partitioned(
    const std::vector<const Series*>& in_keys,
    const std::vector<const Series*>& in_values,
    const std::vector<AggSpec>& specs,
    const std::vector<std::string>& key_names) {
    constexpr std::int64_t MANY_GROUPS = 2048;
    // Four partitions per thread: tables small enough for the first-level
    // cache, and the final drain's rounds divide the thread count.
    const auto threads = static_cast<std::int64_t>(
        std::max<std::size_t>(2, dftracer::utils::hardware_concurrency()));
    const auto PARTITIONS = static_cast<std::size_t>(4 * threads);
    const std::int64_t n = in_keys.empty() ? 0 : in_keys[0]->length();
    if (n <= 16 * AGG_GRAIN || !parallel_backend_installed())
        return std::nullopt;
    std::vector<Series> owned;
    owned.reserve(in_keys.size() + in_values.size());
    auto flat = [&](const std::vector<const Series*>& cols) {
        std::vector<const Series*> out;
        for (const Series* c : cols) {
            owned.push_back(c->encoding() == Encoding::Flat ? c->share()
                                                            : c->materialize());
            out.push_back(&owned.back());
        }
        return out;
    };
    const std::vector<const Series*> keys = flat(in_keys);
    const std::vector<const Series*> values = flat(in_values);
    AggPacked shape;
    AggStatePtr probe = agg_new(specs);
    // Integer keys have the direct table, which the scatter cannot beat.
    if (!agg_pack_shape(*probe, keys, values, shape) || !shape.strings)
        return std::nullopt;
    agg_accumulate(*probe, keys, values, 0, AGG_GRAIN / 4);
    if (probe->ngroups() < MANY_GROUPS) return std::nullopt;

    // No barriers: a thread packs its chunk into a page per partition and,
    // when a page fills, queues it on the partition and drains that queue
    // into the partition's state if no one else holds it (else the holder
    // will). The pages in flight are the ones filled while a holder was
    // busy, so memory is the partial pages plus little.
    struct Partition {
        std::atomic<bool> held{false};  // the state's owner
        moodycamel::ConcurrentQueue<std::unique_ptr<std::uint64_t[]>> queue;
        AggStatePtr state;
    };
    std::vector<Partition> partitions(PARTITIONS);
    for (Partition& part : partitions) part.state = agg_new(specs);
    const std::size_t per_page = AggPages::PAGE_WORDS / shape.stride;
    auto drain = [&](std::size_t p) {
        Partition& part = partitions[p];
        if (part.held.exchange(true, std::memory_order_acquire)) return;
        for (;;) {
            AggPages view;
            view.open(shape.stride);
            view.pages.resize(64);
            const std::size_t got =
                part.queue.try_dequeue_bulk(view.pages.begin(), 64);
            if (got == 0) break;
            view.pages.resize(got);
            view.count = got * per_page;
            agg_accumulate_packed(*part.state, keys, values, shape, view);
        }
        part.held.store(false, std::memory_order_release);
    };
    using Pages = std::vector<AggPages>;
    std::vector<std::unique_ptr<Pages>> pool;
    std::mutex pool_mutex;
    std::atomic<bool> packed{true};
    parallel_for(n, AGG_GRAIN, [&](std::int64_t b, std::int64_t e) {
        if (!packed.load(std::memory_order_relaxed)) return;
        std::unique_ptr<Pages> pages;
        {
            const std::lock_guard<std::mutex> lock(pool_mutex);
            if (!pool.empty()) {
                pages = std::move(pool.back());
                pool.pop_back();
            }
        }
        if (!pages) pages = std::make_unique<Pages>(PARTITIONS);
        auto full = [&](std::size_t p) {
            for (auto& page : (*pages)[p].release())
                partitions[p].queue.enqueue(std::move(page));
            drain(p);
        };
        if (!agg_pack(*probe, keys, values, b, e, shape, *pages, full))
            packed.store(false, std::memory_order_relaxed);
        const std::lock_guard<std::mutex> lock(pool_mutex);
        pool.push_back(std::move(pages));
    });
    if (!packed.load()) return std::nullopt;
    // The partial pages and whatever is still queued, one partition per
    // task, no holder to wait for.
    std::vector<AggStatePtr> states(PARTITIONS);
    parallel_for(static_cast<std::int64_t>(PARTITIONS), 1,
                 [&](std::int64_t p0, std::int64_t p1) {
                     for (std::int64_t p = p0; p < p1; ++p) {
                         const auto pi = static_cast<std::size_t>(p);
                         drain(pi);
                         for (const std::unique_ptr<Pages>& pages : pool) {
                             AggPages& rest = (*pages)[pi];
                             if (rest.count > 0)
                                 agg_accumulate_packed(*partitions[pi].state,
                                                       keys, values, shape,
                                                       rest);
                             rest.pages.clear();
                         }
                         states[pi] = std::move(partitions[pi].state);
                     }
                 });
    pool.clear();
    std::vector<DataFrame> parts(PARTITIONS);
    std::vector<std::vector<std::int64_t>> first(PARTITIONS);
    parallel_for(static_cast<std::int64_t>(PARTITIONS), 1,
                 [&](std::int64_t p0, std::int64_t p1) {
                     for (std::int64_t p = p0; p < p1; ++p) {
                         const auto pi = static_cast<std::size_t>(p);
                         if (states[pi]->ngroups() == 0) continue;
                         first[pi] = states[pi]->group_first_row;
                         parts[pi] = agg_finalize(*states[pi], key_names);
                     }
                 });
    std::vector<const DataFrame*> ptrs;
    std::vector<std::int64_t> first_all;
    for (std::size_t p = 0; p < PARTITIONS; ++p) {
        if (parts[p].columns.empty()) continue;
        ptrs.push_back(&parts[p]);
        first_all.insert(first_all.end(), first[p].begin(), first[p].end());
    }
    if (ptrs.empty()) return std::nullopt;
    const DataFrame all = concat(ptrs, ConcatHow::Vertical);
    const Series first_col = Series::flat_i64(
        first_all.data(), static_cast<std::int64_t>(first_all.size()));
    return take(all, argsort(first_col, false));
}

}  // namespace

DataFrame group_agg(const std::vector<const Series*>& keys,
                    const std::vector<const Series*>& values,
                    std::vector<AggSpec> specs,
                    const std::vector<std::string>& key_names) {
    if (std::optional<DataFrame> out =
            group_agg_partitioned(keys, values, specs, key_names))
        return std::move(*out);
    return agg_finalize(*group_agg_state(keys, values, std::move(specs)),
                        key_names);
}

DataFrame group_agg(const Series& key, const std::vector<const Series*>& values,
                    std::vector<AggSpec> specs, const std::string& key_name) {
    const std::vector<const Series*> keys{&key};
    return group_agg(keys, values, std::move(specs),
                     std::vector<std::string>{key_name});
}

}  // namespace dftracer::utils::dataframe
