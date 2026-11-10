// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/duql/builder.h>
#include <dftracer/utils/duql/syntax/parser.h>
#include <dftracer/utils/duql/syntax/tree.h>

#include <cstddef>

namespace dftracer::utils::duql {

namespace {

DuqlError input_error(const dstd::String& message) {
    DuqlError error;
    error.message = message;
    return error;
}

}  // namespace

dstd::Expected<dstd::String, DuqlError> Pipe::text() const {
    if (!error_.empty()) return dstd::Unexpected(input_error(error_));
    auto program = syntax::parse(raw());
    if (!program) return dstd::Unexpected(program.error());
    return syntax::to_text(*program);
}

dstd::Expected<dstd::String, DuqlError> Source::text() const {
    if (!error_.empty()) return dstd::Unexpected(input_error(error_));
    if (members_.empty()) return dstd::String();
    dstd::String joined;
    for (const auto& m : members_) joined += dstd::fmt<"{};\n">(m);
    auto program = syntax::parse(dstd::fmt<"source s {\n{}}">(joined));
    if (!program) return dstd::Unexpected(program.error());
    const dstd::String text = syntax::to_text(*program);
    std::size_t pos = 0;
    auto next_line = [&](dstd::StringView& line) {
        if (pos >= text.size()) return false;
        const std::size_t nl = text.find('\n', pos);
        const std::size_t end = nl == dstd::String::npos ? text.size() : nl;
        line = dstd::StringView(text).substr(pos, end - pos);
        pos = nl == dstd::String::npos ? text.size() : nl + 1;
        return true;
    };
    dstd::StringView line;
    dstd::String out;
    next_line(line);
    next_line(line);
    while (next_line(line) && line != "}") {
        line = line.substr(2, line.size() - 3);
        if (!out.empty()) out += ";\n";
        out += line;
    }
    return out;
}

}  // namespace dftracer::utils::duql
