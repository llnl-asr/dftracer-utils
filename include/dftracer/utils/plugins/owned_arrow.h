#ifndef DFTRACER_UTILS_PLUGINS_OWNED_ARROW_H
#define DFTRACER_UTILS_PLUGINS_OWNED_ARROW_H

// In an Arrow build nanoarrow provides the Arrow C interfaces; otherwise
// arrow_abi.h does. Either one defines them all, so the include order of the
// two does not matter.
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <nanoarrow/nanoarrow.hpp>
#else
#include <dftracer/utils/plugins/arrow_abi.h>
#endif

namespace dftracer::utils::plugins {

/// An Arrow array+schema whose owner releases both; move-only. Used both
/// host-side (the named-result registry) and by plugin authors as the RAII
/// return type of the Host Arrow helpers.
struct OwnedArrow {
    ArrowArray array{};
    ArrowSchema schema{};

    OwnedArrow() = default;
    OwnedArrow(OwnedArrow&& o) noexcept { steal(o); }
    OwnedArrow& operator=(OwnedArrow&& o) noexcept {
        if (this != &o) {
            reset();
            steal(o);
        }
        return *this;
    }
    OwnedArrow(const OwnedArrow&) = delete;
    OwnedArrow& operator=(const OwnedArrow&) = delete;
    ~OwnedArrow() { reset(); }

    void reset() {
        if (array.release) array.release(&array);
        if (schema.release) schema.release(&schema);
        array = ArrowArray{};
        schema = ArrowSchema{};
    }

    /// True once both the array and schema carry a live release callback.
    explicit operator bool() const noexcept {
        return array.release != nullptr && schema.release != nullptr;
    }

   private:
    void steal(OwnedArrow& o) {
        array = o.array;
        schema = o.schema;
        o.array = ArrowArray{};
        o.schema = ArrowSchema{};
    }
};

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_OWNED_ARROW_H
