// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_MACROS_H
#define DFTRACER_UTILS_DUQL_MACROS_H

#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/variant.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/duql/syntax/tree.h>

#include <variant>

namespace dftracer::utils::duql {

/// `def name(params) = body`, from `origin` (a query, a source or a file).
struct Macro {
    dstd::String name;
    dstd::Vector<dstd::String> params;
    dstd::Variant<dstd::SharedPtr<const syntax::Expr>,
                  dstd::SharedPtr<const syntax::Pipeline>>
        body;
    dstd::String origin;
};

/// The macros a text can call, the first scope hiding the later ones.
using MacroScopes = dstd::Vector<dstd::Vector<Macro>>;

/// `defs` as one scope, their bodies moved out; the `args_fallback` flag is
/// not a macro and is left out. Fails for two macros of one name.
dftracer::utils::dstd::Expected<dstd::Vector<Macro>, DuqlError> macros_of(
    dstd::Vector<syntax::Def>& defs, dstd::StringView text,
    const dstd::String& origin);

/// Whether `defs` define `args_fallback = true`.
bool args_fallback(const dstd::Vector<syntax::Def>& defs);

/// The `def`s of `decls`, taken out of it.
dstd::Vector<syntax::Def> take_defs(dstd::Vector<syntax::Decl>& decls);

/// Replaces every call of a macro in `pipeline` and in the pipelines it holds
/// with its body, the arguments bound to the parameters by position. A
/// pipeline macro called as a stage splices its stages; a call in the first
/// position that names no pipeline macro becomes a `where` filter, with no
/// macro in scope too. `builtin` names the built-in functions, which a macro
/// may not take. Fails for a wrong number of arguments, a named argument, a
/// cycle, a macro with a built-in's name, a pipeline macro inside an
/// expression, an expression macro or an unknown name after the first stage;
/// errors point into `text`.
dftracer::utils::dstd::Expected<void, DuqlError> expand_macros(
    syntax::Pipeline& pipeline, const MacroScopes& scopes,
    dstd::StringView text,
    const dstd::Function<bool(dstd::StringView)>& builtin);

/// The macros of the `.duql` files on `$DFTRACER_UTILS_DUQL_PATH` (colon
/// separated) and every path `load_macros` added. Thread-safe. Fails like
/// @ref load_macros for a path on the variable.
dstd::Result<dstd::Vector<Macro>> path_macros();

/// Adds the `def`s of the `.duql` file `path`, or of every `.duql` file of a
/// directory in name order. A path is loaded once. Fails with
/// ErrorCode::InvalidArgument for a file that holds anything but `def`s or
/// does not parse, ErrorCode::Io when it cannot be read.
dstd::Result<void> load_macros(dstd::StringView path);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_MACROS_H
