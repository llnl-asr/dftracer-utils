#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/python/schema_reconcile.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace dftracer::utils::python {

namespace {

bool cstr_eq(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return std::strcmp(a, b) == 0;
}

// Unknown formats fall back to NA so we can still emit a safe null column.
ArrowType type_from_format(const ArrowSchema *s) {
    if (!s || !s->format) return NANOARROW_TYPE_NA;
    const char *f = s->format;
    if (cstr_eq(f, "n")) return NANOARROW_TYPE_NA;
    if (cstr_eq(f, "b")) return NANOARROW_TYPE_BOOL;
    if (cstr_eq(f, "c")) return NANOARROW_TYPE_INT8;
    if (cstr_eq(f, "s")) return NANOARROW_TYPE_INT16;
    if (cstr_eq(f, "i")) return NANOARROW_TYPE_INT32;
    if (cstr_eq(f, "l")) return NANOARROW_TYPE_INT64;
    if (cstr_eq(f, "C")) return NANOARROW_TYPE_UINT8;
    if (cstr_eq(f, "S")) return NANOARROW_TYPE_UINT16;
    if (cstr_eq(f, "I")) return NANOARROW_TYPE_UINT32;
    if (cstr_eq(f, "L")) return NANOARROW_TYPE_UINT64;
    if (cstr_eq(f, "f")) return NANOARROW_TYPE_FLOAT;
    if (cstr_eq(f, "g")) return NANOARROW_TYPE_DOUBLE;
    if (cstr_eq(f, "u")) return NANOARROW_TYPE_STRING;
    if (cstr_eq(f, "z")) return NANOARROW_TYPE_BINARY;
    if (cstr_eq(f, "U")) return NANOARROW_TYPE_LARGE_STRING;
    if (cstr_eq(f, "Z")) return NANOARROW_TYPE_LARGE_BINARY;
    return NANOARROW_TYPE_NA;
}

int build_null_array(const ArrowSchema *child_schema, int64_t length,
                     ArrowArray *out) {
    ArrowError err;
    ArrowErrorInit(&err);
    ArrowType t = type_from_format(child_schema);
    if (ArrowArrayInitFromType(out, t) != NANOARROW_OK) return -1;
    if (ArrowArrayStartAppending(out) != NANOARROW_OK) return -1;
    if (ArrowArrayAppendNull(out, length) != NANOARROW_OK) return -1;
    if (ArrowArrayFinishBuildingDefault(out, &err) != NANOARROW_OK) return -1;
    return 0;
}

void json_escape(std::string_view in, std::string &out) {
    for (char c : in) {
        switch (c) {
            case '"':
                out.append("\\\"");
                break;
            case '\\':
                out.append("\\\\");
                break;
            case '\n':
                out.append("\\n");
                break;
            case '\r':
                out.append("\\r");
                break;
            case '\t':
                out.append("\\t");
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(
                        buf, sizeof(buf), "\\u%04x",
                        static_cast<int>(static_cast<unsigned char>(c)));
                    out.append(buf);
                } else {
                    out.push_back(c);
                }
        }
    }
}

// `view` is prepared once per column (init + SetArray) by the caller; a null
// view signals a column that failed to bind and always emits JSON null.
void append_json_scalar(const ArrowArrayView *view, ArrowType t, int64_t row,
                        std::string &out) {
    if (!view || ArrowArrayViewIsNull(view, row)) {
        out.append("null");
        return;
    }
    switch (t) {
        case NANOARROW_TYPE_BOOL:
            out.append(ArrowArrayViewGetIntUnsafe(view, row) ? "true"
                                                             : "false");
            break;
        case NANOARROW_TYPE_INT8:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT64: {
            char buf[32];
            std::snprintf(
                buf, sizeof(buf), "%lld",
                static_cast<long long>(ArrowArrayViewGetIntUnsafe(view, row)));
            out.append(buf);
            break;
        }
        case NANOARROW_TYPE_UINT8:
        case NANOARROW_TYPE_UINT16:
        case NANOARROW_TYPE_UINT32:
        case NANOARROW_TYPE_UINT64: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%llu",
                          static_cast<unsigned long long>(
                              ArrowArrayViewGetUIntUnsafe(view, row)));
            out.append(buf);
            break;
        }
        case NANOARROW_TYPE_FLOAT:
        case NANOARROW_TYPE_DOUBLE: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g",
                          ArrowArrayViewGetDoubleUnsafe(view, row));
            out.append(buf);
            break;
        }
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING: {
            auto sv = ArrowArrayViewGetStringUnsafe(view, row);
            out.push_back('"');
            json_escape(std::string_view(sv.data, sv.size_bytes), out);
            out.push_back('"');
            break;
        }
        default:
            out.append("null");
    }
}

}  // namespace

SchemaReconciler::SchemaReconciler() = default;

bool SchemaReconciler::merge(const ArrowSchema *incoming) {
    if (finalized_ || !incoming) return false;
    bool added = false;
    for (int64_t i = 0; i < incoming->n_children; ++i) {
        const ArrowSchema *child = incoming->children[i];
        if (!child || !child->name) continue;
        std::string name(child->name);
        if (name == EXTRA_COLUMN_NAME) continue;  // reserved
        if (name_to_idx_.count(name)) continue;
        nanoarrow::UniqueSchema copy;
        if (ArrowSchemaDeepCopy(child, copy.get()) != NANOARROW_OK) {
            last_error_ = "schema deep-copy failed while merging";
            return added;
        }
        int64_t idx = static_cast<int64_t>(names_.size());
        names_.push_back(name);
        child_schemas_.push_back(std::move(copy));
        name_to_idx_.emplace(std::move(name), idx);
        added = true;
    }
    return added;
}

int SchemaReconciler::finalize() {
    if (finalized_) return 0;
    int64_t n = static_cast<int64_t>(child_schemas_.size()) + 1;
    ArrowSchemaInit(locked_schema_.get());
    if (ArrowSchemaSetTypeStruct(locked_schema_.get(), n) != NANOARROW_OK) {
        last_error_ = "failed to initialize union struct schema";
        return -1;
    }
    for (size_t i = 0; i < child_schemas_.size(); ++i) {
        nanoarrow::UniqueSchema tmp;
        if (ArrowSchemaDeepCopy(child_schemas_[i].get(), tmp.get()) !=
            NANOARROW_OK) {
            last_error_ = "failed to deep-copy union child";
            return -1;
        }
        ArrowSchemaMove(tmp.get(), locked_schema_->children[i]);
    }
    ArrowSchema *extra = locked_schema_->children[child_schemas_.size()];
    if (ArrowSchemaSetType(extra, NANOARROW_TYPE_STRING) != NANOARROW_OK) {
        last_error_ = "failed to set _extra column type";
        return -1;
    }
    if (ArrowSchemaSetName(extra, EXTRA_COLUMN_NAME) != NANOARROW_OK) {
        last_error_ = "failed to name _extra column";
        return -1;
    }
    finalized_ = true;
    return 0;
}

int SchemaReconciler::copy_schema(ArrowSchema *out) const {
    if (!finalized_) {
        last_error_ = "copy_schema called before finalize";
        return -1;
    }
    nanoarrow::UniqueSchema tmp;
    if (ArrowSchemaDeepCopy(locked_schema_.get(), tmp.get()) != NANOARROW_OK) {
        last_error_ = "failed to deep-copy locked schema";
        return -1;
    }
    ArrowSchemaMove(tmp.get(), out);
    return 0;
}

int SchemaReconciler::reconcile(const ArrowSchema *in_schema,
                                ArrowArray *in_array, ArrowArray *out) const {
    if (!finalized_) {
        last_error_ = "reconcile called before finalize";
        return -1;
    }
    if (!in_schema || !in_array || !out) return -1;

    int64_t num_rows = in_array->length;

    // Initialize out as a struct matching the locked schema. This allocates
    // children of the right types; we'll populate them below.
    ArrowError err;
    ArrowErrorInit(&err);
    if (ArrowArrayInitFromSchema(out, locked_schema_.get(), &err) !=
        NANOARROW_OK) {
        last_error_ = "ArrowArrayInitFromSchema failed for reconciled array";
        return -1;
    }

    // Build: input-name -> input-child-index
    std::unordered_map<std::string, int64_t> in_idx;
    in_idx.reserve(static_cast<size_t>(in_schema->n_children));
    for (int64_t i = 0; i < in_schema->n_children; ++i) {
        const ArrowSchema *c = in_schema->children[i];
        if (c && c->name) in_idx.emplace(c->name, i);
    }

    // For each known union column (all except the final _extra), try to take
    // it from the input batch. If missing, null-pad.
    int64_t n_known = num_known_columns();
    for (int64_t i = 0; i < n_known; ++i) {
        const std::string &name = names_[static_cast<size_t>(i)];
        auto it = in_idx.find(name);
        if (it != in_idx.end()) {
            // Release the pre-initialized placeholder child and move the
            // input child into its slot (zero copy; release of the input
            // goes null after the move).
            ArrowArray *slot = out->children[i];
            if (slot->release) slot->release(slot);
            ArrowArrayMove(in_array->children[it->second], slot);
        } else {
            ArrowArray *slot = out->children[i];
            if (slot->release) slot->release(slot);
            if (build_null_array(locked_schema_->children[i], num_rows, slot) !=
                0) {
                last_error_ = "failed to build null column for missing field";
                return -1;
            }
        }
    }

    // Find input children whose names aren't in the union: these feed _extra.
    std::vector<int64_t> unknown_in;
    for (int64_t i = 0; i < in_schema->n_children; ++i) {
        const ArrowSchema *c = in_schema->children[i];
        if (!c || !c->name) continue;
        if (!name_to_idx_.count(c->name)) unknown_in.push_back(i);
    }

    // Build the _extra column. Fast path: no unknowns -> all nulls.
    ArrowArray *extra_slot = out->children[n_known];
    if (extra_slot->release) extra_slot->release(extra_slot);
    if (unknown_in.empty()) {
        if (ArrowArrayInitFromType(extra_slot, NANOARROW_TYPE_STRING) !=
            NANOARROW_OK) {
            last_error_ = "failed to init null _extra column";
            return -1;
        }
        if (ArrowArrayStartAppending(extra_slot) != NANOARROW_OK ||
            ArrowArrayAppendNull(extra_slot, num_rows) != NANOARROW_OK ||
            ArrowArrayFinishBuildingDefault(extra_slot, &err) != NANOARROW_OK) {
            last_error_ = "failed to append nulls to _extra";
            return -1;
        }
    } else {
        // Slow path: JSON-encode unknown fields per row.
        if (ArrowArrayInitFromType(extra_slot, NANOARROW_TYPE_STRING) !=
            NANOARROW_OK) {
            last_error_ = "failed to init string _extra column";
            return -1;
        }
        if (ArrowArrayStartAppending(extra_slot) != NANOARROW_OK) {
            last_error_ = "failed to start appending to _extra";
            return -1;
        }
        // Bind one ArrowArrayView per unknown column once, then index by row.
        // A column that fails to init/bind is marked invalid (emits null).
        struct UnknownColumn {
            ArrowArrayView view;
            ArrowType type = NANOARROW_TYPE_NA;
            bool valid = false;
        };
        std::vector<UnknownColumn> unknown_views(unknown_in.size());
        for (size_t k = 0; k < unknown_in.size(); ++k) {
            int64_t u = unknown_in[k];
            const ArrowSchema *cs = in_schema->children[u];
            const ArrowArray *ca = in_array->children[u];
            if (!cs || !ca || !cs->name) continue;
            UnknownColumn &uc = unknown_views[k];
            uc.type = type_from_format(cs);
            ArrowArrayViewInitFromType(&uc.view, uc.type);
            ArrowError verr;
            ArrowErrorInit(&verr);
            if (ArrowArrayViewSetArray(&uc.view, ca, &verr) == NANOARROW_OK) {
                uc.valid = true;
            } else {
                ArrowArrayViewReset(&uc.view);
            }
        }

        std::string buf;
        for (int64_t row = 0; row < num_rows; ++row) {
            buf.clear();
            buf.push_back('{');
            bool first = true;
            for (size_t k = 0; k < unknown_in.size(); ++k) {
                int64_t u = unknown_in[k];
                const ArrowSchema *cs = in_schema->children[u];
                const ArrowArray *ca = in_array->children[u];
                if (!cs || !ca || !cs->name) continue;
                if (!first) buf.push_back(',');
                first = false;
                buf.push_back('"');
                json_escape(cs->name, buf);
                buf.append("\":");
                const UnknownColumn &uc = unknown_views[k];
                append_json_scalar(uc.valid ? &uc.view : nullptr, uc.type, row,
                                   buf);
            }
            buf.push_back('}');
            ArrowStringView sv{buf.data(), static_cast<int64_t>(buf.size())};
            if (ArrowArrayAppendString(extra_slot, sv) != NANOARROW_OK) {
                last_error_ = "failed to append _extra row";
                for (auto &uc : unknown_views) {
                    if (uc.valid) ArrowArrayViewReset(&uc.view);
                }
                return -1;
            }
        }
        for (auto &uc : unknown_views) {
            if (uc.valid) ArrowArrayViewReset(&uc.view);
        }
        if (ArrowArrayFinishBuildingDefault(extra_slot, &err) != NANOARROW_OK) {
            last_error_ = "failed to finish _extra column";
            return -1;
        }
    }

    out->length = num_rows;
    out->null_count = 0;
    return 0;
}

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_ENABLE_ARROW
