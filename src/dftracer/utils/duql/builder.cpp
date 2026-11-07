// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/duql/builder.h>
#include <dftracer/utils/duql/syntax/parser.h>
#include <dftracer/utils/duql/syntax/tree.h>

#include <cstddef>
#include <stdexcept>

namespace dftracer::utils::duql {

dstd::String Pipe::text() const {
    auto program = syntax::parse(raw());
    if (!program) throw std::invalid_argument(program.error().format().c_str());
    return syntax::to_text(*program);
}

dstd::String Source::text() const {
    if (members_.empty()) return {};
    dstd::String joined;
    for (const auto& m : members_) joined += m + ";\n";
    auto program = syntax::parse("source s {\n" + joined + "}");
    if (!program) throw std::invalid_argument(program.error().format().c_str());
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
