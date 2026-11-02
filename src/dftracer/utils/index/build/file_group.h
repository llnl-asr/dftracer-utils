// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_BUILD_FILE_GROUP_H
#define DFTRACER_UTILS_INDEX_BUILD_FILE_GROUP_H

#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/runtime/coro/task.h>

#include <cstddef>
#include <string>

namespace dftracer::utils::index::build {

/// A file whose group publish failed.
struct GroupFailure {
    std::size_t tag;
    std::string path;
    std::string message;
};

/// What a group published since its last commit().
struct GroupResult {
    dstd::Vector<GroupFailure> failures;
    /// Files that published, and publish records written.
    std::size_t published = 0;
    std::size_t publishes = 0;
};

/// The filled builds one worker publishes together. Not thread safe. Every
/// build must come from one Index.
class FileGroup {
   public:
    /// Files a group holds before the worker publishes it.
    static constexpr std::size_t MAX_FILES = 128;

    bool full() const { return builds_.size() >= MAX_FILES; }

    /// Moves the puts of `build` into the store, so the group holds no data,
    /// and adds it. `path` is the logical path the build is named by. One the
    /// group already holds publishes the group first, so the later build
    /// replaces the earlier one. Returns the staging error, empty on success.
    coro::CoroTask<std::string> add(FileBuild build, std::size_t tag,
                                    std::string path);

    /// Publishes the group in one record and empties it. The result also
    /// covers the publishes `add` made since the last commit().
    coro::CoroTask<GroupResult> commit();

    std::size_t size() const { return builds_.size(); }

   private:
    struct Entry {
        std::size_t tag;
        std::string path;
    };

    coro::CoroTask<void> publish();

    GroupResult result_;
    dstd::Vector<FileBuild> builds_;
    dstd::Vector<Entry> entries_;
};

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_FILE_GROUP_H
