#ifndef DFTRACER_UTILS_DUQL_MACROS_H
#define DFTRACER_UTILS_DUQL_MACROS_H

#include <dftracer/utils/core/common/expected.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/duql/syntax/tree.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::duql {

/// `def name(params) = body`, from `origin` (a query, a source or a file).
struct Macro {
    std::string name;
    std::vector<std::string> params;
    std::shared_ptr<const syntax::Expr> body;
    std::string origin;
};

/// The macros a text can call, the first scope hiding the later ones.
using MacroScopes = std::vector<std::vector<Macro>>;

/// `defs` as one scope, their bodies moved out; the `args_fallback` flag is
/// not a macro and is left out. Fails for two macros of one name.
dftracer::utils::expected<std::vector<Macro>, DuqlError> macros_of(
    std::vector<syntax::Def>& defs, std::string_view text,
    const std::string& origin);

/// Whether `defs` define `args_fallback = true`.
bool args_fallback(const std::vector<syntax::Def>& defs);

/// The `def`s of `decls`, taken out of it.
std::vector<syntax::Def> take_defs(std::vector<syntax::Decl>& decls);

/// Replaces every call of a macro in `pipeline` with its body, the arguments
/// bound to the parameters by position. `builtin` names the built-in
/// functions, which a macro may not take. Fails for a wrong number of
/// arguments, a named argument, a cycle or a macro with a built-in's name;
/// errors point into `text`.
dftracer::utils::expected<void, DuqlError> expand_macros(
    syntax::Pipeline& pipeline, const MacroScopes& scopes,
    std::string_view text,
    const std::function<bool(std::string_view)>& builtin);

/// The macros of the `.duql` files on `$DFTRACER_DUQL_PATH` (colon separated)
/// and every path `load_macros` added. Thread-safe.
std::vector<Macro> path_macros();

/// Adds the `def`s of the `.duql` file `path`, or of every `.duql` file of a
/// directory in name order. A path is loaded once. Throws DFTUtilsException
/// INVALID_ARGUMENT for a file that holds anything but `def`s or does not
/// parse, IO when it cannot be read.
void load_macros(const std::string& path);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_MACROS_H
