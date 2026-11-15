// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_BORROW_RELEASE_PIN_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_BORROW_RELEASE_PIN_H

namespace dftracer::utils::dataframe {

/// The release callback of a buffer a column borrows from its owner.
using BorrowRelease = void (*)(void* context);

/// Keep the code at @p release mapped until @ref drop_borrow_release_pin runs
/// on the answer. A borrowed-buffer owner calls @p release when its column
/// dies, which can be after the library that holds @p release was unloaded, so
/// whoever loaded that library installs a pinner here (see
/// @ref set_borrow_release_pinner). Answer is null for a null @p release and
/// when no pinner is installed or @p release lives outside every library the
/// pinner tracks; such a callback needs no pin.
void* pin_borrow_release(BorrowRelease release) noexcept;

/// Drop a pin from @ref pin_borrow_release. Call it AFTER @p release ran: it
/// may unload the library that held that code. A null pin does nothing.
void drop_borrow_release_pin(void* pin) noexcept;

/// Install the pinner and the dropper, or clear them with nulls. The plugins
/// host installs theirs once at startup; the dataframe library links no plugin
/// header and calls them through these hooks only.
void set_borrow_release_pinner(void* (*pin)(BorrowRelease) noexcept,
                               void (*drop)(void* pin) noexcept) noexcept;

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_BORROW_RELEASE_PIN_H
