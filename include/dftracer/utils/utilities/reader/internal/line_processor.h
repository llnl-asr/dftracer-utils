#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_LINE_PROCESSOR_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_LINE_PROCESSOR_H

#ifdef __cplusplus
#include <cstddef>

namespace dftracer::utils::utilities::reader::internal {

/**
 * Base class for processing lines during streaming read operations.
 * Provides zero-copy callback interface for line-by-line processing.
 */
class LineProcessor {
   public:
    virtual ~LineProcessor() = default;

    /**
     * Process a single line of data.
     * @param data Pointer to line data (not null-terminated)
     * @param length Length of the line data in bytes
     * @return true to continue processing, false to stop early
     */
    virtual bool process(const char* data, std::size_t length) = 0;

    /**
     * Called before processing begins.
     * Override to perform initialization.
     * @param start_line Starting line number (1-based)
     * @param end_line Ending line number (1-based)
     */
    virtual void begin([[maybe_unused]] std::size_t start_line,
                       [[maybe_unused]] std::size_t end_line) {}

    /**
     * Called after processing completes.
     * Override to perform cleanup or finalization.
     */
    virtual void end() {}
};

/**
 * Bridge class that wraps C callback as C++ LineProcessor.
 * Internal use only - converts C function pointer to C++ virtual interface.
 */
class CLineProcessor : public LineProcessor {
   private:
    int (*callback_)(const char* data, std::size_t length, void* user_data);
    void* user_data_;

   public:
    CLineProcessor(int (*callback)(const char*, std::size_t, void*),
                   void* user_data)
        : callback_(callback), user_data_(user_data) {}

    bool process(const char* data, std::size_t length) override {
        // Convert C++ call to C callback
        int result = callback_(data, length, user_data_);
        return result != 0;  // Non-zero means continue
    }
};

}  // namespace dftracer::utils::utilities::reader::internal

extern "C" {
#endif

#include <stddef.h>

/**
 * C callback function type for line processing.
 * @param data Pointer to line data (not null-terminated)
 * @param length Length of the line data in bytes
 * @param user_data User-provided data pointer
 * @return Non-zero to continue processing, 0 to stop early
 */
typedef int (*dft_line_processor_callback_t)(const char* data, size_t length,
                                             void* user_data);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_LINE_PROCESSOR_H
