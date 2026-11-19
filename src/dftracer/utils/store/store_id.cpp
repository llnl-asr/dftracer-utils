// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/store/store_id.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

namespace dftracer::utils::store::detail {

namespace {

/// How long a reader waits for the creator of a short store.id, on its own
/// clock: the file times come from another machine's clock on a shared file
/// system, so they say nothing about how long a creator took.
constexpr std::uint64_t CREATOR_LIMIT_NS = 2'000'000'000ULL;
constexpr std::chrono::milliseconds LONGEST_PAUSE{16};

std::uint64_t random_bits() {
    std::random_device device;
    return (static_cast<std::uint64_t>(device()) << 32U) | device();
}

/// True when `text` is made of hex digits only, and so is an id or the start
/// of one.
bool is_hex_prefix(dstd::StringView text, std::uint64_t& value) {
    value = 0;
    for (const char c : text) {
        std::uint64_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<std::uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<std::uint64_t>(c - 'a' + 10);
        } else {
            return false;
        }
        value = (value << 4U) | digit;
    }
    return true;
}

dstd::Path id_path(const dstd::Path& dir) { return dir / STORE_ID_FILE; }

/// Replaces store.id with a whole file in one rename, so no reader sees a
/// short one.
bool replace_store_id(const dstd::Path& dir, std::uint64_t id) {
    const dstd::String name =
        dstd::fmt<"{}.{:016x}.tmp">(STORE_ID_FILE, random_bits());
    const dstd::Path temp = dir / name;
    const dstd::String text = dstd::fmt<"{:016x}">(id);
    if (!dstd::fs::write_file(temp.view(), text).has_value()) return false;
    if (dstd::fs::rename(temp.view(), id_path(dir).view()).has_value()) {
        return true;
    }
    dstd::fs::remove(temp.view()).discard();
    return false;
}

}  // namespace

dstd::String marker_file_name(std::uint64_t id) {
    return dstd::fmt<"id-{:016x}.mark">(id);
}

bool path_is_gone(const dstd::Error& error) {
    return error.is(dstd::Condition::NotFound);
}

StoreIdRead read_store_id(const dstd::Path& dir) {
    const dstd::Path file = id_path(dir);
    const std::uint64_t started = dstd::steady_now_ns();
    std::chrono::milliseconds pause{1};
    for (;;) {
        const auto text = dstd::fs::read_file(file.view());
        if (!text) {
            return {path_is_gone(text.error()) ? IdFile::Missing
                                               : IdFile::Unreadable,
                    0};
        }
        std::uint64_t id = 0;
        const bool hex = is_hex_prefix(*text, id);
        if (hex && text->size() == STORE_ID_DIGITS) {
            return {IdFile::Found, id};
        }
        // Anything that is not the start of an id was not written by a
        // creator, and is replaced at once. A creator makes the file and then
        // writes it, so the start of an id is a write in progress.
        if (!hex || text->size() > STORE_ID_DIGITS) {
            return {IdFile::Invalid, 0};
        }
        if (dstd::steady_now_ns() - started > CREATOR_LIMIT_NS) {
            return {IdFile::Invalid, 0};
        }
        std::this_thread::sleep_for(pause);
        pause = std::min(pause * 2, LONGEST_PAUSE);
    }
}

bool ensure_marker_file(const dstd::Path& dir, std::uint64_t id) {
    const dstd::Path marker = dir / marker_file_name(id);
    if (dstd::fs::stat(marker.view())) return true;
    dstd::OpenOptions options;
    options.access = dstd::Access::Write;
    options.creation = dstd::Creation::OnlyIfNew;
    auto made = dstd::File::open(marker.view(), options);
    if (made) made->close().discard();
    return dstd::fs::stat(marker.view()).has_value();
}

std::uint64_t ensure_store_id(const dstd::Path& dir, IdFile* seen) {
    StoreIdRead found = read_store_id(dir);
    if (seen) *seen = found.state;
    if (found.state == IdFile::Missing) {
        const std::uint64_t mine = random_bits();
        const dstd::String text = dstd::fmt<"{:016x}">(mine);
        dstd::OpenOptions options;
        options.access = dstd::Access::Write;
        options.creation = dstd::Creation::OnlyIfNew;
        auto made = dstd::File::open(id_path(dir).view(), options);
        if (made) {
            const bool written =
                made->write_all_at(0, {text.data(), text.size()}).has_value();
            made->close().discard();
            if (written && ensure_marker_file(dir, mine)) return mine;
            return 0;
        }
        // Another process made it first, or this one cannot write.
        found = read_store_id(dir);
        if (seen) *seen = found.state;
    }
    if (found.state == IdFile::Invalid) {
        // The creator died before it wrote the id. Racing repairs each rename
        // a whole file, and every one of them reads the winner's.
        if (!replace_store_id(dir, random_bits())) return 0;
        found = read_store_id(dir);
        if (seen) *seen = found.state;
    }
    if (found.state == IdFile::Found && ensure_marker_file(dir, found.id)) {
        return found.id;
    }
    return 0;
}

std::uint64_t existing_store_id(const dstd::Path& dir, IdFile* seen) {
    const StoreIdRead found = read_store_id(dir);
    if (seen) *seen = found.state;
    if (found.state == IdFile::Found && ensure_marker_file(dir, found.id)) {
        return found.id;
    }
    return 0;
}

}  // namespace dftracer::utils::store::detail
