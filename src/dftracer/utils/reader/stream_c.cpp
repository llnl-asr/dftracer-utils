#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/reader/stream.h>

#include <memory>

using namespace dftracer::utils;

// Helper function to cast stream handle to C++ object
static std::shared_ptr<ReaderStream>* cast_stream(dft_reader_stream_t stream) {
    return static_cast<std::shared_ptr<ReaderStream>*>(stream);
}

extern "C" {

size_t dft_reader_stream_read(dft_reader_stream_t stream, char* buffer,
                              size_t buffer_size) {
    if (!stream || !buffer || buffer_size == 0) {
        DFTRACER_UTILS_LOG_ERROR("%s", "Invalid stream handle or buffer");
        return 0;
    }

    try {
        return (*cast_stream(stream))->read(buffer, buffer_size);
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Stream read failed: %s", e.what());
        return 0;
    }
}

int dft_reader_stream_done(dft_reader_stream_t stream) {
    if (!stream) {
        DFTRACER_UTILS_LOG_ERROR("%s", "Invalid stream handle");
        return 1;  // Return done=true for invalid stream
    }

    try {
        return (*cast_stream(stream))->done() ? 1 : 0;
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Stream done check failed: %s", e.what());
        return 1;  // Return done=true on error
    }
}

void dft_reader_stream_reset(dft_reader_stream_t stream) {
    if (!stream) {
        DFTRACER_UTILS_LOG_ERROR("%s", "Invalid stream handle");
        return;
    }

    try {
        (*cast_stream(stream))->reset();
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Stream reset failed: %s", e.what());
    }
}

void dft_reader_stream_destroy(dft_reader_stream_t stream) {
    if (stream) {
        delete cast_stream(stream);
    }
}

}  // extern "C"
