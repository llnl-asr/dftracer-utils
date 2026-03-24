#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/python/arrow_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/trace_reader.h>
#include <dftracer/utils/python/trace_reader_iterator.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>

#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#include <yyjson.h>
#endif

namespace {

using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
using dftracer::utils::utilities::reader::ReadConfig;
using dftracer::utils::utilities::reader::TraceReader;
using dftracer::utils::utilities::reader::TraceReaderConfig;

CoroTask<void> produce_lines(std::shared_ptr<IteratorState> state,
                             TraceReaderConfig cfg, ReadConfig rc) {
    auto *sp = state.get();
    try {
        TraceReader reader(std::move(cfg));
        auto gen = reader.read_lines(rc);
        while (auto opt = co_await gen.next()) {
            if (sp->cancelled.load(std::memory_order_acquire)) break;
            std::string item(opt->content);
            {
                std::unique_lock<std::mutex> lock(sp->mtx);
                sp->cv_producer.wait(lock, [sp] {
                    return sp->queue.size() < sp->max_queue_size ||
                           sp->cancelled.load(std::memory_order_acquire);
                });
                if (sp->cancelled.load(std::memory_order_acquire)) break;
                sp->queue.push(std::move(item));
            }
            sp->cv_consumer.notify_one();
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(sp->mtx);
        sp->error = std::current_exception();
        sp->queue.push(std::nullopt);
        sp->done.store(true, std::memory_order_release);
        sp->cv_consumer.notify_one();
        co_return;
    }
    {
        std::lock_guard<std::mutex> lock(sp->mtx);
        sp->queue.push(std::nullopt);
        sp->done.store(true, std::memory_order_release);
    }
    sp->cv_consumer.notify_one();
}

CoroTask<void> produce_raw(std::shared_ptr<IteratorState> state,
                           TraceReaderConfig cfg, ReadConfig rc) {
    auto *sp = state.get();
    try {
        TraceReader reader(std::move(cfg));
        auto gen = reader.read_raw(rc);
        while (auto opt = co_await gen.next()) {
            if (sp->cancelled.load(std::memory_order_acquire)) break;
            std::string item(opt->data(), opt->size());
            {
                std::unique_lock<std::mutex> lock(sp->mtx);
                sp->cv_producer.wait(lock, [sp] {
                    return sp->queue.size() < sp->max_queue_size ||
                           sp->cancelled.load(std::memory_order_acquire);
                });
                if (sp->cancelled.load(std::memory_order_acquire)) break;
                sp->queue.push(std::move(item));
            }
            sp->cv_consumer.notify_one();
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(sp->mtx);
        sp->error = std::current_exception();
        sp->queue.push(std::nullopt);
        sp->done.store(true, std::memory_order_release);
        sp->cv_consumer.notify_one();
        co_return;
    }
    {
        std::lock_guard<std::mutex> lock(sp->mtx);
        sp->queue.push(std::nullopt);
        sp->done.store(true, std::memory_order_release);
    }
    sp->cv_consumer.notify_one();
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW

using dftracer::utils::utilities::common::arrow::ColumnType;
using dftracer::utils::utilities::common::arrow::RecordBatchBuilder;

CoroTask<void> produce_arrow_batches(std::shared_ptr<ArrowIteratorState> state,
                                     TraceReaderConfig cfg, ReadConfig rc,
                                     std::size_t batch_size) {
    auto *sp = state.get();
    try {
        TraceReader reader(std::move(cfg));
        auto gen = reader.read_lines(rc);
        RecordBatchBuilder builder;
        builder.reserve(batch_size);

        // Keep yyjson docs alive until finish() since string columns hold
        // string_views into doc memory. Serialized object/array values are
        // stored as owned strings in held_serialized.
        std::vector<yyjson_doc *> held_docs;
        std::vector<std::string> held_serialized;
        held_docs.reserve(batch_size);

        while (auto opt = co_await gen.next()) {
            if (sp->cancelled.load(std::memory_order_acquire)) break;

            const char *trimmed;
            std::size_t trimmed_length;
            if (!dftracer::utils::json_trim_and_validate(
                    opt->content.data(), opt->content.size(), trimmed,
                    trimmed_length)) {
                continue;
            }

            yyjson_doc *doc = yyjson_read(trimmed, trimmed_length, 0);
            if (!doc) continue;

            yyjson_val *root = yyjson_doc_get_root(doc);
            if (!root || !yyjson_is_obj(root)) {
                yyjson_doc_free(doc);
                continue;
            }

            yyjson_obj_iter iter;
            yyjson_obj_iter_init(root, &iter);
            yyjson_val *key;
            while ((key = yyjson_obj_iter_next(&iter))) {
                yyjson_val *val = yyjson_obj_iter_get_val(key);
                const char *key_str = yyjson_get_str(key);
                std::size_t key_len = yyjson_get_len(key);
                std::string_view key_sv(key_str, key_len);

                if (yyjson_is_int(val)) {
                    std::size_t idx =
                        builder.add_or_get_column(key_sv, ColumnType::INT64);
                    builder.append_int64(idx, yyjson_get_sint(val));
                } else if (yyjson_is_uint(val)) {
                    std::size_t idx =
                        builder.add_or_get_column(key_sv, ColumnType::UINT64);
                    builder.append_uint64(idx, yyjson_get_uint(val));
                } else if (yyjson_is_real(val)) {
                    std::size_t idx =
                        builder.add_or_get_column(key_sv, ColumnType::DOUBLE);
                    builder.append_double(idx, yyjson_get_real(val));
                } else if (yyjson_is_bool(val)) {
                    std::size_t idx =
                        builder.add_or_get_column(key_sv, ColumnType::BOOL);
                    builder.append_bool(idx, yyjson_get_bool(val));
                } else if (yyjson_is_str(val)) {
                    std::size_t idx =
                        builder.add_or_get_column(key_sv, ColumnType::STRING);
                    // string_view into doc memory — doc kept alive in
                    // held_docs until finish()
                    builder.append_string(
                        idx, std::string_view(yyjson_get_str(val),
                                              yyjson_get_len(val)));
                } else if (yyjson_is_null(val)) {
                    // Only append null to an existing column; skip if the
                    // column is new — we don't know its type yet and creating
                    // it as STRING would corrupt later typed appends.
                    auto existing = builder.find_column(key_sv);
                    if (existing) builder.append_null(*existing);
                } else {
                    // object/array: serialize to JSON string
                    std::size_t json_len;
                    char *json_str = yyjson_val_write(val, 0, &json_len);
                    std::size_t idx =
                        builder.add_or_get_column(key_sv, ColumnType::STRING);
                    if (json_str) {
                        held_serialized.emplace_back(json_str, json_len);
                        free(json_str);
                        builder.append_string(idx, held_serialized.back());
                    } else {
                        builder.append_null(idx);
                    }
                }
            }
            builder.end_row();
            held_docs.push_back(doc);

            if (builder.num_rows() >= batch_size) {
                auto result = builder.finish();
                for (auto *d : held_docs) yyjson_doc_free(d);
                held_docs.clear();
                held_serialized.clear();

                {
                    std::unique_lock<std::mutex> lock(sp->mtx);
                    sp->cv_producer.wait(lock, [sp] {
                        return sp->queue.size() < sp->max_queue_size ||
                               sp->cancelled.load(std::memory_order_acquire);
                    });
                    if (sp->cancelled.load(std::memory_order_acquire)) break;
                    sp->queue.push(std::move(result));
                }
                sp->cv_consumer.notify_one();
                builder.reset(false);
                builder.reserve(batch_size);
            }
        }

        // Flush remaining rows
        if (builder.num_rows() > 0) {
            auto result = builder.finish();
            for (auto *d : held_docs) yyjson_doc_free(d);
            held_docs.clear();
            held_serialized.clear();
            {
                std::lock_guard<std::mutex> lock(sp->mtx);
                sp->queue.push(std::move(result));
            }
            sp->cv_consumer.notify_one();
        } else {
            for (auto *d : held_docs) yyjson_doc_free(d);
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(sp->mtx);
        sp->error = std::current_exception();
        sp->queue.push(std::nullopt);
        sp->done.store(true, std::memory_order_release);
        sp->cv_consumer.notify_one();
        co_return;
    }
    {
        std::lock_guard<std::mutex> lock(sp->mtx);
        sp->queue.push(std::nullopt);
        sp->done.store(true, std::memory_order_release);
    }
    sp->cv_consumer.notify_one();
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW

TraceReaderConfig build_config(TraceReaderObject *self) {
    TraceReaderConfig cfg;
    cfg.file_path = PyUnicode_AsUTF8(self->file_path);
    const char *idx = PyUnicode_AsUTF8(self->index_dir);
    if (idx) cfg.index_dir = idx;
    cfg.checkpoint_size = self->checkpoint_size;
    cfg.auto_build_index = self->auto_build_index != 0;
    cfg.index_threshold = self->index_threshold;
    return cfg;
}

static Runtime *get_runtime(TraceReaderObject *self) {
    if (self->runtime_obj) {
        return ((RuntimeObject *)self->runtime_obj)->runtime.get();
    }
    return get_default_runtime();
}

static TraceReaderIteratorObject *make_iterator(
    std::shared_ptr<IteratorState> state, IteratorMode mode) {
    TraceReaderIteratorObject *it =
        (TraceReaderIteratorObject *)TraceReaderIteratorType.tp_alloc(
            &TraceReaderIteratorType, 0);
    if (!it) return NULL;
    new (&it->state) std::shared_ptr<IteratorState>(std::move(state));
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    new (&it->arrow_state) std::shared_ptr<ArrowIteratorState>();
#endif
    it->mode = mode;
    return it;
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW
static TraceReaderIteratorObject *make_arrow_iterator(
    std::shared_ptr<ArrowIteratorState> state) {
    TraceReaderIteratorObject *it =
        (TraceReaderIteratorObject *)TraceReaderIteratorType.tp_alloc(
            &TraceReaderIteratorType, 0);
    if (!it) return NULL;
    new (&it->state) std::shared_ptr<IteratorState>();
    new (&it->arrow_state)
        std::shared_ptr<ArrowIteratorState>(std::move(state));
    it->mode = IteratorMode::ARROW;
    return it;
}
#endif

}  // namespace

using dftracer::utils::python::wrap_arrow_table;

static void TraceReader_dealloc(TraceReaderObject *self) {
    Py_XDECREF(self->file_path);
    Py_XDECREF(self->index_dir);
    Py_XDECREF(self->runtime_obj);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *TraceReader_new(PyTypeObject *type, PyObject *args,
                                 PyObject *kwds) {
    TraceReaderObject *self = (TraceReaderObject *)type->tp_alloc(type, 0);
    if (self) {
        self->file_path = NULL;
        self->index_dir = NULL;
        self->checkpoint_size = 32 * 1024 * 1024;
        self->auto_build_index = 0;
        self->index_threshold =
            dftracer::utils::constants::indexer::DEFAULT_INDEX_SIZE_THRESHOLD;
        self->has_index = 0;
        self->runtime_obj = NULL;
    }
    return (PyObject *)self;
}

static int TraceReader_init(TraceReaderObject *self, PyObject *args,
                            PyObject *kwds) {
    static const char *kwlist[] = {"file_path",
                                   "index_dir",
                                   "checkpoint_size",
                                   "auto_build_index",
                                   "index_threshold",
                                   "runtime",
                                   NULL};

    const char *file_path;
    const char *index_dir = "";
    std::size_t checkpoint_size = 32 * 1024 * 1024;
    int auto_build_index = 0;
    std::size_t index_threshold =
        dftracer::utils::constants::indexer::DEFAULT_INDEX_SIZE_THRESHOLD;
    PyObject *runtime_arg = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|snpnO", (char **)kwlist,
                                     &file_path, &index_dir, &checkpoint_size,
                                     &auto_build_index, &index_threshold,
                                     &runtime_arg)) {
        return -1;
    }

    if (runtime_arg && runtime_arg != Py_None) {
        if (PyObject_TypeCheck(runtime_arg, &RuntimeType)) {
            // Direct C++ Runtime object
            Py_INCREF(runtime_arg);
            self->runtime_obj = runtime_arg;
        } else {
            // Python wrapper, extract _native attribute
            PyObject *native = PyObject_GetAttrString(runtime_arg, "_native");
            if (native && PyObject_TypeCheck(native, &RuntimeType)) {
                self->runtime_obj = native;  // already incref'd by GetAttr
            } else {
                Py_XDECREF(native);
                PyErr_SetString(PyExc_TypeError,
                                "runtime must be a Runtime instance or None");
                return -1;
            }
        }
    }

    self->file_path = PyUnicode_FromString(file_path);
    if (!self->file_path) return -1;

    self->index_dir = PyUnicode_FromString(index_dir);
    if (!self->index_dir) {
        Py_DECREF(self->file_path);
        self->file_path = NULL;
        return -1;
    }

    self->checkpoint_size = checkpoint_size;
    self->auto_build_index = auto_build_index;
    self->index_threshold = index_threshold;

    try {
        TraceReaderConfig cfg;
        cfg.file_path = file_path;
        cfg.index_dir = index_dir;
        cfg.checkpoint_size = checkpoint_size;
        cfg.auto_build_index = auto_build_index != 0;
        cfg.index_threshold = index_threshold;
        TraceReader probe(std::move(cfg));
        self->has_index = probe.has_index() ? 1 : 0;
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        Py_DECREF(self->file_path);
        Py_DECREF(self->index_dir);
        self->file_path = NULL;
        self->index_dir = NULL;
        return -1;
    }

    return 0;
}

static PyObject *TraceReader_iter_lines(TraceReaderObject *self, PyObject *args,
                                        PyObject *kwds) {
    static const char *kwlist[] = {"start_line", "end_line",    "start_byte",
                                   "end_byte",   "buffer_size", "query",
                                   NULL};
    Py_ssize_t start_line = 0, end_line = 0;
    Py_ssize_t start_byte = 0, end_byte = 0;
    Py_ssize_t buffer_size = 4 * 1024 * 1024;
    const char *query_str = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|nnnnnz", (char **)kwlist,
                                     &start_line, &end_line, &start_byte,
                                     &end_byte, &buffer_size, &query_str)) {
        return NULL;
    }

    if (start_line < 0 || end_line < 0 || start_byte < 0 || end_byte < 0 ||
        buffer_size <= 0) {
        PyErr_SetString(
            PyExc_ValueError,
            "range arguments must be >= 0; buffer_size must be > 0");
        return NULL;
    }

    TraceReaderConfig cfg;
    try {
        cfg = build_config(self);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    ReadConfig rc;
    rc.start_line = static_cast<std::size_t>(start_line);
    rc.end_line = static_cast<std::size_t>(end_line);
    rc.start_byte = static_cast<std::size_t>(start_byte);
    rc.end_byte = static_cast<std::size_t>(end_byte);
    rc.buffer_size = static_cast<std::size_t>(buffer_size);
    if (query_str) rc.query = query_str;

    auto state = std::make_shared<IteratorState>();

    Runtime *rt = get_runtime(self);
    try {
        rt->submit(produce_lines(state, cfg, rc), "iter_lines");
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    TraceReaderIteratorObject *it = make_iterator(state, IteratorMode::LINES);
    return (PyObject *)it;
}

static PyObject *TraceReader_iter_raw(TraceReaderObject *self, PyObject *args,
                                      PyObject *kwds) {
    static const char *kwlist[] = {"start_line", "end_line",    "start_byte",
                                   "end_byte",   "buffer_size", "line_aligned",
                                   "multi_line", "query",       NULL};
    Py_ssize_t start_line = 0, end_line = 0;
    Py_ssize_t start_byte = 0, end_byte = 0;
    Py_ssize_t buffer_size = 4 * 1024 * 1024;
    int line_aligned = 1;
    int multi_line = 1;
    const char *query_str = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|nnnnnppz", (char **)kwlist,
                                     &start_line, &end_line, &start_byte,
                                     &end_byte, &buffer_size, &line_aligned,
                                     &multi_line, &query_str)) {
        return NULL;
    }

    if (start_line < 0 || end_line < 0 || start_byte < 0 || end_byte < 0 ||
        buffer_size <= 0) {
        PyErr_SetString(
            PyExc_ValueError,
            "range arguments must be >= 0; buffer_size must be > 0");
        return NULL;
    }

    TraceReaderConfig cfg;
    try {
        cfg = build_config(self);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    ReadConfig rc;
    rc.start_line = static_cast<std::size_t>(start_line);
    rc.end_line = static_cast<std::size_t>(end_line);
    rc.start_byte = static_cast<std::size_t>(start_byte);
    rc.end_byte = static_cast<std::size_t>(end_byte);
    rc.buffer_size = static_cast<std::size_t>(buffer_size);
    rc.line_aligned = line_aligned != 0;
    rc.multi_line = multi_line != 0;
    if (query_str) rc.query = query_str;

    auto state = std::make_shared<IteratorState>();

    Runtime *rt = get_runtime(self);
    try {
        rt->submit(produce_raw(state, cfg, rc), "iter_raw");
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    TraceReaderIteratorObject *it = make_iterator(state, IteratorMode::RAW);
    return (PyObject *)it;
}

static PyObject *TraceReader_read_lines(TraceReaderObject *self, PyObject *args,
                                        PyObject *kwds) {
    PyObject *iter = TraceReader_iter_lines(self, args, kwds);
    if (!iter) return NULL;
    PyObject *list = PySequence_List(iter);
    Py_DECREF(iter);
    return list;
}

static PyObject *TraceReader_read_raw(TraceReaderObject *self, PyObject *args,
                                      PyObject *kwds) {
    PyObject *iter = TraceReader_iter_raw(self, args, kwds);
    if (!iter) return NULL;
    PyObject *list = PySequence_List(iter);
    Py_DECREF(iter);
    return list;
}

static PyObject *TraceReader_iter_lines_json(TraceReaderObject *self,
                                             PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"start_line", "end_line",    "start_byte",
                                   "end_byte",   "buffer_size", "query",
                                   NULL};
    Py_ssize_t start_line = 0, end_line = 0;
    Py_ssize_t start_byte = 0, end_byte = 0;
    Py_ssize_t buffer_size = 4 * 1024 * 1024;
    const char *query_str = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|nnnnnz", (char **)kwlist,
                                     &start_line, &end_line, &start_byte,
                                     &end_byte, &buffer_size, &query_str)) {
        return NULL;
    }

    if (start_line < 0 || end_line < 0 || start_byte < 0 || end_byte < 0 ||
        buffer_size <= 0) {
        PyErr_SetString(
            PyExc_ValueError,
            "range arguments must be >= 0; buffer_size must be > 0");
        return NULL;
    }

    TraceReaderConfig cfg;
    try {
        cfg = build_config(self);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    ReadConfig rc;
    rc.start_line = static_cast<std::size_t>(start_line);
    rc.end_line = static_cast<std::size_t>(end_line);
    rc.start_byte = static_cast<std::size_t>(start_byte);
    rc.end_byte = static_cast<std::size_t>(end_byte);
    rc.buffer_size = static_cast<std::size_t>(buffer_size);
    if (query_str) rc.query = query_str;

    auto state = std::make_shared<IteratorState>();

    Runtime *rt = get_runtime(self);
    try {
        rt->submit(produce_lines(state, cfg, rc), "iter_lines_json");
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    TraceReaderIteratorObject *it = make_iterator(state, IteratorMode::JSON);
    return (PyObject *)it;
}

static PyObject *TraceReader_read_lines_json(TraceReaderObject *self,
                                             PyObject *args, PyObject *kwds) {
    PyObject *iter = TraceReader_iter_lines_json(self, args, kwds);
    if (!iter) return NULL;
    PyObject *list = PySequence_List(iter);
    Py_DECREF(iter);
    return list;
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW

static PyObject *TraceReader_iter_arrow(TraceReaderObject *self, PyObject *args,
                                        PyObject *kwds) {
    static const char *kwlist[] = {"batch_size", "start_line", "end_line",
                                   "start_byte", "end_byte",   "buffer_size",
                                   "query",      NULL};
    Py_ssize_t batch_size = 10000;
    Py_ssize_t start_line = 0, end_line = 0;
    Py_ssize_t start_byte = 0, end_byte = 0;
    Py_ssize_t buffer_size = 4 * 1024 * 1024;
    const char *query_str = NULL;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|nnnnnnz", (char **)kwlist, &batch_size, &start_line,
            &end_line, &start_byte, &end_byte, &buffer_size, &query_str)) {
        return NULL;
    }

    if (batch_size <= 0) {
        PyErr_SetString(PyExc_ValueError, "batch_size must be > 0");
        return NULL;
    }
    if (start_line < 0 || end_line < 0 || start_byte < 0 || end_byte < 0 ||
        buffer_size <= 0) {
        PyErr_SetString(
            PyExc_ValueError,
            "range arguments must be >= 0; buffer_size must be > 0");
        return NULL;
    }

    TraceReaderConfig cfg;
    try {
        cfg = build_config(self);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    ReadConfig rc;
    rc.start_line = static_cast<std::size_t>(start_line);
    rc.end_line = static_cast<std::size_t>(end_line);
    rc.start_byte = static_cast<std::size_t>(start_byte);
    rc.end_byte = static_cast<std::size_t>(end_byte);
    rc.buffer_size = static_cast<std::size_t>(buffer_size);
    if (query_str) rc.query = query_str;

    auto state = std::make_shared<ArrowIteratorState>();

    Runtime *rt = get_runtime(self);
    try {
        rt->submit(produce_arrow_batches(state, cfg, rc,
                                         static_cast<std::size_t>(batch_size)),
                   "iter_arrow");
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    TraceReaderIteratorObject *it = make_arrow_iterator(std::move(state));
    return (PyObject *)it;
}

static PyObject *TraceReader_read_arrow(TraceReaderObject *self, PyObject *args,
                                        PyObject *kwds) {
    PyObject *iter = TraceReader_iter_arrow(self, args, kwds);
    if (!iter) return NULL;
    PyObject *list = PySequence_List(iter);
    Py_DECREF(iter);
    if (!list) return NULL;

    return wrap_arrow_table(list);
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW

static PyObject *TraceReader_enter(TraceReaderObject *self,
                                   PyObject *Py_UNUSED(ignored)) {
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *TraceReader_exit(TraceReaderObject *self, PyObject *args) {
    Py_RETURN_NONE;
}

static PyObject *TraceReader_get_file_path(TraceReaderObject *self,
                                           void *closure) {
    Py_INCREF(self->file_path);
    return self->file_path;
}

static PyObject *TraceReader_get_index_dir(TraceReaderObject *self,
                                           void *closure) {
    Py_INCREF(self->index_dir);
    return self->index_dir;
}

static PyObject *TraceReader_get_has_index(TraceReaderObject *self,
                                           void *closure) {
    return PyBool_FromLong(self->has_index);
}

static PyObject *TraceReader_get_num_lines_prop(TraceReaderObject *self,
                                                void *closure) {
    try {
        TraceReaderConfig cfg = build_config(self);
        TraceReader reader(std::move(cfg));
        std::size_t n = reader.get_num_lines();
        if (n > 0) return PyLong_FromSize_t(n);
    } catch (...) {
    }
    PyObject *empty_args = PyTuple_New(0);
    if (!empty_args) return NULL;
    PyObject *list = TraceReader_read_lines(self, empty_args, NULL);
    Py_DECREF(empty_args);
    if (!list) return NULL;
    Py_ssize_t n = PyList_GET_SIZE(list);
    Py_DECREF(list);
    return PyLong_FromSsize_t(n);
}

static PyObject *TraceReader_get_max_bytes(TraceReaderObject *self,
                                           PyObject *Py_UNUSED(ignored)) {
    try {
        TraceReaderConfig cfg = build_config(self);
        TraceReader reader(std::move(cfg));
        return PyLong_FromSize_t(reader.get_max_bytes());
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
}

static PyObject *TraceReader_get_num_lines(TraceReaderObject *self,
                                           PyObject *Py_UNUSED(ignored)) {
    try {
        TraceReaderConfig cfg = build_config(self);
        TraceReader reader(std::move(cfg));
        return PyLong_FromSize_t(reader.get_num_lines());
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
}

static PyMethodDef TraceReader_methods[] = {
    {"iter_lines", (PyCFunction)TraceReader_iter_lines,
     METH_VARARGS | METH_KEYWORDS,
     "Return an iterator over decoded lines.\n"
     "\n"
     "Args:\n"
     "    start_line (int): First line (0 = beginning).\n"
     "    end_line (int): Last line (0 = end of file).\n"
     "    start_byte (int): First byte offset (0 = beginning).\n"
     "    end_byte (int): Last byte offset (0 = end of file).\n"
     "    buffer_size (int): Internal read buffer size in bytes.\n"},
    {"iter_raw", (PyCFunction)TraceReader_iter_raw,
     METH_VARARGS | METH_KEYWORDS,
     "Return an iterator over raw byte chunks.\n"
     "\n"
     "Args:\n"
     "    start_line (int): First line (0 = beginning).\n"
     "    end_line (int): Last line (0 = end of file).\n"
     "    start_byte (int): First byte offset (0 = beginning).\n"
     "    end_byte (int): Last byte offset (0 = end of file).\n"
     "    buffer_size (int): Internal read buffer size in bytes.\n"
     "    line_aligned (bool): Align chunks to line boundaries.\n"
     "    multi_line (bool): Allow multiple lines per chunk.\n"},
    {"read_lines", (PyCFunction)TraceReader_read_lines,
     METH_VARARGS | METH_KEYWORDS,
     "Read all lines and return as list.\n"
     "\n"
     "Args:\n"
     "    start_line (int): First line (0 = beginning).\n"
     "    end_line (int): Last line (0 = end of file).\n"
     "    start_byte (int): First byte offset (0 = beginning).\n"
     "    end_byte (int): Last byte offset (0 = end of file).\n"
     "    buffer_size (int): Internal read buffer size in bytes.\n"},
    {"read_raw", (PyCFunction)TraceReader_read_raw,
     METH_VARARGS | METH_KEYWORDS,
     "Read all raw chunks and return as list.\n"
     "\n"
     "Args:\n"
     "    start_line (int): First line (0 = beginning).\n"
     "    end_line (int): Last line (0 = end of file).\n"
     "    start_byte (int): First byte offset (0 = beginning).\n"
     "    end_byte (int): Last byte offset (0 = end of file).\n"
     "    buffer_size (int): Internal read buffer size in bytes.\n"
     "    line_aligned (bool): Align chunks to line boundaries.\n"
     "    multi_line (bool): Allow multiple lines per chunk.\n"},
    {"iter_lines_json", (PyCFunction)TraceReader_iter_lines_json,
     METH_VARARGS | METH_KEYWORDS,
     "Return an iterator over parsed JSON objects.\n"
     "\n"
     "Args:\n"
     "    start_line (int): First line (0 = beginning).\n"
     "    end_line (int): Last line (0 = end of file).\n"
     "    start_byte (int): First byte offset (0 = beginning).\n"
     "    end_byte (int): Last byte offset (0 = end of file).\n"
     "    buffer_size (int): Internal read buffer size in bytes.\n"},
    {"read_lines_json", (PyCFunction)TraceReader_read_lines_json,
     METH_VARARGS | METH_KEYWORDS,
     "Read all lines as parsed JSON objects.\n"
     "\n"
     "Args:\n"
     "    start_line (int): First line (0 = beginning).\n"
     "    end_line (int): Last line (0 = end of file).\n"
     "    start_byte (int): First byte offset (0 = beginning).\n"
     "    end_byte (int): Last byte offset (0 = end of file).\n"
     "    buffer_size (int): Internal read buffer size in bytes.\n"},
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    {"iter_arrow", (PyCFunction)TraceReader_iter_arrow,
     METH_VARARGS | METH_KEYWORDS,
     "Return an iterator over Arrow record batches.\n"
     "\n"
     "Args:\n"
     "    batch_size (int): Maximum rows per Arrow batch.\n"
     "    start_line (int): First line (0 = beginning).\n"
     "    end_line (int): Last line (0 = end of file).\n"
     "    start_byte (int): First byte offset (0 = beginning).\n"
     "    end_byte (int): Last byte offset (0 = end of file).\n"
     "    buffer_size (int): Internal read buffer size in bytes.\n"},
    {"read_arrow", (PyCFunction)TraceReader_read_arrow,
     METH_VARARGS | METH_KEYWORDS,
     "Read all events as a materialized ArrowTable.\n"
     "\n"
     "Args:\n"
     "    batch_size (int): Maximum rows per Arrow batch.\n"
     "    start_line (int): First line (0 = beginning).\n"
     "    end_line (int): Last line (0 = end of file).\n"
     "    start_byte (int): First byte offset (0 = beginning).\n"
     "    end_byte (int): Last byte offset (0 = end of file).\n"
     "    buffer_size (int): Internal read buffer size in bytes.\n"},
#endif
    {"get_max_bytes", (PyCFunction)TraceReader_get_max_bytes, METH_NOARGS,
     "Get the maximum byte position (0 if unknown for compressed\n"
     "files without index)."},
    {"get_num_lines", (PyCFunction)TraceReader_get_num_lines, METH_NOARGS,
     "Get the total number of lines (0 if unknown for files without\n"
     "index)."},
    {"__enter__", (PyCFunction)TraceReader_enter, METH_NOARGS,
     "Enter the runtime context for the with statement."},
    {"__exit__", (PyCFunction)TraceReader_exit, METH_VARARGS,
     "Exit the runtime context for the with statement."},
    {NULL}};

static PyGetSetDef TraceReader_getsetters[] = {
    {"file_path", (getter)TraceReader_get_file_path, NULL,
     "Path to the trace file", NULL},
    {"index_dir", (getter)TraceReader_get_index_dir, NULL,
     "Directory for index files", NULL},
    {"has_index", (getter)TraceReader_get_has_index, NULL,
     "True if a checkpoint index was found", NULL},
    {"num_lines", (getter)TraceReader_get_num_lines_prop, NULL,
     "Total line count (reads all lines if needed)", NULL},
    {NULL}};

PyTypeObject TraceReaderType = {
    PyVarObject_HEAD_INIT(NULL, 0) "dftracer_utils_ext.TraceReader",
    sizeof(TraceReaderObject),                /* tp_basicsize */
    0,                                        /* tp_itemsize */
    (destructor)TraceReader_dealloc,          /* tp_dealloc */
    0,                                        /* tp_vectorcall_offset */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_as_async */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    0,                                        /* tp_call */
    0,                                        /* tp_str */
    0,                                        /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "TraceReader(file_path: str, index_dir: str = '',\n"
    "            checkpoint_size: int = 33554432,\n"
    "            auto_build_index: bool = False,\n"
    "            index_threshold: int = 8388608,\n"
    "            runtime: Runtime | None = None)\n"
    "--\n"
    "\n"
    "Smart trace file reader that auto-selects sequential or indexed\n"
    "reading based on whether an ``.idx`` sidecar exists.\n"
    "\n"
    "Args:\n"
    "    file_path (str): Path to the trace file (.pfw.gz or plain "
    "text).\n"
    "    index_dir (str): Directory to search for ``.idx`` sidecar "
    "files.\n"
    "        Empty string (default) searches next to the trace file.\n"
    "    checkpoint_size (int): Checkpoint interval in bytes for index\n"
    "        building (default 32 MB).\n"
    "    auto_build_index (bool): If True, automatically build an "
    "index\n"
    "        when none exists and the file exceeds *index_threshold*.\n"
    "    index_threshold (int): Minimum file size in bytes before\n"
    "        auto-indexing is triggered (default 8 MB).\n"
    "    runtime (Runtime or None): Runtime instance for thread pool "
    "control.\n"
    "        If None, uses the default global Runtime.\n"
    "\n"
    "Raises:\n"
    "    RuntimeError: If *file_path* does not exist or cannot be "
    "opened.\n",                /* tp_doc */
    0,                          /* tp_traverse */
    0,                          /* tp_clear */
    0,                          /* tp_richcompare */
    0,                          /* tp_weaklistoffset */
    0,                          /* tp_iter */
    0,                          /* tp_iternext */
    TraceReader_methods,        /* tp_methods */
    0,                          /* tp_members */
    TraceReader_getsetters,     /* tp_getset */
    0,                          /* tp_base */
    0,                          /* tp_dict */
    0,                          /* tp_descr_get */
    0,                          /* tp_descr_set */
    0,                          /* tp_dictoffset */
    (initproc)TraceReader_init, /* tp_init */
    0,                          /* tp_alloc */
    TraceReader_new,            /* tp_new */
};

int init_trace_reader(PyObject *m) {
    if (PyType_Ready(&TraceReaderType) < 0) return -1;

    Py_INCREF(&TraceReaderType);
    if (PyModule_AddObject(m, "TraceReader", (PyObject *)&TraceReaderType) <
        0) {
        Py_DECREF(&TraceReaderType);
        Py_DECREF(m);
        return -1;
    }

    return 0;
}
