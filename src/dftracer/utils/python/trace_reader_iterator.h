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

enum class IteratorMode { LINES, RAW, JSON };

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

typedef struct {
    PyObject_HEAD std::shared_ptr<IteratorState> state;
    IteratorMode mode;
} TraceReaderIteratorObject;

extern PyTypeObject TraceReaderIteratorType;
int init_trace_reader_iterator(PyObject *m);

#endif  // DFTRACER_UTILS_PYTHON_TRACE_READER_ITERATOR_H
