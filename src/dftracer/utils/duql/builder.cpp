#include <dftracer/utils/duql/builder.h>
#include <dftracer/utils/duql/syntax/parser.h>
#include <dftracer/utils/duql/syntax/tree.h>

#include <sstream>
#include <stdexcept>

namespace dftracer::utils::duql {

std::string Pipe::text() const {
    auto program = syntax::parse(raw());
    if (!program) throw std::invalid_argument(program.error().format());
    return syntax::to_text(*program);
}

std::string Source::text() const {
    if (members_.empty()) return {};
    std::string joined;
    for (const auto& m : members_) joined += m + ";\n";
    auto program = syntax::parse("source s {\n" + joined + "}");
    if (!program) throw std::invalid_argument(program.error().format());
    std::istringstream lines(syntax::to_text(*program));
    std::string line, out;
    std::getline(lines, line);
    std::getline(lines, line);
    while (std::getline(lines, line) && line != "}") {
        line = line.substr(2, line.size() - 3);
        out += (out.empty() ? "" : ";\n") + line;
    }
    return out;
}

}  // namespace dftracer::utils::duql
