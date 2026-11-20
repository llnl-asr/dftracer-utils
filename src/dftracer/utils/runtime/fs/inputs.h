// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_FS_INPUTS_H
#define DFTRACER_UTILS_RUNTIME_FS_INPUTS_H

#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

namespace dftracer::utils::runtime::fs {

/// The files of @ref expand_inputs.
struct ExpandedInputs {
    /// The files, in the order of the inputs, each once. A path that does not
    /// exist and has no wildcard stays here, as the user wrote it.
    dstd::Vector<dstd::String> files;
    /// The paths of @ref files that do not exist.
    dstd::Vector<dstd::String> missing;
};

/// Turns the input paths of a view or an indexer into trace files.
///
/// An input that exists is taken as it is, even when its name has a wildcard.
/// A directory is scanned recursively for the files that
/// `common::constants::indexer::INPUT_FILE_PATTERNS` names, skipping
/// `SCAN_SKIP_DIRS`. A file is taken whatever its name. An input that does not
/// exist and has `*`, `?` or `[` is a glob: its parts are matched with
/// @ref dstd::fs::glob_match_name, and a part `**` is zero or more
/// directories. A matched directory is scanned like one named by the user, a
/// matched file is taken, and `**` does not enter a symlink to a directory or
/// a directory of SCAN_SKIP_DIRS. A glob that matches nothing adds nothing. The
/// walk lists only the directories the pattern can still match in, and leaves
/// out one it cannot read, as a scan does.
///
/// Within one directory or glob the files are sorted by path. Between inputs
/// the order is that of @p inputs. With two or more inputs a file is kept once,
/// by device and inode: the first one wins, so a symlink and a hard link name
/// their file once. The stats of the files an input named come from the stat
/// that told the input's kind; the others are stat'ed in one batch. With one
/// input nothing is stat'ed more, so a link inside one scan or glob is listed
/// as often as it is named. A folder or file named like a staging path of a
/// table writer (`.<name>.tmp-<id>`, `.<name>.old-<id>`) is left out of a scan
/// or a glob, and so is a folder of index data. Work runs as children of
/// @p scope. @p inputs must stay valid until the task ends.
coro::CoroTask<dstd::Expected<ExpandedInputs, dstd::Error>> expand_inputs(
    coro::Scope& scope, dstd::Span<const dstd::String> inputs);

}  // namespace dftracer::utils::runtime::fs

#endif  // DFTRACER_UTILS_RUNTIME_FS_INPUTS_H
