// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/internal/frame_native.h>
#include <dftracer/utils/dataframe/internal/lazy_plan.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/index/cache/lookup_store.h>
#include <dftracer/utils/index/plan/rowsets.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/views/view_duql_internal.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

namespace dftracer::utils::trace::views::duql_detail {

namespace {

constexpr std::uint64_t LOOKUP_MAX_ROWS = 1'000'000;
constexpr std::int64_t TYPE_PROBE_ROWS = 65'536;
// Keys a side's distinct rows in the lookup cache apart from its rows.
constexpr std::uint64_t DISTINCT_CACHE_SALT = 0x64697374696e6374ULL;
constexpr std::uint64_t LOOKUP_MAX_BYTES = std::uint64_t{256} << 20;

struct LookupCaps {
    std::uint64_t rows;
    std::uint64_t bytes;

    static LookupCaps read() {
        return {common::Env::get<std::uint64_t>("DUQL_LOOKUP_MAX_ROWS")
                    .value_or(LOOKUP_MAX_ROWS),
                common::Env::get<std::uint64_t>("DUQL_LOOKUP_MAX_BYTES")
                    .value_or(LOOKUP_MAX_BYTES)};
    }

    bool over(std::uint64_t n_rows, std::uint64_t n_bytes) const {
        return n_rows > rows || n_bytes > bytes;
    }

    void check_rows(const dstd::String& name, std::uint64_t n) const {
        if (n > rows)
            refuse("'" + name + "' gives " + dstd::to_string(n) +
                   " rows or more, over DUQL_LOOKUP_MAX_ROWS (" +
                   dstd::to_string(rows) +
                   "); a lookup reads a small row set: narrow it with "
                   "'where' or 'select'");
    }

    void check_bytes(const dstd::String& name, std::uint64_t n) const {
        if (n > bytes)
            refuse("'" + name + "' holds " + dstd::to_string(n) +
                   " bytes, over DUQL_LOOKUP_MAX_BYTES (" +
                   dstd::to_string(bytes) +
                   "); a lookup reads a small row set: narrow it with "
                   "'select'");
    }
};

// Stores a side's rows in the lookup cache; a store that fails keeps none.
void persist(const CacheKey& key,
             const dstd::Vector<dataframe::DataFrame>& parts) {
    if (parts.empty()) return;
    try {
        const auto db = index::cache::open_lookup_db(
            key.path, index::cache::CacheStore::Mode::ReadWrite);
        if (!db) return;
        dstd::Vector<const dataframe::DataFrame*> ptrs;
        for (const auto& p : parts) ptrs.push_back(&p);
        index::cache::persist_lookup(
            *db, key.sig,
            common::unwrap(
                ptrs.size() == 1
                    ? dataframe::frame_to_native(parts.front())
                    : dataframe::frame_to_native(
                          common::unwrap(dataframe::concat(
                              ptrs, dataframe::ConcatHow::Diagonal)))));
    } catch (const std::exception&) {
    }
}

}  // namespace

Sides::Sides(duql::Program p)
    : program(std::move(p)),
      views(program.sides.size()),
      columns(program.sides.size()),
      keys(program.sides.size()),
      files(program.sides.size()),
      stored(program.sides.size(), 0),
      frames_(program.sides.size()),
      once_(program.sides.size()),
      key_frames_(program.sides.size()),
      key_once_(program.sides.size()) {}

dstd::SharedPtr<const dataframe::DataFrame> Sides::frame(std::size_t i) {
    std::call_once(once_[i], [&] { frames_[i] = run(i); });
    return frames_[i];
}

dstd::SharedPtr<const dataframe::DataFrame> Sides::probe(std::size_t i) const {
    const dataframe::LazyFrame plan = views[i]->head(TYPE_PROBE_ROWS).lazy();
    dataframe::DataFrame f;
    common::unwrap(dftracer::utils::default_runtime().block_on(
        coro::named("duql_side_types", [&]() -> coro::CoroTask<void> {
            f = common::unwrap(co_await plan.collect());
            co_return;
        }())));
    return dstd::make_shared<const dataframe::DataFrame>(std::move(f));
}

dstd::SharedPtr<const dataframe::DataFrame> Sides::key_frame(std::size_t i) {
    std::call_once(key_once_[i], [&] { key_frames_[i] = distinct(i); });
    return key_frames_[i];
}

Pushdown Sides::prepare(const duql::Pipeline& p) {
    dstd::Vector<std::size_t> reads;
    term_reads(p, reads);
    for (const auto i : reads) frame(i);
    dstd::Vector<std::size_t> sets;
    set_reads(p, sets);
    for (const auto i : sets)
        if (std::find(reads.begin(), reads.end(), i) == reads.end())
            key_frame(i);
    {
        const std::lock_guard<std::mutex> lock(bind_);
        // Built after the walk: make_lookup_table can throw, and the walk is
        // in duql, which is built without exceptions.
        dstd::Vector<const duql::TLookup*> unbound;
        duql::for_each_pipeline_term(p, [&](const duql::Term& t) {
            const auto* l = std::get_if<duql::TLookup>(&t.node);
            if (!l || duql::joined(*l) || l->slot->table) return;
            unbound.push_back(l);
        });
        for (const duql::TLookup* l : unbound) {
            if (l->slot->table) continue;
            const auto& rows =
                frames_[l->side] ? frames_[l->side] : key_frames_[l->side];
            l->slot->table = common::unwrap(duql::make_lookup_table(*l, rows));
        }
    }
    return key_sets(p);
}

dstd::SharedPtr<const dataframe::DataFrame> Sides::distinct(std::size_t i) {
    const dstd::String& name = dstd::String(program.sides[i].name);
    dstd::Optional<CacheKey> key = keys[i];
    if (key) {
        key->sig = dftracer::utils::dstd::hash::splitmix64(key->sig ^
                                                           DISTINCT_CACHE_SALT);
        try {
            if (const auto db = index::cache::open_lookup_db(
                    key->path, index::cache::CacheStore::Mode::ReadOnly))
                if (auto bytes = index::cache::read_lookup(*db, key->sig))
                    if (auto cached = dataframe::frame_from_native(*bytes))
                        return dstd::make_shared<const dataframe::DataFrame>(
                            duql::flat_frame(std::move(*cached)));
        } catch (const std::exception&) {
        }
    }
    const std::uint64_t budget = dftracer::utils::memory::resolve_spill_budget(
        views[i]->memory_budget_bytes());
    dftracer::utils::dstd::StringViewSet seen;
    dstd::Vector<dataframe::DataFrame> parts;
    std::uint64_t bytes = 0;
    const dataframe::LazyFrame plan = views[i]->lazy();
    common::unwrap(dftracer::utils::default_runtime().block_on(
        coro::named("duql_key_set", [&]() -> coro::CoroTask<void> {
            auto gen = plan.stream();
            dstd::String row_key;
            for (;;) {
                auto f = co_await gen.next();
                if (!f) break;
                dataframe::DataFrame part =
                    duql::flat_frame(common::unwrap(std::move(*f)));
                dstd::Vector<std::int64_t> take;
                for (std::int64_t r = 0; r < part.num_rows(); ++r) {
                    row_key.clear();
                    bool keyed = true;
                    for (const auto& c : part.columns)
                        keyed = keyed && duql::append_cell_key(row_key, c, r);
                    if (!keyed || !seen.insert(row_key).second) continue;
                    take.push_back(r);
                    bytes += row_key.size() + sizeof(dstd::String);
                }
                if (take.empty()) continue;
                dataframe::DataFrame kept;
                kept.names = part.names;
                for (const auto& c : part.columns)
                    kept.columns.push_back(c.take(take));
                bytes += duql::frame_bytes(kept);
                if (bytes > budget)
                    refuse("'" + name + "' holds more distinct rows than " +
                           "the memory budget (" + dstd::to_string(budget) +
                           " bytes); a top-level 'in' reads them into "
                           "memory: raise the budget, or write the test " +
                           "under 'not' or in a later 'where' to join it");
                parts.push_back(std::move(kept));
            }
            co_return;
        }())));
    if (parts.empty()) {
        dataframe::DataFrame none;
        for (const auto& c : columns[i]) {
            none.names.emplace_back(c.name);
            none.columns.push_back(
                dataframe::Series::nulls(c.type.id == dataframe::TypeId::Unknown
                                             ? dataframe::TypeId::String
                                             : c.type.id,
                                         0));
        }
        return dstd::make_shared<const dataframe::DataFrame>(std::move(none));
    }
    if (key) persist(*key, parts);
    dstd::Vector<const dataframe::DataFrame*> ptrs;
    for (const auto& part : parts) ptrs.push_back(&part);
    return dstd::make_shared<const dataframe::DataFrame>(common::unwrap(
        dataframe::concat(ptrs, dataframe::ConcatHow::Diagonal)));
}

dstd::SharedPtr<const dataframe::DataFrame> Sides::run(std::size_t i) {
    const dstd::String& name = dstd::String(program.sides[i].name);
    const LookupCaps caps = LookupCaps::read();
    const auto limit = static_cast<std::int64_t>(std::min<std::uint64_t>(
        caps.rows, std::numeric_limits<std::int64_t>::max() - 1));
    if (stored[i])
        if (auto f = index::plan::stored_rowset(files[i], name)) {
            caps.check_rows(name, static_cast<std::uint64_t>(f->num_rows()));
            dataframe::DataFrame flat = duql::flat_frame(std::move(*f));
            caps.check_bytes(name, duql::frame_bytes(flat));
            return dstd::make_shared<const dataframe::DataFrame>(
                std::move(flat));
        }
    if (keys[i]) try {
            if (const auto db = index::cache::open_lookup_db(
                    keys[i]->path, index::cache::CacheStore::Mode::ReadOnly))
                if (auto bytes = index::cache::read_lookup(*db, keys[i]->sig))
                    if (auto cached = dataframe::frame_from_native(*bytes))
                        return dstd::make_shared<const dataframe::DataFrame>(
                            duql::flat_frame(std::move(*cached)));
        } catch (const std::exception&) {
        }
    const dataframe::LazyFrame plan = views[i]->head(limit + 1).lazy();
    dataframe::DataFrame f;
    common::unwrap(dftracer::utils::default_runtime().block_on(
        coro::named("duql_lookup_side", [&]() -> coro::CoroTask<void> {
            f = common::unwrap(co_await plan.collect());
            co_return;
        }())));
    caps.check_rows(name, static_cast<std::uint64_t>(f.num_rows()));
    f = duql::flat_frame(std::move(f));
    caps.check_bytes(name, duql::frame_bytes(f));
    if (keys[i]) {
        dstd::Vector<dataframe::DataFrame> parts;
        parts.push_back(common::unwrap(f.select(f.names)));
        persist(*keys[i], parts);
    }
    return dstd::make_shared<const dataframe::DataFrame>(std::move(f));
}

dstd::SharedPtr<Sides> owner(const dstd::WeakPtr<Sides>& sides) {
    auto s = sides.lock();
    if (!s) refuse("a row set ran after the query that reads it was released");
    return s;
}

namespace {

class SideSource final : public dataframe::Source {
   public:
    SideSource(dstd::SharedPtr<Sides> strong, dstd::WeakPtr<Sides> weak,
               std::size_t side, dataframe::Schema schema)
        : strong_(std::move(strong)),
          weak_(std::move(weak)),
          side_(side),
          schema_(std::move(schema)) {}

    dataframe::Schema schema() const override { return schema_; }

    dataframe::ScanResult scan(
        const dataframe::ScanRequest& req) const override {
        dataframe::ScanResult r;
        r.cursor = dstd::make_unique<Rows>(strong_ ? strong_ : owner(weak_),
                                           side_, req.projection);
        r.filters.assign(req.filters.size(), dataframe::Pushed::No);
        return r;
    }

   private:
    class Rows final : public dataframe::Cursor {
       public:
        Rows(dstd::SharedPtr<Sides> sides, std::size_t side,
             // types
             dstd::Vector<dstd::String> projection)
            : sides_(std::move(sides)),
              side_(side),
              projection_(std::move(projection)) {}

        coro::CoroTask<dataframe::MorselResult> next(
            std::int64_t max_rows) override {
            if (!rows_) {
                const auto frame = sides_->frame(side_);
                dataframe::DataFrame f;
                f.names = frame->names;
                for (const auto& c : frame->columns)
                    f.columns.push_back(c.share());
                if (!projection_.empty())
                    f = common::unwrap(f.select(projection_));
                rows_ = dataframe::InMemorySource(std::move(f)).scan({}).cursor;
            }
            auto next_task = rows_->next(max_rows);
            co_return co_await std::move(next_task);
        }

       private:
        dstd::SharedPtr<Sides> sides_;
        std::size_t side_;
        dstd::Vector<dstd::String> projection_;
        dstd::UniquePtr<dataframe::Cursor> rows_;
    };

    dstd::SharedPtr<Sides> strong_;
    dstd::WeakPtr<Sides> weak_;
    std::size_t side_;
    dataframe::Schema schema_;
};

}  // namespace

dataframe::LazyFrame side_source(dstd::SharedPtr<Sides> strong,
                                 dstd::WeakPtr<Sides> weak, std::size_t side,
                                 dataframe::Schema schema) {
    return dataframe::LazyFrame::scan(dstd::make_shared<SideSource>(
        std::move(strong), std::move(weak), side, std::move(schema)));
}

dstd::SharedPtr<const dataframe::detail::Tap> side_tap(
    dstd::SharedPtr<Sides> strong, dstd::WeakPtr<Sides> weak,
    std::size_t side) {
    auto tap = dstd::make_shared<dataframe::detail::Tap>();
    tap->open = [strong = std::move(strong), weak = std::move(weak), side] {
        struct State {
            std::uint64_t rows = 0;
            std::uint64_t bytes = 0;
            bool over = false;
            dstd::Vector<dataframe::DataFrame> parts;
        };
        auto state = dstd::make_shared<State>();
        const auto s = strong ? strong : owner(weak);
        const dstd::Optional<CacheKey> key = s->keys[side];
        const LookupCaps caps = LookupCaps::read();
        dataframe::detail::TapRun run;
        run.rows = [state, key,
                    caps](const dataframe::DataFrame& f) -> dstd::Result<void> {
            if (state->over) return {};
            state->rows += static_cast<std::uint64_t>(f.num_rows());
            dataframe::DataFrame part;
            part.names = f.names;
            for (const auto& c : f.columns) part.columns.push_back(c.share());
            part = duql::flat_frame(std::move(part));
            state->bytes += duql::frame_bytes(part);
            if (caps.over(state->rows, state->bytes)) {
                state->over = true;
                state->parts.clear();
                return {};
            }
            if (key) state->parts.push_back(std::move(part));
            return {};
        };
        run.end = [state, key]() -> dstd::Result<void> {
            if (key && !state->over) persist(*key, state->parts);
            return {};
        };
        return run;
    };
    return tap;
}

dstd::String cache_state(const dstd::Optional<CacheKey>& key) {
    if (!key) return "not cached";
    const auto db = index::cache::open_lookup_db(
        key->path, index::cache::CacheStore::Mode::ReadOnly);
    if (!db || !index::cache::lookup_exists(*db, key->sig)) return "not cached";
    try {
        if (auto bytes = index::cache::read_lookup(*db, key->sig))
            if (auto f = dataframe::frame_from_native(*bytes))
                return "cached, " + dstd::to_string(f->num_rows()) + " rows";
    } catch (const std::exception&) {
    }
    return "cached";
}

}  // namespace dftracer::utils::trace::views::duql_detail
