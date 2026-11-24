// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_FORMATS_FORMAT_H
#define DFTRACER_UTILS_PYTHON_FORMATS_FORMAT_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/dstd/function_ref.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/dom_value.h>
#include <dftracer/utils/json/writer.h>
#include <dftracer/utils/yaml/document.h>
#include <dftracer/utils/yaml/records.h>
#include <dftracer/utils/yaml/writer.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::python::formats {

/// What the binding needs of a format: its parsed types, its writer and how a
/// parsed value reads. The binding is one template on this, so the two formats
/// have the same names and the same behavior.
struct JsonFormat {
    using Doc = json::DomDocument;
    using Val = json::DomValue;
    using Writer = json::Writer;

    static constexpr const char *NAME = "json";
    static constexpr const char *TYPE_PREFIX = "Json";
    static constexpr const char *PARSE = "json_parse";
    static constexpr const char *LOADS = "json_loads";
    static constexpr const char *READ = "json_read";
    static constexpr const char *LOAD = "json_load";
    static constexpr const char *READ_RECORDS = "json_read_records";
    static constexpr int KIND_COUNT = 7;
    static constexpr const char *KIND_NAMES[KIND_COUNT] = {
        "OBJECT", "ARRAY", "STRING", "INT", "FLOAT", "BOOL", "NULL"};
    /// The kinds of the Python enum, by value.
    enum Kind : int { Object, Array, String, Int, Float, Bool, Null };

    static Writer make_writer(dstd::String &out, std::uint8_t) {
        return Writer(out);
    }
    static int kind(const Val &v);
    static bool is_object(const Val &v) {
        return v.type() == Val::Type::Object;
    }
    static bool is_array(const Val &v) { return v.type() == Val::Type::Array; }
    static bool is_null(const Val &v) { return v.type() == Val::Type::Null; }
    static dstd::Optional<Val> find(const Val &v, dstd::StringView key) {
        return v.find(key);
    }
    static void members(const Val &v,
                        dstd::FunctionRef<void(dstd::StringView, Val)> fn) {
        v.for_each_member(fn);
    }
    static void elements(const Val &v, dstd::FunctionRef<void(Val)> fn) {
        v.for_each_element(fn);
    }
    /// A new reference to the Python scalar of a value that is not a container.
    static PyObject *scalar(const Val &v);
    static PyObject *as_int(const Val &v);
    static PyObject *as_float(const Val &v);
    static PyObject *as_bool(const Val &v);
    static PyObject *as_str(const Val &v);
};

struct YamlFormat {
    using Doc = yaml::Document;
    using Val = yaml::Value;
    using Writer = yaml::Writer;

    static constexpr const char *NAME = "yaml";
    static constexpr const char *TYPE_PREFIX = "Yaml";
    static constexpr const char *PARSE = "yaml_parse";
    static constexpr const char *LOADS = "yaml_loads";
    static constexpr const char *READ = "yaml_read";
    static constexpr const char *LOAD = "yaml_load";
    static constexpr const char *READ_RECORDS = "yaml_read_records";
    static constexpr int KIND_COUNT = 4;
    static constexpr const char *KIND_NAMES[KIND_COUNT] = {"OBJECT", "ARRAY",
                                                           "SCALAR", "NULL"};
    enum Kind : int { Object, Array, Scalar, Null };

    static Writer make_writer(dstd::String &out, std::uint8_t indent) {
        yaml::WriterOptions options;
        options.indent = indent;
        return Writer(out, options);
    }
    static int kind(const Val &v);
    static bool is_object(const Val &v) { return v.is_object(); }
    static bool is_array(const Val &v) { return v.is_array(); }
    static bool is_null(const Val &v) { return v.is_null(); }
    static dstd::Optional<Val> find(const Val &v, dstd::StringView key) {
        return v.find(key);
    }
    static void members(const Val &v,
                        dstd::FunctionRef<void(dstd::StringView, Val)> fn) {
        v.for_each_member(fn);
    }
    static void elements(const Val &v, dstd::FunctionRef<void(Val)> fn) {
        v.for_each_element(fn);
    }
    static PyObject *scalar(const Val &v);
    static PyObject *as_int(const Val &v);
    static PyObject *as_float(const Val &v);
    static PyObject *as_bool(const Val &v);
    static PyObject *as_str(const Val &v);
};

/// A new str of UTF-8 text, or null with the Python error set.
inline PyObject *make_str(dstd::StringView text) {
    return PyUnicode_DecodeUTF8(text.data(),
                                static_cast<Py_ssize_t>(text.size()), nullptr);
}

}  // namespace dftracer::utils::python::formats

#endif  // DFTRACER_UTILS_PYTHON_FORMATS_FORMAT_H
