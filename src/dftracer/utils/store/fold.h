#ifndef DFTRACER_UTILS_STORE_FOLD_H
#define DFTRACER_UTILS_STORE_FOLD_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/store/segment.h>

#include <cstdint>

namespace dftracer::utils::store {

/// NamespaceItem::fold_kind stores these numbers, so they are append only.
enum class FoldKind : std::uint8_t {
    None = 0,
    SumI64 = 1,
    SumF64 = 2,
    MinI64 = 3,
    MaxI64 = 4,
    MinF64 = 5,
    MaxF64 = 6,
    Count = 7,
    Last = 8,
    Concat = 9
};

/// True for the folds that store one 8-byte little-endian number: an integer
/// or an IEEE double.
bool is_numeric_fold(FoldKind kind);

/// The fold to give SegmentBuilder::seal, or nullptr for FoldKind::None.
FoldFn fold_fn(FoldKind kind);

/// InvalidArgument when a numeric fold gets a value that is not 8 bytes.
/// FoldKind::None, Count, Last and Concat take any bytes.
dstd::Expected<void, dstd::Error> check_fold_value(FoldKind kind,
                                                   ByteView value);

}  // namespace dftracer::utils::store

#endif  // DFTRACER_UTILS_STORE_FOLD_H
