// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/duql/errc.h>
#include <dftracer/utils/duql/lower.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/duql/syntax/parser.h>

namespace dftracer::utils::duql {

dstd::String DuqlError::format() const {
    dstd::String out = "duql parse error at line ";
    out += dstd::int_text(line);
    out += ", column ";
    out += dstd::int_text(column);
    out += ":\n  ";
    out += source;
    out += "\n  ";
    out += indicator;
    out += "\n  ";
    out += message;
    out += '\n';
    return out;
}

DuqlParseError::DuqlParseError(DuqlError err)
    : common::DFTUtilsException(
          dftracer::utils::dstd::Error{DuqlErrc::Parse, err.format()}),
      err_(std::move(err)) {}

dftracer::utils::dstd::Expected<detail::QueryNodePtr, DuqlError> parse(
    dstd::StringView input, const Params& params) {
    auto tree = duql::syntax::parse(input);
    if (!tree) return dftracer::utils::dstd::Unexpected(tree.error());
    return duql::lower_filter(std::move(*tree), params, input);
}

dftracer::utils::dstd::Expected<detail::QueryNodePtr, DuqlError> parse(
    dstd::StringView input) {
    return parse(input, Params{});
}

}  // namespace dftracer::utils::duql
