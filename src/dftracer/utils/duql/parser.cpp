#include <dftracer/utils/duql/errc.h>
#include <dftracer/utils/duql/lower.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/duql/syntax/parser.h>

#include <sstream>

namespace dftracer::utils::duql {

std::string DuqlError::format() const {
    std::ostringstream os;
    os << "duql parse error at line " << line << ", column " << column << ":\n";
    os << "  " << source << '\n';
    os << "  " << indicator << '\n';
    os << "  " << message << '\n';
    return os.str();
}

DuqlParseError::DuqlParseError(DuqlError err)
    : DFTUtilsException(
          dftracer::utils::make_error(DuqlErrc::Parse, err.format())),
      err_(std::move(err)) {}

dftracer::utils::expected<QueryNodePtr, DuqlError> parse(std::string_view input,
                                                         const Params& params) {
    auto tree = duql::syntax::parse(input);
    if (!tree) return dftracer::utils::unexpected(tree.error());
    return duql::lower_filter(std::move(*tree), params, input);
}

dftracer::utils::expected<QueryNodePtr, DuqlError> parse(
    std::string_view input) {
    return parse(input, Params{});
}

}  // namespace dftracer::utils::duql
