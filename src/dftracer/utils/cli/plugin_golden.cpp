// SPDX-License-Identifier: MIT
#include <dftracer/utils/cli/common_cli.h>
#include <dftracer/utils/cli/json_cell_printer.h>
#include <dftracer/utils/cli/plugin_golden.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/trace/index/build/resolve_and_build.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace dftracer::utils;

namespace dftracer::utils::cli {
namespace {

constexpr dstd::StringView INPUT_WORD = "input";
constexpr dstd::StringView QUERY_WORD = "query";
constexpr dstd::StringView SEPARATOR = "----";
constexpr dstd::StringView TEST_SUFFIX = ".test";

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

dstd::StringView trim(dstd::StringView text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_space(text[begin])) ++begin;
    while (end > begin && is_space(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

dstd::StringView first_word(dstd::StringView line, dstd::StringView* rest) {
    const std::size_t at = line.find_first_of(" \t");
    if (at == dstd::StringView::npos) {
        *rest = {};
        return line;
    }
    *rest = trim(line.substr(at + 1));
    return line.substr(0, at);
}

dstd::Error parse_error(dstd::StringView path, std::size_t line,
                        dstd::StringView what) {
    return dstd::Error::from_format<"{}:{}: {}">(
        common::ErrorCode::InvalidArgument, path, line, what);
}

/// One line of `text` starting at `at`, and the offset just past its newline.
dstd::StringView next_line(dstd::StringView text, std::size_t& at) {
    if (at >= text.size()) return {};
    const std::size_t nl = text.find('\n', at);
    const std::size_t end = nl == dstd::StringView::npos ? text.size() : nl;
    const dstd::StringView line = text.substr(at, end - at);
    at = nl == dstd::StringView::npos ? text.size() : nl + 1;
    return line;
}

/// The line without its terminator: the `\n` is already gone and a `\r`
/// before it goes too. Nothing else is removed, so a value that starts or ends
/// with a space, or an empty last cell, reads back as it was written.
dstd::StringView strip_eol(dstd::StringView line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return line;
}

dstd::Vector<dstd::StringView> split_lines(dstd::StringView text) {
    dstd::Vector<dstd::StringView> lines;
    std::size_t at = 0;
    while (at < text.size()) {
        const dstd::StringView line = next_line(text, at);
        lines.push_back(line);
    }
    return lines;
}

/// The `input` path a line declares, or an empty view when it declares none.
bool is_trace_input(dstd::StringView path) {
    if (path.ends_with(".pfw") || path.ends_with(".pfw.gz") ||
        path.ends_with(".jsonl") || path.ends_with(".ndjson") ||
        path.ends_with(".trace"))
        return true;
    const auto is_dir = dstd::fs::is_directory(dstd::PathView(path));
    return is_dir.has_value() && *is_dir;
}

dstd::String absolute_from(dstd::StringView base_dir, dstd::StringView path) {
    dstd::String joined;
    if (path.starts_with('/')) {
        joined.assign(path.data(), path.size());
    } else {
        joined.assign(base_dir.data(), base_dir.size());
        joined += '/';
        joined.append(path.data(), path.size());
    }
    // `..` and `.` resolved: the trace reader stats this path itself and does
    // not normalize it.
    auto absolute = dstd::fs::weakly_canonical(dstd::PathView(joined));
    if (!absolute) return joined;
    return dstd::String(absolute->str());
}

/// The columns a plugin-format query asks for: `select a, b`, the one stage a
/// non-trace input supports.
dstd::Result<dstd::Vector<dstd::String>> select_stage(dstd::StringView query) {
    const dstd::StringView text = trim(query);
    dstd::StringView rest;
    const dstd::StringView word = first_word(text, &rest);
    if (word != "select")
        return dstd::Error::from_format<
            "a plugin-format input reads `select <columns>` only, not '{}'">(
            common::ErrorCode::InvalidArgument, text);
    dstd::Vector<dstd::String> names;
    for (std::size_t at = 0; at <= rest.size();) {
        const std::size_t comma = rest.find(',', at);
        const dstd::StringView name = trim(rest.substr(
            at, comma == dstd::StringView::npos ? dstd::StringView::npos
                                                : comma - at));
        if (name.empty())
            return dstd::Error::from_format<
                "a `select` stage names an empty column: '{}'">(
                common::ErrorCode::InvalidArgument, text);
        names.emplace_back(name);
        if (comma == dstd::StringView::npos) break;
        at = comma + 1;
    }
    return names;
}

/// A double as one tab-separated field. A whole value prints as an integer,
/// exactly as the JSON cell printer prints a float cell; every other value
/// prints in the shortest form that reads back as the same double ("91.5",
/// "0.1234567"), where the JSON printer falls back to dstd::fixed_text and
/// would print "0.123457". A golden must survive a round trip through its own
/// text, and six fixed decimals round a value and lose it.
dstd::String double_text(double v) {
    constexpr double INT64_LOW = -9223372036854775808.0;
    constexpr double INT64_HIGH = 9223372036854775808.0;
    if (v > INT64_LOW && v < INT64_HIGH && v == std::trunc(v)) {
        return dstd::int_text(static_cast<std::int64_t>(v));
    }
    return dstd::double_text(v);
}

/// One cell as one tab-separated field. A string keeps its own text, with a
/// tab, a newline and a backslash escaped so a row stays on one line; every
/// other type prints as its JSON value (numbers bare, null as `null`).
dstd::String cell_text(const dataframe::Series& column, std::int64_t row) {
    dstd::String out;
    switch (column.type()) {
        case dataframe::TypeId::Float16:
        case dataframe::TypeId::Float32:
        case dataframe::TypeId::Float64:
            if (column.is_null(row)) {
                out = "null";
                break;
            }
            out = double_text(dataframe::read_f64(column, row));
            break;
        case dataframe::TypeId::String:
        case dataframe::TypeId::Binary:
        case dataframe::TypeId::LargeString:
        case dataframe::TypeId::LargeBinary:
        case dataframe::TypeId::FixedSizeBinary: {
            if (column.is_null(row)) {
                out = "null";
                break;
            }
            const dstd::StringView bytes = dataframe::read_bytes(column, row);
            for (const char c : bytes) {
                if (c == '\\') {
                    out += "\\\\";
                } else if (c == '\t') {
                    out += "\\t";
                } else if (c == '\n') {
                    out += "\\n";
                } else if (c == '\r') {
                    out += "\\r";
                } else {
                    out += c;
                }
            }
            break;
        }
        default:
            append_cell_json(out, column, row);
            break;
    }
    return out;
}

}  // namespace

dstd::String table_text(const dataframe::DataFrame& table) {
    dstd::String out;
    for (std::size_t c = 0; c < table.columns.size(); ++c) {
        if (c > 0) out += '\t';
        out.append(table.names[c].data(), table.names[c].size());
    }
    out += '\n';
    const std::int64_t rows = table.num_rows();
    for (std::int64_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < table.columns.size(); ++c) {
            if (c > 0) out += '\t';
            out += cell_text(table.columns[c], r);
        }
        out += '\n';
    }
    return out;
}

dstd::Expected<GoldenFile, dstd::Error> parse_golden_file(
    dstd::StringView text, dstd::StringView path) {
    GoldenFile file;
    file.path.assign(path.data(), path.size());
    dstd::String lead;
    dstd::String input;
    const dstd::Vector<dstd::StringView> lines = split_lines(text);
    std::size_t at = 0;
    while (at < lines.size()) {
        const dstd::StringView line = trim(lines[at]);
        const std::size_t line_no = at + 1;
        if (line.empty() || line.starts_with("#")) {
            lead.append(lines[at].data(), lines[at].size());
            lead += '\n';
            ++at;
            continue;
        }
        dstd::StringView rest;
        const dstd::StringView word = first_word(line, &rest);
        if (word == INPUT_WORD) {
            if (rest.empty())
                return dstd::Unexpected(
                    parse_error(path, line_no, "`input` names no path"));
            input.assign(rest.data(), rest.size());
            lead.append(lines[at].data(), lines[at].size());
            lead += '\n';
            ++at;
            continue;
        }
        if (word != QUERY_WORD)
            return dstd::Unexpected(parse_error(
                path, line_no,
                dstd::fmt<"expected `input`, `query` or a comment, not '{}'">(
                    line)));
        GoldenBlock block;
        block.lead = lead;
        lead.clear();
        block.head.assign(strip_eol(lines[at]).data(),
                          strip_eol(lines[at]).size());
        block.input = input;
        block.line = line_no;
        if (rest == "rowsort") {
            block.rowsort = true;
        } else if (rest.starts_with("error")) {
            const dstd::StringView wanted = trim(rest.substr(5));
            if (wanted.empty())
                return dstd::Unexpected(parse_error(
                    path, line_no, "`query error` names no message text"));
            block.error_text.assign(wanted.data(), wanted.size());
        } else if (!rest.empty()) {
            return dstd::Unexpected(parse_error(
                path, line_no, dstd::fmt<"unknown query modifier '{}'">(rest)));
        }
        ++at;
        dstd::String query;
        while (at < lines.size() && trim(lines[at]) != SEPARATOR) {
            if (!query.empty()) query += '\n';
            query.append(lines[at].data(), lines[at].size());
            ++at;
        }
        if (at >= lines.size())
            return dstd::Unexpected(parse_error(
                path, line_no,
                dstd::fmt<"the query block has no '{}' line">(SEPARATOR)));
        block.query = trim(query);
        if (block.query.empty())
            return dstd::Unexpected(
                parse_error(path, line_no, "the query block is empty"));
        ++at;
        dstd::String expected;
        while (at < lines.size() && !trim(lines[at]).empty()) {
            const dstd::StringView row = strip_eol(lines[at]);
            expected.append(row.data(), row.size());
            expected += '\n';
            ++at;
        }
        block.expected = expected;
        if (block.input.empty())
            return dstd::Unexpected(parse_error(
                path, line_no, "no input line before this query block"));
        file.blocks.push_back(std::move(block));
    }
    file.trailing = lead;
    return file;
}

dstd::String render_golden_file(const GoldenFile& file,
                                dstd::Span<const dstd::String> actuals) {
    dstd::String out;
    for (std::size_t i = 0; i < file.blocks.size(); ++i) {
        const GoldenBlock& block = file.blocks[i];
        out += block.lead;
        out += block.head;
        out += '\n';
        out += block.query;
        out += '\n';
        out += SEPARATOR;
        out += '\n';
        const bool replace = i < actuals.size() && !actuals[i].empty() &&
                             block.error_text.empty();
        out += replace ? actuals[i] : block.expected;
        // The blank line that ended the previous block is already the lead of
        // this one; add a separator only when the file had none.
        if (i + 1 < file.blocks.size() &&
            !file.blocks[i + 1].lead.starts_with("\n"))
            out += '\n';
        if (i + 1 == file.blocks.size()) out += file.trailing;
    }
    return out;
}

dstd::Result<dstd::String> run_golden_query(
    const GoldenBlock& block, dstd::StringView base_dir,
    dstd::StringView index_dir, const plugins::host::Plugins* folds) {
    const dstd::String input = absolute_from(base_dir, block.input);
    const dstd::String index_root(index_dir);
    const dstd::String query(block.query);
    const bool trace_input = is_trace_input(dstd::StringView(input));
    if (trace_input) {
        return run_on_pool(
            "dftracer_plugin golden", RuntimeArgs{},
            [input, index_root, query, folds](
                coro::Scope&) -> coro::CoroTask<dstd::Result<dstd::String>> {
                auto fresh = co_await trace::index::build::ensure_index_fresh(
                    dstd::String(), input, index_root);
                if (!fresh.has_value()) co_return fresh.error();
                // The view reads the index this run built, not the default
                // `.dftindex` beside the user's trace, so it builds none there.
                const dstd::String index_path =
                    trace::internal::determine_index_path(input, index_root);
                auto is_dir = dstd::fs::is_directory(dstd::PathView(input));
                if (!is_dir.has_value()) co_return is_dir.error();
                const bool directory_input = *is_dir;
                trace::views::View view;
                if (directory_input) {
                    auto opened =
                        trace::views::View::from_directory(input, index_path);
                    view = co_await opened;
                } else {
                    view = trace::views::View::from_file(input, index_path);
                }
                if (folds != nullptr) {
                    auto folded = co_await folds->run(view);
                    if (!folded.has_value()) co_return folded.error();
                }
                view = view.duql(query);
                auto collected = co_await view.collect();
                if (!collected.has_value()) co_return collected.error();
                co_return table_text(*collected);
            });
    }
    auto columns = select_stage(dstd::StringView(query));
    if (!columns.has_value())
        return dstd::Unexpected(std::move(columns).error());
    dstd::Vector<dstd::String> names = std::move(*columns);
    return run_on_pool(
        "dftracer_plugin golden", RuntimeArgs{},
        [input, names = std::move(names)](
            coro::Scope&) -> coro::CoroTask<dstd::Result<dstd::String>> {
            dataframe::LazyFrame frame =
                dataframe::LazyFrame::scan_file(dstd::StringView(input));
            frame = frame.select(std::move(names));
            auto collected = co_await frame.collect();
            if (!collected.has_value()) co_return collected.error();
            co_return table_text(*collected);
        });
}

bool GoldenFileOutcome::ok() const {
    for (const GoldenOutcome& block : blocks)
        if (!block.ok) return false;
    return true;
}

namespace {

/// The rows of `actual`, sorted line by line, so `rowsort` compares sets. The
/// header line stays first.
dstd::String sorted_rows(dstd::StringView actual) {
    const std::size_t nl = actual.find('\n');
    if (nl == dstd::StringView::npos) return dstd::String(actual);
    dstd::String out(actual.substr(0, nl + 1));
    dstd::Vector<dstd::String> rows;
    std::size_t at = nl + 1;
    while (at < actual.size()) {
        const std::size_t end = actual.find('\n', at);
        rows.emplace_back(dstd::String(
            actual.substr(at, end == dstd::StringView::npos ? actual.size() - at
                                                            : end - at)));
        at = end == dstd::StringView::npos ? actual.size() : end + 1;
    }
    std::sort(rows.begin(), rows.end());
    for (const dstd::String& row : rows) {
        out += row;
        out += '\n';
    }
    return out;
}

}  // namespace

dstd::Result<GoldenFileOutcome> run_golden_file(
    const GoldenFile& file, dstd::StringView base_dir,
    dstd::StringView index_dir, bool record,
    const plugins::host::Plugins* folds) {
    GoldenFileOutcome out;
    out.path = file.path;
    dstd::Vector<dstd::String> actuals;
    actuals.reserve(file.blocks.size());
    for (const GoldenBlock& block : file.blocks) {
        GoldenOutcome outcome;
        outcome.line = block.line;
        auto ran = run_golden_query(block, base_dir, index_dir, folds);
        if (!block.error_text.empty()) {
            outcome.ok = !ran;
            if (!ran) {
                outcome.actual = ran.error().message();
                outcome.ok = dstd::StringView(outcome.actual)
                                 .find(dstd::StringView(block.error_text)) !=
                             dstd::StringView::npos;
            }
            actuals.emplace_back();
            out.blocks.push_back(std::move(outcome));
            continue;
        }
        if (!ran) {
            outcome.actual = ran.error().message();
            actuals.emplace_back();
            out.blocks.push_back(std::move(outcome));
            continue;
        }
        outcome.actual = std::move(*ran);
        actuals.push_back(block.rowsort ? sorted_rows(outcome.actual)
                                        : outcome.actual);
        const dstd::String& want =
            block.rowsort ? sorted_rows(block.expected) : block.expected;
        // A record run rewrites this block from what the query gave back, so it
        // matches by construction. A block whose query failed never reaches
        // here: it keeps its expected output and fails the run.
        outcome.ok = record || want == actuals.back();
        out.blocks.push_back(std::move(outcome));
    }
    if (record) {
        const dstd::String text = render_golden_file(file, actuals);
        auto written = dstd::fs::write_file(dstd::PathView(file.path), text);
        if (!written) return written.error();
    }
    return out;
}

dstd::Result<dstd::Vector<dstd::String>> golden_test_files(
    dstd::StringView dir) {
    dstd::Vector<dstd::String> files;
    auto exists = dstd::fs::exists(dstd::PathView(dir));
    if (!exists.has_value()) return exists.error();
    if (!*exists)
        return dstd::Error::from_format<"no such golden-test folder: {}">(
            common::ErrorCode::NotFound, dir);
    auto walked =
        dstd::fs::walk(dstd::PathView(dir),
                       [&files](dstd::PathView path, dstd::fs::FileKind kind) {
                           if (kind != dstd::fs::FileKind::File)
                               return dstd::fs::WalkAction::Continue;
                           const dstd::StringView leaf = path.str();
                           if (!leaf.ends_with(TEST_SUFFIX))
                               return dstd::fs::WalkAction::Continue;
                           files.emplace_back(leaf.data(), leaf.size());
                           return dstd::fs::WalkAction::Continue;
                       });
    if (!walked.has_value()) return walked.error();
    std::sort(files.begin(), files.end());
    return files;
}

}  // namespace dftracer::utils::cli
