// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/trace/index/extensions/plugin_columns.h>
#include <dftracer/utils/trace/views/fold_event.h>

#include <algorithm>
#include <bit>
#include <limits>
#include <variant>

namespace dftracer::utils::trace::index::extensions {

namespace {

constexpr std::uint32_t NO_ID = dftracer::utils::dstd::StringIntern::NO_ID;
constexpr std::uint64_t INT64_TOP =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

dstd::Vector<std::uint8_t> bitmap(dstd::Span<const std::uint8_t> valid) {
    dstd::Vector<std::uint8_t> out((valid.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < valid.size(); ++i)
        if (valid[i]) out[i / 8] |= static_cast<std::uint8_t>(1U << (i % 8));
    return out;
}

bool has_nulls(dstd::Span<const std::uint8_t> valid) {
    for (std::uint8_t v : valid)
        if (!v) return true;
    return false;
}

}  // namespace

PluginColumns::PluginColumns(dstd::Span<const dstd::String> paths) {
    for (auto& f : fixed_) f = UNSET;
    for (const dstd::String& path : paths) {
        if (path == "*") {
            all_ = true;
            continue;
        }
        if (wanted_.count(path)) continue;
        wanted_.emplace(path, static_cast<std::int32_t>(accs_.size()));
        accs_.push_back({path, {}, {}, Kind::None, false, false, false});
    }
}

bool PluginColumns::is_never(dstd::StringView path) const {
    return std::binary_search(
        never_.begin(), never_.end(), path,
        [](dstd::StringView a, dstd::StringView b) { return a < b; });
}

void PluginColumns::set_never_index(dstd::Span<const dstd::String> never) {
    never_ = never;
    for (Acc& a : accs_) a.never = is_never(a.path);
}

std::int32_t PluginColumns::index_of(dstd::StringView path) {
    if (auto it = wanted_.find(path); it != wanted_.end()) return it->second;
    if (!all_ || is_never(path)) return NOT_WANTED;
    const auto idx = static_cast<std::int32_t>(accs_.size());
    accs_.push_back(
        {dstd::String(path), {}, {}, Kind::None, false, false, false});
    wanted_.emplace(dstd::String(path), idx);
    return idx;
}

std::int32_t PluginColumns::fixed_index(Fixed which) {
    static constexpr dstd::StringView NAMES[FxCount] = {
        "name", "cat", "fhash", "hhash", "pid", "tid", "ts", "dur"};
    if (fixed_[which] == UNSET) fixed_[which] = index_of(NAMES[which]);
    return fixed_[which];
}

std::int32_t PluginColumns::arg_index(std::uint32_t key, bool by_path) {
    auto& memo = args_[by_path ? 1 : 0];
    if (auto it = memo.find(key); it != memo.end()) return it->second;
    const dstd::StringView name = intern_->resolve(key);
    const std::int32_t idx =
        index_of(by_path ? dstd::String(name) : dstd::fmt<"args.{}">(name));
    memo.emplace(key, idx);
    return idx;
}

void PluginColumns::put(std::int32_t acc, Kind kind, std::uint64_t bits,
                        bool negative, bool big) {
    if (acc < 0) return;
    Acc& a = accs_[static_cast<std::size_t>(acc)];
    if (a.never || a.kind == Kind::Mixed) return;
    if (a.kind == Kind::None) {
        a.kind = kind;
    } else if (a.kind != kind) {
        a.kind = Kind::Mixed;
        return;
    }
    // The first value of a path in a record wins.
    if (a.valid.size() > rows_) return;
    a.valid.resize(rows_, 0);
    a.bits.resize(rows_, 0);
    a.valid.push_back(1);
    a.bits.push_back(bits);
    a.negative |= negative;
    a.big |= big;
}

void PluginColumns::put_uint(std::int32_t acc, std::uint64_t value) {
    put(acc, Kind::Int, value, false, value > INT64_TOP);
}

void PluginColumns::put_id(std::int32_t acc, std::uint32_t id) {
    put(acc, Kind::String, id, false, false);
}

void PluginColumns::add(
    dstd::Span<const trace::views::detail::FoldEvent> events,
    const dftracer::utils::dstd::StringIntern& intern) {
    intern_ = &intern;
    for (const auto& e : events) {
        if (e.phase != trace::RecordPhase::Metadata) {
            if (!e.by_path) {
                if (e.name_id != NO_ID) put_id(fixed_index(FxName), e.name_id);
                if (e.cat_id != NO_ID) put_id(fixed_index(FxCat), e.cat_id);
                if (e.fhash_id != NO_ID)
                    put_id(fixed_index(FxFhash), e.fhash_id);
                if (e.hhash_id != NO_ID)
                    put_id(fixed_index(FxHhash), e.hhash_id);
                put_uint(fixed_index(FxPid), e.pid);
                put_uint(fixed_index(FxTid), e.tid);
                put_uint(fixed_index(FxTs), e.ts);
                if (e.has_dur) put_uint(fixed_index(FxDur), e.dur);
            }
            for (const auto& [key, value] : e.args) {
                const std::int32_t acc = arg_index(key, e.by_path);
                if (acc < 0) continue;
                if (const auto* i = std::get_if<std::int64_t>(&value))
                    put(acc, Kind::Int, static_cast<std::uint64_t>(*i), *i < 0,
                        false);
                else if (const auto* u = std::get_if<std::uint64_t>(&value))
                    put_uint(acc, *u);
                else if (const auto* d = std::get_if<double>(&value))
                    put(acc, Kind::Double, std::bit_cast<std::uint64_t>(*d),
                        false, false);
                else
                    put_id(acc, std::get<std::uint32_t>(value));
            }
        }
        ++rows_;
    }
}

PluginColumns::Taken PluginColumns::take() {
    Taken out;
    for (Acc& a : accs_) {
        if (a.never && rows_ > 0) out.never.push_back(a.path);
        const Kind kind = a.kind;
        if (kind == Kind::None) continue;
        const bool unsound = kind == Kind::Mixed || (a.big && a.negative);
        if (unsound) {
            out.mixed.push_back(a.path);
        } else {
            a.valid.resize(rows_, 0);
            a.bits.resize(rows_, 0);
            const auto n = static_cast<std::int64_t>(rows_);
            const bool nulls = has_nulls({a.valid.data(), a.valid.size()});
            dstd::Vector<std::uint8_t> bits;
            if (nulls) bits = bitmap({a.valid.data(), a.valid.size()});
            const std::uint8_t* validity = nulls ? bits.data() : nullptr;
            if (kind == Kind::String) {
                dstd::Vector<dstd::StringView> views(rows_);
                for (std::size_t i = 0; i < rows_; ++i)
                    if (a.valid[i])
                        views[i] = intern_->resolve(
                            static_cast<std::uint32_t>(a.bits[i]));
                out.columns.push_back(
                    {a.path, dataframe::Series::strings(
                                 dstd::Span<const dstd::StringView>(
                                     views.data(), views.size()),
                                 validity)});
            } else {
                const dataframe::TypeId type =
                    kind == Kind::Double ? dataframe::TypeId::Float64
                                         : (a.big ? dataframe::TypeId::Uint64
                                                  : dataframe::TypeId::Int64);
                out.columns.push_back(
                    {a.path, dataframe::Series::flat(type, a.bits.data(), n,
                                                     validity)});
            }
        }
        a.bits.clear();
        a.valid.clear();
        a.kind = Kind::None;
        a.negative = false;
        a.big = false;
    }
    rows_ = 0;
    return out;
}

}  // namespace dftracer::utils::trace::index::extensions
