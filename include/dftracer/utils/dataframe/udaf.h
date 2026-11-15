// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_UDAF_H
#define DFTRACER_UTILS_DATAFRAME_UDAF_H

#include <dftracer/utils/dataframe/c_api_udaf.h>
#include <dftracer/utils/dataframe/udaf_adapter.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

#include <utility>

namespace dftracer::utils::dataframe {

/// A registered user-defined aggregate. Move-only.
///
/// A handle from @ref make_udaf owns the registration: destroying it removes
/// the name, so no new lookup finds it. A handle from @ref udaf only looks the
/// name up and never removes it. Either way a copy of the record that was taken
/// before keeps the UDAF callable, and so keeps the plugin library that holds
/// it loaded.
class Udaf {
   public:
    Udaf(dstd::String name, dstd::SharedPtr<const void> record,
         bool is_owning) noexcept
        : name_(std::move(name)),
          record_(std::move(record)),
          is_owning_(is_owning) {}
    Udaf(Udaf&& other) noexcept
        : name_(std::move(other.name_)),
          record_(std::move(other.record_)),
          is_owning_(other.is_owning_) {
        other.name_.clear();
        other.is_owning_ = false;
    }
    Udaf& operator=(Udaf&& other) noexcept;
    Udaf(const Udaf&) = delete;
    Udaf& operator=(const Udaf&) = delete;
    ~Udaf();

    dstd::StringView name() const noexcept { return name_; }

    /// Whether destroying this handle removes the name from the registry.
    bool is_owning() const noexcept { return is_owning_; }

   private:
    dstd::String name_;
    dstd::SharedPtr<const void> record_;
    bool is_owning_;
};

/// Registers the UDAF record `desc` like dftu_svc_ops::register_udaf, for code
/// in this process. The result owns the registration. Fails, naming the
/// problem, for a bad descriptor or a name that a built-in function or
/// aggregate, an op, a UDF or another UDAF already holds.
dstd::Result<Udaf> make_udaf(const dftu_udaf_desc& desc, void* self = nullptr);

/// Registers `A` as a UDAF named `A::NAME` for this process, with the same
/// bind, update, combine, finalize and state rules as a plugin UDAF: both build
/// the descriptor through one adapter. No library is loaded. The result owns
/// the registration, so the name leaves the registry when it is destroyed.
template <NamedUdafType A>
dstd::Result<Udaf> make_udaf() {
    const dftu_udaf_desc desc =
        detail::udaf_desc_of<A>(A::NAME, detail::udaf_options_of<A>());
    return make_udaf(desc, nullptr);
}

/// The UDAF registered under `name`, from a plugin or @ref make_udaf. Fails
/// with NotFound for an unknown name. The handle does not own the registration.
dstd::Result<Udaf> udaf(dstd::StringView name);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_UDAF_H
