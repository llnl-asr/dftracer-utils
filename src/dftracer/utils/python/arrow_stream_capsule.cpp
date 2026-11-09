// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/python/py_type_helpers.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/python/arrow_stream_capsule.h>
#include <dftracer/utils/python/batch_byte_size.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/schema_reconcile.h>
#include <nanoarrow/nanoarrow.h>

#include <cerrno>
#include <cstring>
#include <exception>
#include <mutex>

using ArrowExportResult = dftracer::utils::utilities::arrow::ArrowExportResult;

namespace {

// Drain until K consecutive batches add no new columns, bounded by MAX.
constexpr int STABLE_BATCHES = 5;
constexpr int MAX_DRAIN = 128;

struct StreamPrivate : dftracer::utils::dstd::HeapObject {
    dftracer::utils::dstd::SharedPtr<ArrowIteratorState> state;
    dftracer::utils::python::SchemaReconciler reconciler;
    // Drained during discovery, emitted first from get_next.
    dftracer::utils::dstd::Deque<ArrowExportResult> pending;
    dftracer::utils::dstd::String last_error;
    bool initialized = false;
    // Sticky: once set, all entry points short-circuit to EIO.
    bool error_set = false;
};

static void mark_error(StreamPrivate *p, dftracer::utils::dstd::String msg) {
    if (p->last_error.empty()) p->last_error = std::move(msg);
    p->error_set = true;
    p->initialized = true;
}

static int initialize_stream(StreamPrivate *p) {
    if (p->error_set) return EIO;
    if (p->initialized) return 0;
    auto *astate = p->state.get();

    int stable_run = 0;
    int drained = 0;
    while (stable_run < STABLE_BATCHES && drained < MAX_DRAIN) {
        auto batch = astate->channel->blocking_receive();
        if (!batch.has_value()) {
            // End-of-stream or producer error before discovery converged.
            std::lock_guard<std::mutex> lock(astate->error_mtx);
            if (astate->error) {
                try {
                    std::rethrow_exception(astate->error);
                } catch (const std::exception &e) {
                    mark_error(p, e.what());
                } catch (...) {
                    mark_error(p, "unknown error in Arrow stream");
                }
                return EIO;
            }
            break;  // clean early EOS; finalize with whatever we have
        }
        auto dequeued = dftracer::utils::python::byte_size(*batch);
        astate->bytes_in_queue.fetch_sub(dequeued, std::memory_order_acq_rel);

        bool added = p->reconciler.merge(batch->schema());
        if (!p->reconciler.last_error().empty()) {
            mark_error(p, p->reconciler.last_error());
            return EIO;
        }
        p->pending.push_back(std::move(*batch));
        stable_run = added ? 0 : (stable_run + 1);
        ++drained;
    }

    if (p->reconciler.finalize() != 0) {
        mark_error(p, p->reconciler.last_error().empty()
                          ? "failed to finalize schema union"
                          : p->reconciler.last_error());
        return EIO;
    }
    p->initialized = true;
    return 0;
}

static int stream_get_schema(struct ArrowArrayStream *s,
                             struct ArrowSchema *out) {
    auto *p = static_cast<StreamPrivate *>(s->private_data);
    int rc = initialize_stream(p);
    if (rc != 0) return rc;
    if (p->error_set) return EIO;
    if (p->reconciler.copy_schema(out) != 0) {
        mark_error(p, p->reconciler.last_error().empty()
                          ? "failed to copy locked schema"
                          : p->reconciler.last_error());
        return EIO;
    }
    return 0;
}

static int stream_get_next(struct ArrowArrayStream *s, struct ArrowArray *out) {
    auto *p = static_cast<StreamPrivate *>(s->private_data);
    if (p->error_set) return EIO;
    if (!p->initialized) {
        int rc = initialize_stream(p);
        if (rc != 0) return rc;
    }

    // Drain any discovery-phase batches first, then pull from the channel.
    dftracer::utils::dstd::Optional<ArrowExportResult> batch;
    if (!p->pending.empty()) {
        batch = std::move(p->pending.front());
        p->pending.pop_front();
    } else {
        auto *astate = p->state.get();
        batch = astate->channel->blocking_receive();
        if (!batch.has_value()) {
            std::lock_guard<std::mutex> lock(astate->error_mtx);
            if (astate->error) {
                try {
                    std::rethrow_exception(astate->error);
                } catch (const std::exception &e) {
                    mark_error(p, e.what());
                } catch (...) {
                    mark_error(p, "unknown error in Arrow stream");
                }
                return EIO;
            }
            // End of stream per Arrow C spec: return success with
            // out->release == nullptr.
            out->release = nullptr;
            return 0;
        }
        auto dequeued = dftracer::utils::python::byte_size(*batch);
        astate->bytes_in_queue.fetch_sub(dequeued, std::memory_order_acq_rel);
    }

    if (p->reconciler.reconcile(batch->schema(), batch->array(), out) != 0) {
        mark_error(p, p->reconciler.last_error().empty()
                          ? "schema reconciliation failed"
                          : p->reconciler.last_error());
        return EIO;
    }
    return 0;
}

static const char *stream_get_last_error(struct ArrowArrayStream *s) {
    auto *p = static_cast<StreamPrivate *>(s->private_data);
    if (!p || p->last_error.empty()) return nullptr;
    return p->last_error.c_str();
}

static void stream_release(struct ArrowArrayStream *s) {
    auto *p = static_cast<StreamPrivate *>(s->private_data);
    if (p) {
        if (p->state) {
            p->state->cancelled.store(true, std::memory_order_release);
            if (p->state->channel) p->state->channel->close();
            if (p->state->task_future.valid()) {
                // Release the GIL if this callback was invoked from a
                // Python-holding context (e.g. capsule destructor during
                // GC). If the GIL is not held (pyarrow's C reader path),
                // _PyThreadState_UncheckedGet() returns null and we wait
                // without touching the Python thread state.
                if (Py_IsInitialized() && PyGILState_Check()) {
                    Py_BEGIN_ALLOW_THREADS p->state->task_future.wait();
                    Py_END_ALLOW_THREADS
                } else {
                    p->state->task_future.wait();
                }
            }
        }
        delete p;
    }
    s->private_data = nullptr;
    s->release = nullptr;
}

static void release_stream_capsule(PyObject *capsule) {
    auto *stream = static_cast<ArrowArrayStream *>(
        PyCapsule_GetPointer(capsule, "arrow_array_stream"));
    if (stream && stream->release) {
        stream->release(stream);
    }
    dftracer::utils::dstd::UniquePtr<ArrowArrayStream>(stream).reset();
}

static PyObject *ArrowBatchStream_arrow_c_stream(ArrowBatchStreamObject *self,
                                                 PyObject *args) {
    PyObject *requested_schema = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &requested_schema)) return NULL;

    // Per the PyCapsule protocol, a non-None `requested_schema` means the
    // caller wants the stream cast to that schema. We only emit our native
    // schema today; reject explicitly so misuse fails loudly instead of
    // silently returning arrays that don't match what the caller asked for.
    if (requested_schema != Py_None) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "iter_arrow_stream does not support "
                        "requested_schema casting; pass None to use the "
                        "native schema.");
        return NULL;
    }

    if (self->consumed || !self->state) {
        PyErr_SetString(PyExc_RuntimeError,
                        "Arrow stream already exported via "
                        "__arrow_c_stream__; each stream can be "
                        "exported only once.");
        return NULL;
    }

    auto *priv = new StreamPrivate;
    priv->state = self->state;
    self->consumed = true;
    self->state.reset();

    auto *stream =
        dftracer::utils::dstd::make_unique<ArrowArrayStream>().release();
    std::memset(stream, 0, sizeof(*stream));
    stream->get_schema = stream_get_schema;
    stream->get_next = stream_get_next;
    stream->get_last_error = stream_get_last_error;
    stream->release = stream_release;
    stream->private_data = priv;

    PyObject *capsule =
        PyCapsule_New(stream, "arrow_array_stream", release_stream_capsule);
    if (!capsule) {
        stream->release(stream);
        dftracer::utils::dstd::UniquePtr<ArrowArrayStream>(stream).reset();
        return NULL;
    }
    return capsule;
}

static void ArrowBatchStream_dealloc(ArrowBatchStreamObject *self) {
    if (self->state) {
        self->state->cancelled.store(true, std::memory_order_release);
        if (self->state->channel) self->state->channel->close();
        Py_BEGIN_ALLOW_THREADS if (self->state->task_future.valid()) {
            self->state->task_future.wait();
        }
        Py_END_ALLOW_THREADS
    }
    self->state.~shared_ptr<ArrowIteratorState>();
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyMethodDef ArrowBatchStream_methods[] = {
    {"__arrow_c_stream__", DFTU_PYCFUNCTION(ArrowBatchStream_arrow_c_stream),
     METH_VARARGS, "Export as Arrow C Data Interface stream PyCapsule"},
    {NULL, NULL, 0, NULL}};

}  // namespace

PyTypeObject ArrowBatchStreamType = {
    .ob_base = PyVarObject_HEAD_INIT(NULL, 0).tp_name =
        "dftracer_utils_ext._ArrowBatchStream",
    .tp_basicsize = sizeof(ArrowBatchStreamObject),
    .tp_dealloc = (destructor)ArrowBatchStream_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Zero-iteration Arrow stream backed by a C++ coroutine channel",
    .tp_methods = ArrowBatchStream_methods,
};

int dftracer::utils::python::init_arrow_batch_stream(PyObject *m) {
    if (register_type(m, &ArrowBatchStreamType, "_ArrowBatchStream") < 0)
        return -1;
    return 0;
}

PyObject *dftracer::utils::python::make_arrow_batch_stream(
    dftracer::utils::dstd::SharedPtr<ArrowIteratorState> state) {
    auto *obj = (ArrowBatchStreamObject *)ArrowBatchStreamType.tp_alloc(
        &ArrowBatchStreamType, 0);
    if (!obj) return NULL;
    new (&obj->state)
        dftracer::utils::dstd::SharedPtr<ArrowIteratorState>(std::move(state));
    obj->consumed = false;
    return (PyObject *)obj;
}

#endif
