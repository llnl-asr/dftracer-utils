#ifndef DFTRACER_UTILS_UTILITIES_IO_LINES_ITERATOR_H
#define DFTRACER_UTILS_UTILITIES_IO_LINES_ITERATOR_H

#include <dftracer/utils/utilities/io/lines/line_types.h>

#include <iterator>

namespace dftracer::utils::utilities::io::lines {

/**
 * @brief Generic input iterator template for line-by-line access.
 *
 * This template provides a reusable iterator implementation for any container
 * that implements has_next() and next() methods. It eliminates code duplication
 * across LineRange, LineBytesRange, and IndexedFileLineIterator.
 *
 * @tparam Container The container type that provides has_next() and next()
 * methods
 *
 * Requirements for Container:
 * - bool has_next() const - Check if more items available
 * - Line next() - Get the next line
 *
 * Usage:
 * @code
 * class MyLineContainer {
 *    public:
 *     using Iterator = LineIterator<MyLineContainer>;
 *
 *     bool has_next() const { ... }
 *     Line next() { ... }
 *
 *     Iterator begin() { return Iterator(this, false); }
 *     Iterator end() { return Iterator(nullptr, true); }
 * };
 *
 * // Now you can use range-based for loops:
 * MyLineContainer container;
 * for (const auto& line : container) {
 *     std::cout << line.content << "\n";
 * }
 * @endcode
 */
template <typename Container>
class LineIterator {
   private:
    Container* parent_;
    Line current_line_;
    bool is_end_;

   public:
    // Iterator traits for STL compatibility
    using iterator_category = std::input_iterator_tag;
    using value_type = Line;
    using difference_type = std::ptrdiff_t;
    using pointer = const Line*;
    using reference = const Line&;

    /**
     * @brief Construct an iterator.
     * @param parent Pointer to the parent container (nullptr for end iterator)
     * @param is_end True if this is an end iterator
     */
    LineIterator(Container* parent, bool is_end)
        : parent_(parent),
          current_line_(std::string_view{}, 0),
          is_end_(is_end) {
        if (!is_end_ && parent_ && parent_->has_next()) {
            current_line_ = parent_->next();
        } else {
            is_end_ = true;
        }
    }

    /**
     * @brief Dereference operator.
     * @return Reference to the current line
     */
    reference operator*() const { return current_line_; }

    /**
     * @brief Member access operator.
     * @return Pointer to the current line
     */
    pointer operator->() const { return &current_line_; }

    /**
     * @brief Pre-increment operator.
     * @return Reference to this iterator after incrementing
     */
    LineIterator& operator++() {
        if (parent_ && parent_->has_next()) {
            current_line_ = parent_->next();
        } else {
            is_end_ = true;
        }
        return *this;
    }

    /**
     * @brief Post-increment operator.
     * @return Copy of this iterator before incrementing
     */
    LineIterator operator++(int) {
        LineIterator tmp = *this;
        ++(*this);
        return tmp;
    }

    /**
     * @brief Equality comparison.
     * @param other The iterator to compare with
     * @return True if iterators are equal
     */
    bool operator==(const LineIterator& other) const {
        // Two end iterators are always equal
        if (is_end_ && other.is_end_) {
            return true;
        }
        // End iterator is not equal to non-end iterator
        if (is_end_ != other.is_end_) {
            return false;
        }
        // Two non-end iterators are equal if they point to the same parent
        // and have the same line number
        return parent_ == other.parent_ &&
               current_line_.line_number == other.current_line_.line_number;
    }

    /**
     * @brief Inequality comparison.
     * @param other The iterator to compare with
     * @return True if iterators are not equal
     */
    bool operator!=(const LineIterator& other) const {
        return !(*this == other);
    }
};

}  // namespace dftracer::utils::utilities::io::lines

#endif  // DFTRACER_UTILS_UTILITIES_IO_LINES_ITERATOR_H
