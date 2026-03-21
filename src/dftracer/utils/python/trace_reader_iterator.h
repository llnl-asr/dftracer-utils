#ifndef DFTRACER_UTILS_PYTHON_TRACE_READER_ITERATOR_H
#define DFTRACER_UTILS_PYTHON_TRACE_READER_ITERATOR_H

#include <Python.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>

typedef struct {
    PyObject_HEAD dftracer::utils::utilities::common::arrow::ArrowExportResult
        *result;
} ArrowBatchCapsuleObject;

extern PyTypeObject ArrowBatchCapsuleType;
#endif

enum class IteratorMode {
    LINES,
    RAW,
    JSON,
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    ARROW,
#endif
};

struct IteratorState {
    std::queue<std::optional<std::string>> queue;
    std::mutex mtx;
    std::condition_variable cv_producer;
    std::condition_variable cv_consumer;
    std::exception_ptr error;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false};
    std::size_t max_queue_size = 64;
};

#ifdef DFTRACER_UTILS_ENABLE_ARROW
struct ArrowIteratorState {
    using BatchItem = std::optional<
        dftracer::utils::utilities::common::arrow::ArrowExportResult>;
    std::queue<BatchItem> queue;
    std::mutex mtx;
    std::condition_variable cv_producer;
    std::condition_variable cv_consumer;
    std::exception_ptr error;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false};
    std::size_t max_queue_size = 8;
};
#endif

typedef struct {
    PyObject_HEAD std::shared_ptr<IteratorState> state;
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    std::shared_ptr<ArrowIteratorState> arrow_state;
#endif
    IteratorMode mode;
} TraceReaderIteratorObject;

extern PyTypeObject TraceReaderIteratorType;
int init_trace_reader_iterator(PyObject *m);

#endif  // DFTRACER_UTILS_PYTHON_TRACE_READER_ITERATOR_H
