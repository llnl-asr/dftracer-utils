#ifndef DFTRACER_UTILS_PYTHON_TRACE_READER_ITERATOR_H
#define DFTRACER_UTILS_PYTHON_TRACE_READER_ITERATOR_H

#include <Python.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/task_handle.h>
#include <dftracer/utils/python/memoryview_batch.h>
#include <dftracer/utils/utilities/composites/dft/args_map.h>

#include <memory>
#include <string>
#include <vector>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>

typedef struct {
    PyObject_HEAD dftracer::utils::utilities::common::arrow::ArrowExportResult
        *result;
} ArrowBatchCapsuleObject;

extern PyTypeObject ArrowBatchCapsuleType;
#endif

enum class IteratorMode {
    MEMORYVIEW,
    JSON_DICT,
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    ARROW,
#endif
};

#ifdef DFTRACER_UTILS_ENABLE_ARROW
struct ArrowIteratorState {
    using BatchType =
        dftracer::utils::utilities::common::arrow::ArrowExportResult;
    std::shared_ptr<dftracer::utils::coro::Channel<BatchType>> channel;
    std::mutex error_mtx;
    std::exception_ptr error;
    std::atomic<bool> cancelled{false};
    std::size_t memory_budget_bytes = 0;
    std::atomic<std::size_t> bytes_in_queue{0};
    std::shared_future<void> task_future;

    void set_error(std::exception_ptr e) {
        std::lock_guard<std::mutex> lock(error_mtx);
        if (!error) error = e;
    }
};
#endif

using ArgsValue = dftracer::utils::utilities::composites::dft::ArgsValue;
using ArgsMap = dftracer::utils::utilities::composites::dft::ArgsMap;

struct JsonDictEvent {
    ArgsMap top;
    ArgsMap args;
};

struct JsonDictBatch {
    std::vector<JsonDictEvent> events;
};

struct JsonDictIteratorState {
    std::shared_ptr<dftracer::utils::coro::Channel<JsonDictBatch>> channel;
    std::mutex error_mtx;
    std::exception_ptr error;
    std::atomic<bool> cancelled{false};
    std::size_t memory_budget_bytes = 0;
    std::atomic<std::size_t> bytes_in_queue{0};
    std::shared_future<void> task_future;

    void set_error(std::exception_ptr e) {
        std::lock_guard<std::mutex> lock(error_mtx);
        if (!error) error = e;
    }
};

using dftracer::utils::python::MemoryViewBatchIteratorState;
using dftracer::utils::python::MemoryViewBatchObject;
using dftracer::utils::python::MemoryViewBatchType;

typedef struct {
    PyObject_HEAD std::shared_ptr<MemoryViewBatchIteratorState> batch_state;
    PyObject *current_batch;
    Py_ssize_t batch_index;
    std::shared_ptr<JsonDictIteratorState> json_dict_state;
    std::shared_ptr<JsonDictBatch> json_dict_current_batch;
    Py_ssize_t json_dict_index;
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    std::shared_ptr<ArrowIteratorState> arrow_state;
#endif
    IteratorMode mode;
} TraceReaderIteratorObject;

extern PyTypeObject TraceReaderIteratorType;
int init_trace_reader_iterator(PyObject *m);

#endif  // DFTRACER_UTILS_PYTHON_TRACE_READER_ITERATOR_H
