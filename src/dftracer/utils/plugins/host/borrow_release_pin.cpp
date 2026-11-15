// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/host/borrow_release_pin.h>
#include <dlfcn.h>

#include <memory>
#include <mutex>

namespace dftracer::utils::plugins::host {

namespace {

using dataframe::BorrowRelease;

struct Entry {
    void* base;
    dstd::WeakPtr<PluginLibrary> library;
};

struct Registry {
    std::mutex mutex;
    dstd::Vector<Entry> entries;
    /// The base and library of the last hit, so a frame of many columns from
    /// one plugin pays the scan once rather than once per column.
    void* last_base = nullptr;
    dstd::WeakPtr<PluginLibrary> last_library;
};

Registry& registry() {
    // Destroyed before the memory manager it allocates from: this static is
    // built after that one, long after the first allocation.
    static Registry r;
    return r;
}

void* pin_library_release(BorrowRelease release) noexcept {
    if (release == nullptr) return nullptr;
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(release), &info) == 0 ||
        info.dli_fbase == nullptr)
        return nullptr;
    Registry& reg = registry();
    dstd::SharedPtr<PluginLibrary> library;
    {
        std::lock_guard<std::mutex> lock(reg.mutex);
        if (reg.last_base == info.dli_fbase) library = reg.last_library.lock();
        if (!library) {
            // Several sets can load one file at one base; any live one holds
            // the mapping, so pin the first that is still alive.
            for (const Entry& entry : reg.entries) {
                if (entry.base != info.dli_fbase) continue;
                library = entry.library.lock();
                if (library) break;
            }
            if (!library) return nullptr;
            reg.last_base = info.dli_fbase;
            reg.last_library = library;
        }
    }
    dstd::UniquePtr<dstd::SharedPtr<PluginLibrary>> hold =
        dstd::make_unique<dstd::SharedPtr<PluginLibrary>>(library);
    return hold.release();
}

void drop_library_pin(void* pin) noexcept {
    if (pin == nullptr) return;
    dstd::UniquePtr<dstd::SharedPtr<PluginLibrary>>(
        static_cast<dstd::SharedPtr<PluginLibrary>*>(pin))
        .reset();
}

void install_pinner() {
    static std::once_flag once;
    std::call_once(once, [] {
        dataframe::set_borrow_release_pinner(pin_library_release,
                                             drop_library_pin);
    });
}

}  // namespace

void track_plugin_library(void* image_base,
                          const dstd::SharedPtr<PluginLibrary>& library) {
    if (image_base == nullptr || library == nullptr) return;
    install_pinner();
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    dstd::Vector<Entry> live;
    live.reserve(reg.entries.size() + 1);
    for (Entry& entry : reg.entries)
        if (!entry.library.expired()) live.push_back(std::move(entry));
    live.push_back(Entry{image_base, library});
    reg.entries = std::move(live);
}

}  // namespace dftracer::utils::plugins::host
