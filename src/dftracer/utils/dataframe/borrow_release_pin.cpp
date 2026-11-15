// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/internal/borrow_release_pin.h>

#include <atomic>

using dftracer::utils::dataframe::BorrowRelease;

namespace {

using PinFn = void* (*)(BorrowRelease) noexcept;
using DropFn = void (*)(void*) noexcept;

std::atomic<PinFn> g_pin{nullptr};
std::atomic<DropFn> g_drop{nullptr};

}  // namespace

namespace dftracer::utils::dataframe {

void set_borrow_release_pinner(PinFn pin, DropFn drop) noexcept {
    g_drop.store(drop, std::memory_order_release);
    g_pin.store(pin, std::memory_order_release);
}

void* pin_borrow_release(BorrowRelease release) noexcept {
    const PinFn pin = g_pin.load(std::memory_order_acquire);
    if (release == nullptr || pin == nullptr) return nullptr;
    return pin(release);
}

void drop_borrow_release_pin(void* pin) noexcept {
    const DropFn drop = g_drop.load(std::memory_order_acquire);
    if (pin != nullptr && drop != nullptr) drop(pin);
}

}  // namespace dftracer::utils::dataframe
