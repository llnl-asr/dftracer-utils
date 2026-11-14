// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/agg/detail.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#include <numeric>
#include <utility>

namespace dftracer::utils::dataframe {

namespace {

dstd::Unexpected<dstd::Error> udaf_invalid(dstd::StringView what) {
    return dstd::Unexpected(dstd::Error::from_format<"group_by: {}">(
        dstd::Condition::InvalidArgument, what));
}

}  // namespace

dstd::Vector<std::int32_t> AggState::udaf_arg_cols(const AggSpec& sp) {
    dstd::Vector<std::int32_t> cols;
    if (sp.value_col >= 0) cols.push_back(sp.value_col);
    cols.insert(cols.end(), sp.args.begin(), sp.args.end());
    return cols;
}

void AggState::init_udaf_layout() {
    spec_udaf.assign(specs.size(), -1);
    udafs.clear();
    udaf_serial = false;
    for (std::size_t s = 0; s < specs.size(); ++s) {
        const AggSpec& sp = specs[s];
        if (sp.op != AggOp::Udaf) continue;
        UdafSlot slot;
        slot.spec = s;
        if (sp.fn.empty()) {
            if (init_error.empty())
                init_error = "group_by: a udaf aggregate has no function name";
        } else {
            slot.record = find_udaf(sp.fn);
            if (!slot.record && init_error.empty())
                init_error = dstd::fmt<"group_by: no udaf named '{}'">(sp.fn);
        }
        if (!slot.record || !slot.record->is_parallel_safe())
            udaf_serial = true;
        spec_udaf[s] = static_cast<int>(udafs.size());
        udafs.push_back(std::move(slot));
    }
    has_udaf = !udafs.empty();
}

dstd::Result<void> AggState::udaf_prepare(
    const dstd::Vector<const Series*>& values) {
    for (UdafSlot& slot : udafs) {
        const AggSpec& sp = specs[slot.spec];
        if (sp.value_col < 0 && !sp.args.empty())
            return udaf_invalid(dstd::fmt<"udaf '{}' has arguments but no "
                                          "first column">(sp.fn));
        const dstd::Vector<std::int32_t> cols = udaf_arg_cols(sp);
        dstd::Vector<TypeId> types;
        types.reserve(cols.size());
        for (std::size_t a = 0; a < cols.size(); ++a) {
            const std::int32_t c = cols[a];
            if (c < 0 || static_cast<std::size_t>(c) >= values.size())
                return udaf_invalid(
                    dstd::fmt<"udaf '{}' argument {} names a column that is "
                              "not there">(sp.fn, a + 1));
            types.push_back(values[static_cast<std::size_t>(c)]->type());
        }
        if (slot.binding) {
            for (std::size_t a = 0; a < types.size(); ++a)
                if (types[a] != slot.arg_types[a])
                    return udaf_invalid(
                        dstd::fmt<"udaf '{}' argument {} changed type from {} "
                                  "to {} between batches">(
                            sp.fn, a + 1, type_name(slot.arg_types[a]),
                            type_name(types[a])));
            continue;
        }
        dstd::Vector<DataType> data_types;
        data_types.reserve(types.size());
        for (const TypeId t : types) data_types.push_back(scalar(t));
        dstd::Result<UdafBinding> bound = bind_udaf(
            slot.record,
            dstd::Span<const DataType>(data_types.data(), data_types.size()),
            {});
        if (!bound) return dstd::Unexpected(std::move(bound.error()));
        slot.arg_types = std::move(types);
        slot.binding = dstd::make_shared<const UdafBinding>(std::move(*bound));
    }
    return {};
}

dstd::Result<UdafStates*> AggState::udaf_table(std::size_t i) const {
    UdafSlot& slot = udafs[i];
    if (!slot.states) slot.states = dstd::make_unique<UdafStates>(slot.binding);
    return slot.states.get();
}

dstd::Result<void> AggState::udaf_sync() const {
    for (std::size_t i = 0; i < udafs.size(); ++i) {
        if (!udafs[i].binding) continue;
        dstd::Result<UdafStates*> table = udaf_table(i);
        if (!table) return dstd::Unexpected(std::move(table.error()));
        dstd::Result<void> grown = (*table)->grow(ngroups());
        if (!grown) return grown;
    }
    return {};
}

dstd::Result<void> AggState::udaf_update(
    const dstd::Vector<std::int64_t>& gids,
    const dstd::Vector<const Series*>& values, std::int64_t begin,
    std::int64_t end) {
    for (std::size_t i = 0; i < udafs.size(); ++i) {
        const dstd::Vector<std::int32_t> cols =
            udaf_arg_cols(specs[udafs[i].spec]);
        dstd::Vector<Series> slices;
        slices.reserve(cols.size());
        dstd::Vector<const Series*> args;
        args.reserve(cols.size());
        for (const std::int32_t c : cols) {
            const Series* col = values[static_cast<std::size_t>(c)];
            if (begin == 0 && end == col->length()) {
                args.push_back(col);
            } else {
                slices.push_back(col->slice(begin, end - begin));
                args.push_back(&slices.back());
            }
        }
        dstd::Result<UdafStates*> table = udaf_table(i);
        if (!table) return dstd::Unexpected(std::move(table.error()));
        dstd::Result<void> grown = (*table)->grow(ngroups());
        if (!grown) return grown;
        dstd::Result<void> fed = (*table)->update(
            dstd::Span<const std::int64_t>(gids.data(), gids.size()),
            dstd::Span<const Series* const>(args.data(), args.size()));
        if (!fed) return fed;
    }
    return {};
}

void AggState::udaf_adopt(const AggState& other) {
    has_udaf = other.has_udaf;
    udaf_serial = other.udaf_serial;
    spec_udaf = other.spec_udaf;
    udafs.clear();
    udafs.reserve(other.udafs.size());
    for (const UdafSlot& o : other.udafs) {
        UdafSlot slot;
        slot.spec = o.spec;
        slot.record = o.record;
        slot.arg_types = o.arg_types;
        slot.binding = o.binding;
        udafs.push_back(std::move(slot));
    }
}

void AggState::udaf_combine(const AggState& other,
                            const dstd::Vector<std::int64_t>& into,
                            const dstd::Vector<std::int64_t>& from) {
    if (!has_udaf) return;
    if (dstd::Result<void> synced = other.udaf_sync(); !synced)
        return udaf_note(std::move(synced.error()));
    if (dstd::Result<void> synced = udaf_sync(); !synced)
        return udaf_note(std::move(synced.error()));
    for (std::size_t i = 0; i < udafs.size(); ++i) {
        const UdafSlot& src = other.udafs[i];
        if (!src.binding) continue;
        UdafSlot& dst = udafs[i];
        if (!dst.binding) {
            dst.arg_types = src.arg_types;
            dst.binding = src.binding;
        } else if (dst.arg_types != src.arg_types) {
            return udaf_note(
                dstd::Error::from_format<
                    "group_by: udaf '{}' merged states of other argument "
                    "types">(dstd::Condition::InvalidArgument,
                             specs[dst.spec].fn));
        }
        dstd::Result<UdafStates*> table = udaf_table(i);
        if (!table) return udaf_note(std::move(table.error()));
        if (dstd::Result<void> grown = (*table)->grow(ngroups()); !grown)
            return udaf_note(std::move(grown.error()));
        dstd::Result<UdafStates*> from_table = other.udaf_table(i);
        if (!from_table) return udaf_note(std::move(from_table.error()));
        dstd::Result<void> merged = (*table)->combine_pairs(
            **from_table,
            dstd::Span<const std::int64_t>(into.data(), into.size()),
            dstd::Span<const std::int64_t>(from.data(), from.size()));
        if (!merged) return udaf_note(std::move(merged.error()));
    }
}

void AggState::udaf_permute(const dstd::Vector<std::int64_t>& perm) {
    if (!has_udaf) return;
    if (dstd::Result<void> synced = udaf_sync(); !synced)
        return udaf_note(std::move(synced.error()));
    dstd::Vector<std::int64_t> into(perm.size());
    std::iota(into.begin(), into.end(), std::int64_t{0});
    for (std::size_t i = 0; i < udafs.size(); ++i) {
        UdafSlot& slot = udafs[i];
        if (!slot.binding) continue;
        auto fresh = dstd::make_unique<UdafStates>(slot.binding);
        if (dstd::Result<void> grown =
                fresh->grow(static_cast<std::int64_t>(perm.size()));
            !grown)
            return udaf_note(std::move(grown.error()));
        dstd::Result<UdafStates*> old = udaf_table(i);
        if (!old) return udaf_note(std::move(old.error()));
        dstd::Result<void> moved = fresh->combine_pairs(
            **old, dstd::Span<const std::int64_t>(into.data(), into.size()),
            dstd::Span<const std::int64_t>(perm.data(), perm.size()));
        if (!moved) return udaf_note(std::move(moved.error()));
        slot.states = std::move(fresh);
    }
}

std::size_t AggState::udaf_bytes() const {
    std::size_t bytes = 0;
    for (const UdafSlot& slot : udafs)
        if (slot.states) bytes += slot.states->approx_bytes();
    return bytes;
}

Series AggState::udaf_column(std::size_t spec) const {
    const auto n = static_cast<std::size_t>(ngroups());
    const std::size_t i = static_cast<std::size_t>(spec_udaf[spec]);
    auto failed = [&](dstd::Error error) {
        udaf_note(std::move(error));
        const dstd::Vector<std::int64_t> zeros(n, 0);
        const dstd::Vector<std::uint8_t> none((n + 7) / 8, 0);
        return Series::flat_i64(zeros.data(), static_cast<std::int64_t>(n),
                                none.data());
    };
    if (udaf_error) return failed(*udaf_error);
    if (!udafs[i].binding && n == 0) return Series::flat_i64(nullptr, 0);
    if (!udafs[i].binding)
        return failed(dstd::Error::from_format<
                      "group_by: udaf '{}' has groups but no bound "
                      "arguments">(dstd::Condition::Internal, specs[spec].fn));
    if (dstd::Result<void> synced = udaf_sync(); !synced)
        return failed(std::move(synced.error()));
    dstd::Result<UdafStates*> table = udaf_table(i);
    if (!table) return failed(std::move(table.error()));
    dstd::Result<Series> column = (*table)->finalize();
    if (!column) return failed(std::move(column.error()));
    return std::move(*column);
}

bool agg_specs_serial(const dstd::Vector<AggSpec>& specs) {
    for (const AggSpec& sp : specs) {
        if (sp.op != AggOp::Udaf) continue;
        const dstd::SharedPtr<const UdafRecord> record = find_udaf(sp.fn);
        if (!record || !record->is_parallel_safe()) return true;
    }
    return false;
}

bool agg_serial_only(const AggState& state) { return state.udaf_serial; }

dstd::Result<void> agg_check(const AggState& state) {
    if (state.udaf_error) return dstd::Unexpected(*state.udaf_error);
    return {};
}

}  // namespace dftracer::utils::dataframe
