// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/python/formats/text_source.h>

#include <utility>

namespace dftracer::utils::python::formats {

namespace {

constexpr std::size_t INPUT_BLOCK = std::size_t{256} << 10;

}  // namespace

TextSource::TextSource(TextSource &&) noexcept = default;
TextSource &TextSource::operator=(TextSource &&) noexcept = default;
TextSource::~TextSource() = default;

dstd::Result<TextSource> TextSource::open(dstd::StringView path) {
    auto file = dstd::File::open(path, {});
    if (!file) return dstd::Unexpected<dstd::Error>(file.error());
    TextSource source;
    source.file_ = std::move(*file);
    auto size = source.file_.size();
    if (!size) return dstd::Unexpected<dstd::Error>(size.error());
    source.file_size_ = *size;
    unsigned char magic[2] = {0, 0};
    auto got = source.file_.read_at(0, dstd::Span<std::uint8_t>(magic, 2));
    if (!got) return dstd::Unexpected<dstd::Error>(got.error());
    source.gzip_ = *got == 2 && magic[0] == 0x1f && magic[1] == 0x8b;
    if (source.gzip_) {
        source.inflater_ =
            dstd::make_unique<fileio::compress::GzipStreamInflater>();
        if (!source.inflater_->init(15 + 16))
            return common::make_error(common::ErrorCode::Compression,
                                      "cannot start the gzip reader");
        source.input_.resize(INPUT_BLOCK);
    }
    return source;
}

dstd::Result<std::size_t> TextSource::read(char *out, std::size_t capacity) {
    if (!gzip_) {
        auto got = file_.read_at(
            at_, dstd::Span<std::uint8_t>(reinterpret_cast<std::uint8_t *>(out),
                                          capacity));
        if (!got) return dstd::Unexpected<dstd::Error>(got.error());
        at_ += *got;
        return *got;
    }
    using fileio::compress::InflateStatus;
    std::size_t produced = 0;
    while (produced < capacity && !ended_) {
        if (inflater_->avail_in() == 0) {
            auto more = file_.read_at(
                at_, dstd::Span<std::uint8_t>(input_.data(), input_.size()));
            if (!more) return dstd::Unexpected<dstd::Error>(more.error());
            if (*more == 0) {
                ended_ = true;
                if (in_member_)
                    return common::make_error(common::ErrorCode::Compression,
                                              "the gzip file is truncated");
                break;
            }
            at_ += *more;
            inflater_->set_input(input_.data(), *more);
        }
        inflater_->set_output(reinterpret_cast<unsigned char *>(out) + produced,
                              capacity - produced);
        const InflateStatus status = inflater_->inflate();
        produced = capacity - inflater_->avail_out();
        in_member_ = true;
        if (status == InflateStatus::StreamEnd) {
            in_member_ = false;
            if (!inflater_->reset())
                return common::make_error(common::ErrorCode::Compression,
                                          "cannot read the next gzip member");
        } else if (status == InflateStatus::DataError ||
                   status == InflateStatus::Error) {
            return common::make_error(common::ErrorCode::Compression,
                                      "the gzip data is not valid");
        } else if (status == InflateStatus::BufError &&
                   inflater_->avail_in() != 0 && inflater_->avail_out() != 0) {
            return common::make_error(common::ErrorCode::Compression,
                                      "the gzip data is not valid");
        }
    }
    return produced;
}

}  // namespace dftracer::utils::python::formats
