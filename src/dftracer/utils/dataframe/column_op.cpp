#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace {

using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::Series;

std::int64_t scalar_i64(const dftu_scalar& s) {
    switch (s.kind) {
        case DFTU_SCALAR_TAG_F64:
            return static_cast<std::int64_t>(s.value.d);
        case DFTU_SCALAR_TAG_U64:
            return static_cast<std::int64_t>(s.value.u);
        default:
            return s.value.i;
    }
}

double scalar_f64(const dftu_scalar& s) {
    switch (s.kind) {
        case DFTU_SCALAR_TAG_F64:
            return s.value.d;
        case DFTU_SCALAR_TAG_U64:
            return static_cast<double>(s.value.u);
        default:
            return static_cast<double>(s.value.i);
    }
}

// The op's operand slots, filled in signature order from the sources the
// frame op carries. False when a token has no source.
bool fill_args(const dftu_op_desc* op, const dftu_series* column2,
               const dftu_scalar* scalars, int n_scalars, const char* text,
               dftu_op_arg& arg, int& n_in) {
    int used_scalars = 0;
    bool used_text = false;
    n_in = static_cast<int>(dftu_op_arity(op->sig));
    for (std::uint32_t i = 0; i < DFTU_OP_MAX_ARGS; ++i) {
        const dftu_op_tok tok = DFTU_OP_SIG_ARG(op->sig, i);
        if (tok == DFTU_TOK_NONE) break;
        if (static_cast<int>(i) < n_in) continue;  // the in[] columns
        dftu_op_val& v = arg.args[i];
        switch (tok) {
            case DFTU_TOK_SERIES:
                if (!column2) return false;
                v.series = column2;
                break;
            case DFTU_TOK_I64:
            case DFTU_TOK_U64:
                if (used_scalars >= n_scalars) return false;
                v.i64 = scalar_i64(scalars[used_scalars++]);
                break;
            case DFTU_TOK_F64:
                if (used_scalars >= n_scalars) return false;
                v.f64 = scalar_f64(scalars[used_scalars++]);
                break;
            case DFTU_TOK_SCALAR:
                if (used_scalars >= n_scalars) return false;
                v.scalar = scalars[used_scalars++];
                break;
            case DFTU_TOK_I32:
            case DFTU_TOK_CMP:
            case DFTU_TOK_PRIM:
            case DFTU_TOK_LOGICAL:
            case DFTU_TOK_DTYPE:
            case DFTU_TOK_REDUCE:
            case DFTU_TOK_RANK:
            case DFTU_TOK_ROLLING:
                if (used_scalars >= n_scalars) return false;
                v.i32 = static_cast<std::int32_t>(
                    scalar_i64(scalars[used_scalars++]));
                break;
            case DFTU_TOK_STR:
                if (!text || used_text) return false;
                v.str.ptr = text;
                v.str.len = static_cast<std::int32_t>(std::strlen(text));
                used_text = true;
                break;
            case DFTU_TOK_CHAR:
                if (!text || used_text || !text[0]) return false;
                v.ch = text[0];
                used_text = true;
                break;
            default:
                return false;  // a list, a frame, an expression: not carried
        }
    }
    return true;
}

}  // namespace

extern "C" dftu_dataframe* dftu_dataframe_column_op(
    const dftu_dataframe* df, const char* column, const char* op_name,
    const char* column2, dftu_scalar a, dftu_scalar b, const char* text) {
    if (!df || !column || !op_name) return nullptr;
    const dftu_op_desc* op = dftu_op_find(op_name);
    if (!op || dftu_op_kind_of(op->sig) != DFTU_OP_KIND_SERIES ||
        DFTU_OP_SIG_ARG(op->sig, 0) != DFTU_TOK_SERIES)
        return nullptr;
    const DataFrame& frame =
        dftracer::utils::dataframe::dataframe_handle_view(df);
    std::int64_t ci = -1, c2 = -1;
    for (std::size_t i = 0; i < frame.names.size(); ++i) {
        if (frame.names[i] == column) ci = static_cast<std::int64_t>(i);
        if (column2 && column2[0] && frame.names[i] == column2)
            c2 = static_cast<std::int64_t>(i);
    }
    if (ci < 0 || (column2 && column2[0] && c2 < 0)) return nullptr;
    const dftu_series* in[2] = {
        frame.columns[static_cast<std::size_t>(ci)].handle(),
        c2 >= 0 ? frame.columns[static_cast<std::size_t>(c2)].handle()
                : nullptr};
    const dftu_scalar scalars[2] = {a, b};
    dftu_op_arg arg{};
    int n_in = 0;
    if (!fill_args(op, in[1], scalars, 2, text, arg, n_in)) return nullptr;
    if (n_in == 2 && !in[1]) return nullptr;
    if (n_in > 2) return nullptr;
    dftu_series* out =
        dftu_op_run(op, in, static_cast<std::uint32_t>(n_in), &arg);
    if (!out) return nullptr;
    if (dftu_series_length(out) != frame.num_rows()) {
        dftu_series_free(out);
        return nullptr;  // not a row-for-row column op (a sort of a subset)
    }
    DataFrame result;
    result.names = frame.names;
    result.columns.reserve(frame.columns.size());
    for (std::size_t i = 0; i < frame.columns.size(); ++i)
        result.columns.push_back(static_cast<std::int64_t>(i) == ci
                                     ? Series{out}
                                     : frame.columns[i].share());
    return dftracer::utils::dataframe::dataframe_handle_wrap(std::move(result));
}
