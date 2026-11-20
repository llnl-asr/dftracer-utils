// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CLI_PLUGIN_GOLDEN_H
#define DFTRACER_UTILS_CLI_PLUGIN_GOLDEN_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdio>

// The golden-test format of `dftracer_plugin check`: a plain text file of
// `input`, `query` and expected-output blocks, in the style of sqllogictest.
//
//   # comment
//   input data/sample.pfw.gz
//   query rowsort
//   select name, dur
//   ----
//   name<TAB>dur
//   open<TAB>300
//
// `<TAB>` is a literal tab. `input` names a trace file, a trace folder or a
// plugin-format file, relative to the .test file, and applies to every block
// after it. `query rowsort` sorts the rows before the compare; `query error
// <text>` expects the query to fail with an error whose message contains
// <text>. Several blocks may share one file, and a blank line ends the
// expected-output block.
namespace dftracer::utils::dataframe {
struct DataFrame;
}

namespace dftracer::utils::plugins::host {
class Plugins;
}

namespace dftracer::utils::cli {

/// One `query` block: its leading comment and input lines, the query and the
/// expected output.
struct GoldenBlock {
    /// Comment, `input` and blank lines written before this block, so a
    /// rewrite keeps them.
    dstd::String lead;
    /// The `query ...` line exactly as written.
    dstd::String head;
    /// The duql text, one or more lines, no trailing newline.
    dstd::String query;
    /// The expected output lines, each ending in a newline; empty when the
    /// block expects none.
    dstd::String expected;
    /// The `input` path in effect for this block, relative to the .test file;
    /// empty when no `input` line precedes it.
    dstd::String input;
    bool rowsort = false;
    /// Non-empty when the block expects a failure whose message holds this.
    dstd::String error_text;
    /// 1-based line number of the `query` line, for the failure message.
    std::size_t line = 0;
};

struct GoldenFile {
    dstd::String path;
    dstd::Vector<GoldenBlock> blocks;
    /// Comment and blank lines after the last block, so a rewrite keeps them.
    dstd::String trailing;
};

/// Parse @p text, which came from @p path (used in errors only). Every error
/// names the line it was found on.
dstd::Expected<GoldenFile, dstd::Error> parse_golden_file(
    dstd::StringView text, dstd::StringView path);

/// The file as text again, with each block's expected output replaced by
/// `actuals[i]`; a block whose `actuals` entry is empty keeps what it had.
dstd::String render_golden_file(const GoldenFile& file,
                                dstd::Span<const dstd::String> actuals);

/// The one-line-per-row text of a table: the column names, tab separated, then
/// one row per line with the same separators.
dstd::String table_text(const dataframe::DataFrame& table);

/// Run one block's query over its input with the loaded plugins in effect and
/// give the text @ref table_text produces, or the error the query ended in.
/// `index_dir` holds the index the run builds, so no `.dftindex` lands beside
/// the user's inputs. When `folds` is set, every plugin it holds also runs as a
/// fold over a trace input, so a fold plugin is exercised too.
dstd::Result<dstd::String> run_golden_query(
    const GoldenBlock& block, dstd::StringView base_dir,
    dstd::StringView index_dir, const plugins::host::Plugins* folds);

/// One block's outcome.
struct GoldenOutcome {
    std::size_t line = 0;
    bool ok = false;
    /// What the query gave, or the error message it failed with.
    dstd::String actual;
};

/// The outcome of one .test file.
struct GoldenFileOutcome {
    dstd::String path;
    dstd::Vector<GoldenOutcome> blocks;
    bool ok() const;
};

/// Run every block of @p file. With @p record, write each block's actual
/// output back into the file instead of comparing; a block whose query failed
/// keeps its expected output and stays a failure.
dstd::Result<GoldenFileOutcome> run_golden_file(
    const GoldenFile& file, dstd::StringView base_dir,
    dstd::StringView index_dir, bool record,
    const plugins::host::Plugins* folds);

/// Every `*.test` file under @p dir, sorted, with the directory walked.
dstd::Result<dstd::Vector<dstd::String>> golden_test_files(
    dstd::StringView dir);

}  // namespace dftracer::utils::cli

#endif  // DFTRACER_UTILS_CLI_PLUGIN_GOLDEN_H
