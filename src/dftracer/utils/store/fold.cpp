#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/store/fold.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::store::engine {
namespace {

const std::uint8_t* byte_data(dstd::ByteView bytes) {
    return reinterpret_cast<const std::uint8_t*>(bytes.data());
}

void copy_value(dstd::ByteView value, dstd::Vector<std::uint8_t>& out) {
    out.clear();
    if (value.empty()) return;
    const auto* begin = byte_data(value);
    out.insert(out.end(), begin, begin + value.size());
}

bool numeric(dstd::ByteView accumulated, dstd::ByteView value) {
    return accumulated.size() == 8 && value.size() == 8;
}

std::uint64_t bits_of(dstd::ByteView value) {
    return dftracer::utils::dstd::read_u64_le(byte_data(value));
}

void store_bits(std::uint64_t bits, dstd::Vector<std::uint8_t>& out) {
    out.clear();
    out.resize(8);
    dftracer::utils::dstd::write_u64_le(out.data(), bits);
}

std::int64_t signed_of(dstd::ByteView value) {
    return std::bit_cast<std::int64_t>(bits_of(value));
}

double double_of(dstd::ByteView value) {
    return dftracer::utils::dstd::bits_double(bits_of(value));
}

void fold_sum_i64(dstd::ByteView accumulated, dstd::ByteView value,
                  dstd::Vector<std::uint8_t>& out) {
    if (!numeric(accumulated, value)) {
        copy_value(accumulated, out);
        return;
    }
    // Wrapping, so a sum that overflows is still a defined 8-byte number.
    store_bits(bits_of(accumulated) + bits_of(value), out);
}

void fold_sum_f64(dstd::ByteView accumulated, dstd::ByteView value,
                  dstd::Vector<std::uint8_t>& out) {
    if (!numeric(accumulated, value)) {
        copy_value(accumulated, out);
        return;
    }
    store_bits(dftracer::utils::dstd::double_bits(double_of(accumulated) +
                                                  double_of(value)),
               out);
}

void fold_min_i64(dstd::ByteView accumulated, dstd::ByteView value,
                  dstd::Vector<std::uint8_t>& out) {
    if (!numeric(accumulated, value)) {
        copy_value(accumulated, out);
        return;
    }
    store_bits(signed_of(value) < signed_of(accumulated) ? bits_of(value)
                                                         : bits_of(accumulated),
               out);
}

void fold_max_i64(dstd::ByteView accumulated, dstd::ByteView value,
                  dstd::Vector<std::uint8_t>& out) {
    if (!numeric(accumulated, value)) {
        copy_value(accumulated, out);
        return;
    }
    store_bits(signed_of(value) > signed_of(accumulated) ? bits_of(value)
                                                         : bits_of(accumulated),
               out);
}

void fold_min_f64(dstd::ByteView accumulated, dstd::ByteView value,
                  dstd::Vector<std::uint8_t>& out) {
    if (!numeric(accumulated, value)) {
        copy_value(accumulated, out);
        return;
    }
    store_bits(double_of(value) < double_of(accumulated) ? bits_of(value)
                                                         : bits_of(accumulated),
               out);
}

void fold_max_f64(dstd::ByteView accumulated, dstd::ByteView value,
                  dstd::Vector<std::uint8_t>& out) {
    if (!numeric(accumulated, value)) {
        copy_value(accumulated, out);
        return;
    }
    store_bits(double_of(value) > double_of(accumulated) ? bits_of(value)
                                                         : bits_of(accumulated),
               out);
}

void fold_last(dstd::ByteView /*accumulated*/, dstd::ByteView value,
               dstd::Vector<std::uint8_t>& out) {
    copy_value(value, out);
}

void fold_concat(dstd::ByteView accumulated, dstd::ByteView value,
                 dstd::Vector<std::uint8_t>& out) {
    out.clear();
    for (const dstd::ByteView part : {accumulated, value}) {
        if (part.empty()) continue;
        const auto* begin = byte_data(part);
        out.insert(out.end(), begin, begin + part.size());
    }
}

}  // namespace

bool is_numeric_fold(FoldKind kind) {
    switch (kind) {
        case FoldKind::SumI64:
        case FoldKind::SumF64:
        case FoldKind::MinI64:
        case FoldKind::MaxI64:
        case FoldKind::MinF64:
        case FoldKind::MaxF64:
            return true;
        default:
            return false;
    }
}

FoldFn fold_fn(FoldKind kind) {
    switch (kind) {
        case FoldKind::SumI64:
        case FoldKind::Count:
            return fold_sum_i64;
        case FoldKind::SumF64:
            return fold_sum_f64;
        case FoldKind::MinI64:
            return fold_min_i64;
        case FoldKind::MaxI64:
            return fold_max_i64;
        case FoldKind::MinF64:
            return fold_min_f64;
        case FoldKind::MaxF64:
            return fold_max_f64;
        case FoldKind::Last:
            return fold_last;
        case FoldKind::Concat:
            return fold_concat;
        default:
            return nullptr;
    }
}

dstd::Expected<void, dstd::Error> check_fold_value(FoldKind kind,
                                                   dstd::ByteView value) {
    if (is_numeric_fold(kind) && value.size() != 8) {
        return dstd::Unexpected<dstd::Error>(
            dstd::Error(dstd::Condition::InvalidArgument));
    }
    return {};
}

}  // namespace dftracer::utils::store::engine
