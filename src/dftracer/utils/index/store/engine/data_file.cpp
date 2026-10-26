#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/index/store/engine/data_file.h>
#include <dftracer/utils/index/store/engine/io_fault.h>
#include <dftracer/utils/index/store/engine/scope_directory.h>
#include <dftracer/utils/index/store/engine/varint.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <utility>

namespace dftracer::utils::index::store::engine {

namespace {

namespace dstd = dftracer::utils::dstd;
namespace compress = dftracer::utils::utilities::fileio::compress;

constexpr std::string_view FILE_MAGIC = "DFTUSTR0";
constexpr std::string_view TRAILER_MAGIC = "DFTUEND0";
constexpr std::uint32_t FORMAT_VERSION = 0;
constexpr std::uint64_t RECORD_HEADER_BYTES = 40;

struct HeaderInfo {
    bool magic_ok = false;
    bool header_crc_ok = false;
    std::uint32_t format_version = 0;
    std::uint32_t page_size = 0;
    std::uint64_t writer_random = 0;
    std::uint32_t pid = 0;
    dstd::String host;
    dstd::String label;
    std::uint64_t creation_time = 0;
    std::uint64_t tail_offset = 0;
    std::uint32_t tail_kind = 0;
    std::uint32_t tail_crc = 0;
};

dstd::Unexpected<dstd::Error> parse_error() {
    return dstd::Unexpected<dstd::Error>(dstd::Error(dstd::Condition::Parse));
}

dstd::Unexpected<dstd::Error> invalid_error() {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::InvalidArgument));
}

dstd::Unexpected<dstd::Error> io_error() {
    return dstd::Unexpected<dstd::Error>(dstd::Error(dstd::Condition::Io));
}

dstd::Unexpected<dstd::Error> unsupported_error() {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::Unsupported));
}

io_fault::WriteKind fault_kind_of(RecordKind kind) {
    return kind == RecordKind::Footer ? io_fault::WriteKind::Footer
                                      : io_fault::WriteKind::Record;
}

std::string path_string(dstd::StringView path) {
    return std::string(path.data(), path.size());
}

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    const std::uint64_t remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
}

dstd::Expected<void, dstd::Error> pwrite_all(int fd, const std::uint8_t* data,
                                             std::size_t size,
                                             std::uint64_t offset) {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t written = ::pwrite(fd, data + done, size - done,
                                         static_cast<off_t>(offset + done));
        if (written < 0) {
            if (errno == EINTR) continue;
            return io_error();
        }
        if (written == 0) return io_error();
        done += static_cast<std::size_t>(written);
    }
    return {};
}

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> pread_all(
    int fd, std::uint64_t offset, std::size_t size) {
    dstd::Vector<std::uint8_t> out(size);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t read = ::pread(fd, out.data() + done, size - done,
                                     static_cast<off_t>(offset + done));
        if (read < 0) {
            if (errno == EINTR) continue;
            return io_error();
        }
        if (read == 0) return parse_error();
        done += static_cast<std::size_t>(read);
    }
    return out;
}

ByteView bytes_of(const dstd::Vector<std::uint8_t>& bytes) {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

std::uint64_t random_writer_id() {
    const auto now =
        std::chrono::steady_clock::now().time_since_epoch().count();
    std::uint64_t seed = static_cast<std::uint64_t>(now) ^
                         (static_cast<std::uint64_t>(::getpid()) << 32U) ^
                         reinterpret_cast<std::uintptr_t>(&seed);
    std::mt19937_64 rng(seed);
    return rng() ^ (rng() << 32U);
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

dstd::Expected<HeaderInfo, dstd::Error> read_header(int fd) {
    HeaderInfo info;
    auto bytes = pread_all(fd, 0, FILE_HEADER_BYTES);
    if (!bytes) return dstd::Unexpected<dstd::Error>(bytes.error());
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes->data());

    if (std::memcmp(data, FILE_MAGIC.data(), FILE_MAGIC.size()) != 0) {
        return info;
    }
    info.magic_ok = true;
    info.format_version = read_u32_le(data + 8);
    if (info.format_version != FORMAT_VERSION) return info;

    const std::uint32_t stored_crc = read_u32_le(data + 12);
    dstd::Vector<std::uint8_t> crc_bytes = *bytes;
    write_u32_le(crc_bytes.data() + 12, 0);
    const std::uint32_t crc =
        compress::gzip_crc32(0, crc_bytes.data(), crc_bytes.size());
    info.header_crc_ok = stored_crc == crc;

    const std::uint32_t page_size = read_u32_le(data + 16);
    const std::uint64_t creation_time = read_u64_le(data + 24);
    const std::uint64_t writer_random = read_u64_le(data + 32);
    const std::uint32_t pid = read_u32_le(data + 40);
    const std::uint16_t host_length = read_u16_le(data + 44);
    const std::uint16_t label_length = read_u16_le(data + 46);

    const std::uint64_t strings_end =
        static_cast<std::uint64_t>(48) + host_length + label_length + 16;
    if (host_length > 255 || label_length > 255 ||
        strings_end > FILE_HEADER_BYTES) {
        return info;
    }

    info.page_size = page_size;
    info.writer_random = writer_random;
    info.pid = pid;
    info.host.assign(reinterpret_cast<const char*>(data + 48), host_length);
    info.label.assign(reinterpret_cast<const char*>(data + 48 + host_length),
                      label_length);
    info.creation_time = creation_time;
    const std::size_t tail_at = 48 + host_length + label_length;
    info.tail_offset = read_u64_le(data + tail_at);
    info.tail_kind = read_u32_le(data + tail_at + 8);
    info.tail_crc = read_u32_le(data + tail_at + 12);
    return info;
}

void fill_header_fields(FileIndex& index, const HeaderInfo& info) {
    index.page_size = info.page_size;
    index.writer_random = info.writer_random;
    index.pid = info.pid;
    index.host = info.host;
    index.label = info.label;
    index.creation_time = info.creation_time;
}

bool tail_layout_is_valid(std::uint64_t file_size, std::uint64_t tail_offset,
                          std::uint32_t tail_kind) {
    return tail_kind >= 1 && tail_kind <= 3 &&
           tail_offset >= FILE_HEADER_BYTES &&
           tail_offset % FILE_HEADER_BYTES == 0 && tail_offset <= file_size &&
           file_size - tail_offset >= RECORD_HEADER_BYTES;
}

dstd::Expected<std::uint64_t, dstd::Error> read_record_length(
    int fd, std::uint64_t file_size, std::uint64_t offset) {
    if (offset < FILE_HEADER_BYTES || offset % FILE_HEADER_BYTES != 0 ||
        offset > file_size || file_size - offset < RECORD_HEADER_BYTES) {
        return parse_error();
    }
    auto head = pread_all(fd, offset, RECORD_HEADER_BYTES);
    if (!head) return dstd::Unexpected<dstd::Error>(head.error());
    const auto* data = reinterpret_cast<const std::uint8_t*>(head->data());
    constexpr std::string_view record_magic = "DFTUREC0";
    if (std::memcmp(data, record_magic.data(), record_magic.size()) != 0) {
        return parse_error();
    }
    const std::uint32_t kind = read_u32_le(data + 8);
    if (kind < 1 || kind > 3) return parse_error();
    const std::uint32_t body_length = read_u32_le(data + 12);
    const std::uint64_t crc_end = RECORD_HEADER_BYTES + body_length + 4;
    const std::uint64_t padded_size = align_up(crc_end, FILE_HEADER_BYTES);
    if (padded_size < crc_end || padded_size > file_size - offset) {
        return parse_error();
    }
    return padded_size;
}

dstd::Expected<Record, dstd::Error> decode_record_at(ByteView bytes,
                                                     std::uint64_t offset) {
    auto record = decode_record(bytes);
    if (!record) return dstd::Unexpected<dstd::Error>(record.error());
    if (record->offset != offset) return parse_error();
    return record;
}

dstd::Expected<Record, dstd::Error> read_record_at(int fd,
                                                   std::uint64_t file_size,
                                                   std::uint64_t offset,
                                                   std::size_t& records_read) {
    auto length = read_record_length(fd, file_size, offset);
    if (!length) return dstd::Unexpected<dstd::Error>(length.error());
    ++records_read;
    auto bytes = pread_all(fd, offset, *length);
    if (!bytes) return dstd::Unexpected<dstd::Error>(bytes.error());
    return decode_record_at(bytes_of(*bytes), offset);
}

dstd::Expected<dstd::Vector<Record>, dstd::Error> scan_records(
    int fd, std::uint64_t file_size, std::size_t& records_read) {
    dstd::Vector<Record> records;
    for (std::uint64_t offset = FILE_HEADER_BYTES;
         offset <= file_size && file_size - offset >= RECORD_HEADER_BYTES;
         offset += FILE_HEADER_BYTES) {
        auto length = read_record_length(fd, file_size, offset);
        if (!length) {
            if (length.error().is(dstd::Condition::Io)) {
                return dstd::Unexpected<dstd::Error>(length.error());
            }
            continue;
        }
        auto bytes = pread_all(fd, offset, *length);
        if (!bytes) {
            if (bytes.error().is(dstd::Condition::Io)) {
                return dstd::Unexpected<dstd::Error>(bytes.error());
            }
            continue;
        }
        ++records_read;
        auto record = decode_record_at(bytes_of(*bytes), offset);
        if (record) records.push_back(std::move(*record));
    }
    std::sort(records.begin(), records.end(),
              [](const Record& a, const Record& b) {
                  if (a.sequence != b.sequence) {
                      return a.sequence > b.sequence;
                  }
                  return a.offset > b.offset;
              });
    return records;
}

dstd::Expected<dstd::Vector<Record>, dstd::Error> walk_back(
    int fd, std::uint64_t file_size, Record start_record,
    std::size_t& records_read) {
    dstd::Vector<Record> chain;
    chain.push_back(std::move(start_record));
    const std::size_t max_steps =
        static_cast<std::size_t>(file_size / FILE_HEADER_BYTES + 1);
    while (chain.size() <= max_steps) {
        const Record& current = chain.back();
        if (current.kind == RecordKind::Checkpoint ||
            current.kind == RecordKind::Footer) {
            break;
        }
        if (current.sequence == 1) break;
        const std::uint64_t prev_offset = current.prev_offset;
        if (prev_offset == 0 || prev_offset >= current.offset ||
            prev_offset < FILE_HEADER_BYTES ||
            prev_offset % FILE_HEADER_BYTES != 0 || prev_offset > file_size ||
            file_size - prev_offset < RECORD_HEADER_BYTES) {
            return parse_error();
        }
        auto record = read_record_at(fd, file_size, prev_offset, records_read);
        if (!record) return dstd::Unexpected<dstd::Error>(record.error());
        if (record->sequence != current.sequence - 1) return parse_error();
        chain.push_back(std::move(*record));
    }
    if (chain.size() > max_steps) return parse_error();
    return chain;
}

dstd::Expected<dstd::Vector<Record>, dstd::Error> recover_from_scan(
    int fd, std::uint64_t file_size, const dstd::Vector<Record>& scanned,
    std::size_t& records_read) {
    std::uint64_t last_sequence = 0;
    bool have_sequence = false;
    for (const Record& start : scanned) {
        if (have_sequence && start.sequence == last_sequence) continue;
        have_sequence = true;
        last_sequence = start.sequence;
        auto chain = walk_back(fd, file_size, start, records_read);
        if (chain) return chain;
        if (chain.error().is(dstd::Condition::Io)) {
            return dstd::Unexpected<dstd::Error>(chain.error());
        }
    }
    return parse_error();
}

void apply_record(FileIndex& index, const Record& record) {
    if (record.kind == RecordKind::Checkpoint ||
        record.kind == RecordKind::Footer) {
        index.namespaces.clear();
        index.scopes.clear();
        index.copies.clear();
        index.drops.clear();
        index.page_refs.clear();
        index.commit_refs.clear();
    }
    for (const RecordItem& item : record.items) {
        if (const auto* namespace_item = std::get_if<NamespaceItem>(&item)) {
            index.namespaces.push_back(*namespace_item);
        } else if (const auto* scope_item = std::get_if<ScopeItem>(&item)) {
            index.scopes.push_back(*scope_item);
        } else if (const auto* copy_item = std::get_if<CopyItem>(&item)) {
            index.copies.push_back(*copy_item);
        } else if (const auto* drop_item = std::get_if<DropItem>(&item)) {
            index.drops.push_back(*drop_item);
        } else if (const auto* page_ref_item =
                       std::get_if<PageRefItem>(&item)) {
            index.page_refs.push_back(
                {page_ref_item->sequence, *page_ref_item});
        } else if (const auto* commit_ref_item =
                       std::get_if<CommitRefItem>(&item)) {
            index.commit_refs.push_back(*commit_ref_item);
        } else if (const auto* file_tail = std::get_if<FileTail>(&item)) {
            index.tail = *file_tail;
        }
    }
    index.last_sequence = record.sequence;
    index.closed = record.kind == RecordKind::Footer;
}

void apply_chain(FileIndex& index, const dstd::Vector<Record>& chain) {
    for (std::size_t i = chain.size(); i > 0; --i) {
        apply_record(index, chain[i - 1]);
    }
}

}  // namespace

dstd::String data_file_name(dstd::StringView host, std::uint32_t pid,
                            std::uint64_t random, dstd::StringView label) {
    const auto keep = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    };

    dstd::String out;
    out.reserve(host.size() + label.size() + 32);
    out.push_back('w');
    out.push_back('-');
    for (const char c : host) {
        if (keep(c)) out.push_back(c);
    }
    out.push_back('-');
    const std::string pid_text = std::to_string(pid);
    out.append(pid_text.data(), pid_text.size());
    out.push_back('-');
    std::array<char, 17> hex{};
    std::snprintf(hex.data(), hex.size(), "%016llx",
                  static_cast<unsigned long long>(random));
    out.append(hex.data(), 16);
    if (!label.empty()) {
        bool has_label = false;
        for (const char c : label) {
            if (!keep(c)) continue;
            if (!has_label) {
                out.push_back('-');
                has_label = true;
            }
            out.push_back(c);
        }
    }
    out.append(".dfs");
    return out;
}

dstd::Expected<DataFileWriter, dstd::Error> DataFileWriter::create(
    dstd::StringView path, std::uint32_t page_size, dstd::StringView host,
    dstd::StringView label) {
    if (page_size < FILE_HEADER_BYTES || page_size % FILE_HEADER_BYTES != 0) {
        return invalid_error();
    }
    if (host.size() > 255 || label.size() > 255) return invalid_error();

    const std::string path_std = path_string(path);
    const int fd =
        ::open(path_std.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return io_error();

    DataFileWriter writer;
    writer.fd_ = fd;
    writer.page_size_ = page_size;
    writer.host_.assign(host.data(), host.size());
    writer.label_.assign(label.data(), label.size());
    writer.pid_ = static_cast<std::uint32_t>(::getpid());
    writer.writer_random_ = random_writer_id();
    writer.creation_time_ = now_ns();

    auto header = writer.write_header();
    if (!header) return dstd::Unexpected<dstd::Error>(header.error());
    return writer;
}

DataFileWriter::DataFileWriter(DataFileWriter&& other) noexcept
    : fd_(other.fd_),
      page_size_(other.page_size_),
      next_offset_(other.next_offset_.load(std::memory_order_relaxed)),
      last_sequence_(other.last_sequence_),
      tail_offset_(other.tail_offset_),
      tail_kind_(other.tail_kind_),
      tail_crc_(other.tail_crc_),
      writer_random_(other.writer_random_),
      pid_(other.pid_),
      creation_time_(other.creation_time_),
      host_(std::move(other.host_)),
      label_(std::move(other.label_)),
      pending_items_(std::move(other.pending_items_)),
      all_items_(std::move(other.all_items_)),
      committed_pages_(std::move(other.committed_pages_)),
      closed_(other.closed_.load(std::memory_order_relaxed)),
      latched_(other.latched_.load(std::memory_order_relaxed)),
      latched_error_(std::move(other.latched_error_)) {
    other.fd_ = -1;
}

DataFileWriter& DataFileWriter::operator=(DataFileWriter&& other) noexcept {
    if (this == &other) return *this;
    if (fd_ >= 0) ::close(fd_);
    fd_ = other.fd_;
    page_size_ = other.page_size_;
    next_offset_.store(other.next_offset_.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
    last_sequence_ = other.last_sequence_;
    tail_offset_ = other.tail_offset_;
    tail_kind_ = other.tail_kind_;
    tail_crc_ = other.tail_crc_;
    writer_random_ = other.writer_random_;
    pid_ = other.pid_;
    creation_time_ = other.creation_time_;
    host_ = std::move(other.host_);
    label_ = std::move(other.label_);
    pending_items_ = std::move(other.pending_items_);
    all_items_ = std::move(other.all_items_);
    committed_pages_ = std::move(other.committed_pages_);
    closed_.store(other.closed_.load(std::memory_order_relaxed),
                  std::memory_order_relaxed);
    latched_.store(other.latched_.load(std::memory_order_relaxed),
                   std::memory_order_relaxed);
    latched_error_ = std::move(other.latched_error_);
    other.fd_ = -1;
    return *this;
}

DataFileWriter::~DataFileWriter() {
    if (fd_ >= 0) ::close(fd_);
}

void DataFileWriter::latch(const dstd::Error& error) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    if (latched_.load(std::memory_order_relaxed)) return;
    latched_error_ = error;
    latched_.store(true, std::memory_order_release);
}

dstd::Error DataFileWriter::latched_result() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return latched_error_;
}

dstd::Expected<void, dstd::Error> DataFileWriter::write_header() {
    if (latched()) return dstd::Unexpected<dstd::Error>(latched_result());
    dstd::Vector<std::uint8_t> header(FILE_HEADER_BYTES, 0);
    std::memcpy(header.data(), FILE_MAGIC.data(), FILE_MAGIC.size());
    write_u32_le(header.data() + 8, FORMAT_VERSION);
    write_u32_le(header.data() + 16, page_size_);
    write_u64_le(header.data() + 24, creation_time_);
    write_u64_le(header.data() + 32, writer_random_);
    write_u32_le(header.data() + 40, pid_);
    write_u16_le(header.data() + 44, static_cast<std::uint16_t>(host_.size()));
    write_u16_le(header.data() + 46, static_cast<std::uint16_t>(label_.size()));
    std::memcpy(header.data() + 48, host_.data(), host_.size());
    std::memcpy(header.data() + 48 + host_.size(), label_.data(),
                label_.size());

    const std::size_t tail_at = 48 + host_.size() + label_.size();
    write_u64_le(header.data() + tail_at, tail_offset_);
    write_u32_le(header.data() + tail_at + 8, tail_kind_);
    write_u32_le(header.data() + tail_at + 12, tail_crc_);

    const std::uint32_t crc =
        compress::gzip_crc32(0, header.data(), header.size());
    write_u32_le(header.data() + 12, crc);
    auto written = pwrite_all(fd_, header.data(), header.size(), 0);
    if (written) {
        io_fault::on_write(fd_, io_fault::WriteKind::Header, 0, header.data(),
                           header.size());
    } else {
        latch(written.error());
    }
    return written;
}

dstd::Expected<PageRef, dstd::Error> DataFileWriter::append_page(
    ByteView bytes) {
    if (latched()) return dstd::Unexpected<dstd::Error>(latched_result());
    if (closed_.load(std::memory_order_acquire)) return invalid_error();
    if (bytes.empty() || bytes.size() > page_size_ ||
        bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        return invalid_error();
    }
    // The reservation records the page range; the write below is its bytes.
    const std::uint64_t offset =
        reserve_range(bytes.size(), page_size_, io_fault::WriteKind::Page);
    PageRef ref{offset, static_cast<std::uint32_t>(bytes.size()), 0};
    ref.crc = compress::gzip_crc32(0, bytes.data(), bytes.size());
    auto written =
        pwrite_all(fd_, reinterpret_cast<const std::uint8_t*>(bytes.data()),
                   bytes.size(), offset);
    if (!written) {
        latch(written.error());
        return dstd::Unexpected<dstd::Error>(written.error());
    }
    return ref;
}

dstd::Expected<PageRef, dstd::Error> DataFileWriter::append_big(
    ByteView bytes) {
    if (latched()) return dstd::Unexpected<dstd::Error>(latched_result());
    if (closed_.load(std::memory_order_acquire)) return invalid_error();
    if (bytes.empty() ||
        bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        return invalid_error();
    }
    const std::uint64_t offset = reserve_range(bytes.size(), FILE_HEADER_BYTES,
                                               io_fault::WriteKind::Big);
    PageRef ref{offset, static_cast<std::uint32_t>(bytes.size()), 0};
    ref.crc = compress::gzip_crc32(0, bytes.data(), bytes.size());
    auto written =
        pwrite_all(fd_, reinterpret_cast<const std::uint8_t*>(bytes.data()),
                   bytes.size(), offset);
    if (!written) {
        latch(written.error());
        return dstd::Unexpected<dstd::Error>(written.error());
    }
    io_fault::on_write(fd_, io_fault::WriteKind::Big, offset,
                       reinterpret_cast<const std::uint8_t*>(bytes.data()),
                       bytes.size());
    return ref;
}

std::uint64_t DataFileWriter::reserve_range(std::uint64_t length,
                                            std::uint64_t align,
                                            io_fault::WriteKind kind) {
    std::uint64_t current = next_offset_.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint64_t start = align_up(current, align);
        if (next_offset_.compare_exchange_weak(current, start + length,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
            if (kind == io_fault::WriteKind::Page) {
                io_fault::on_write(fd_, kind, start, nullptr, length);
            }
            return start;
        }
    }
}

void DataFileWriter::add(RecordItem item) {
    if (latched() || closed_.load(std::memory_order_acquire)) return;
    // A page ref lives in its commit's record only: a checkpoint and the footer
    // list the commit by the summary commit() adds once the record is written.
    if (!std::holds_alternative<PageRefItem>(item)) all_items_.push_back(item);
    pending_items_.push_back(std::move(item));
}

RecordItems DataFileWriter::items_with_pending_pages() const {
    RecordItems items = all_items_;
    for (const RecordItem& item : pending_items_) {
        if (std::holds_alternative<PageRefItem>(item)) items.push_back(item);
    }
    return items;
}

dstd::Expected<FileTail, dstd::Error> DataFileWriter::build_blocks(
    const RecordItems& items, std::uint64_t base,
    dstd::Vector<std::uint8_t>& out) const {
    // Each block goes at the end of `out`, which lands at `base` in the file.
    // An empty block takes no bytes and a zero handle.
    const auto place = [base, &out](const dstd::Vector<std::uint8_t>& bytes) {
        if (bytes.empty()) return BlockHandle{};
        BlockHandle handle;
        handle.offset = base + out.size();
        handle.length = static_cast<std::uint32_t>(bytes.size());
        handle.crc = compress::gzip_crc32(0, bytes.data(), bytes.size());
        out.insert(out.end(), bytes.begin(), bytes.end());
        return handle;
    };
    // Every page ref the file holds a section of, sorted by offset and
    // deduplicated, so the index of a page in this order names the page.
    dstd::Vector<const PageRefItem*> pages;
    for (const PageRefItem& page : committed_pages_) {
        if (!page.sections.empty()) pages.push_back(&page);
    }
    for (const RecordItem& item : items) {
        const auto* page = std::get_if<PageRefItem>(&item);
        if (page != nullptr && !page->sections.empty()) pages.push_back(page);
    }
    std::stable_sort(pages.begin(), pages.end(),
                     [](const PageRefItem* a, const PageRefItem* b) {
                         return a->offset < b->offset;
                     });
    std::size_t kept = 0;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (kept > 0 && pages[kept - 1]->offset == pages[i]->offset) continue;
        pages[kept++] = pages[i];
    }
    pages.resize(kept);
    if (pages.size() > std::numeric_limits<std::uint32_t>::max()) {
        return invalid_error();
    }
    // One section block per copy of one commit, in page order: every section
    // sorted once by commit, copy, page and place in the page, cut in runs.
    struct SectionKey {
        std::uint64_t sequence = 0;
        std::uint32_t copy_id = 0;
    };
    const auto less = [](const SectionKey& a, const SectionKey& b) {
        if (a.sequence != b.sequence) return a.sequence < b.sequence;
        return a.copy_id < b.copy_id;
    };
    struct Placed {
        SectionKey key;
        std::uint32_t page = 0;
        const PageRefItem* ref = nullptr;
        const PageSection* section = nullptr;
    };
    dstd::Vector<Placed> placed;
    for (std::size_t page = 0; page < pages.size(); ++page) {
        for (const PageSection& section : pages[page]->sections) {
            placed.push_back({{pages[page]->sequence, section.copy_id},
                              static_cast<std::uint32_t>(page),
                              pages[page],
                              &section});
        }
    }
    std::sort(placed.begin(), placed.end(),
              [&less](const Placed& a, const Placed& b) {
                  if (less(a.key, b.key)) return true;
                  if (less(b.key, a.key)) return false;
                  if (a.page != b.page) return a.page < b.page;
                  return a.section->offset_in_page < b.section->offset_in_page;
              });
    dstd::Vector<std::pair<SectionKey, BlockHandle>> handles;
    for (std::size_t first = 0; first < placed.size();) {
        std::size_t end = first + 1;
        while (end < placed.size() &&
               !less(placed[first].key, placed[end].key)) {
            ++end;
        }
        dstd::Vector<std::uint8_t> bytes;
        append_u32_le(bytes, static_cast<std::uint32_t>(end - first));
        for (std::size_t at = first; at < end; ++at) {
            const Placed& entry = placed[at];
            append_u32_le(bytes, entry.page);
            append_u64_le(bytes, entry.ref->offset);
            append_u32_le(bytes, entry.ref->length);
            append_u32_le(bytes, entry.ref->crc);
            append_u32_le(bytes, entry.section->offset_in_page);
            append_u32_le(bytes, entry.section->length);
            append_u64_le(bytes, entry.section->entry_count);
            for (const auto* key :
                 {&entry.section->first_key, &entry.section->last_key}) {
                encode_varint(bytes, key->size());
                bytes.insert(bytes.end(), key->begin(), key->end());
            }
        }
        handles.push_back({placed[first].key, place(bytes)});
        first = end;
    }
    const auto section_handle = [&handles, &less](std::uint64_t sequence,
                                                  std::uint32_t copy_id) {
        const SectionKey wanted{sequence, copy_id};
        const auto at = std::lower_bound(
            handles.begin(), handles.end(), wanted,
            [&less](const std::pair<SectionKey, BlockHandle>& entry,
                    const SectionKey& key) { return less(entry.first, key); });
        if (at == handles.end() || less(wanted, at->first))
            return BlockHandle{};
        return at->second;
    };

    dstd::Vector<NamespaceItem> namespaces;
    dstd::Vector<ScopeItem> scopes;
    dstd::Vector<CopyItem> copies;
    dstd::Vector<DropItem> drops;
    dstd::Vector<DirectoryCommit> commits;
    for (const RecordItem& item : items) {
        if (const auto* namespace_item = std::get_if<NamespaceItem>(&item)) {
            namespaces.push_back(*namespace_item);
        } else if (const auto* scope = std::get_if<ScopeItem>(&item)) {
            scopes.push_back(*scope);
        } else if (const auto* copy = std::get_if<CopyItem>(&item)) {
            copies.push_back(*copy);
        } else if (const auto* drop = std::get_if<DropItem>(&item)) {
            drops.push_back(*drop);
        } else if (const auto* commit = std::get_if<CommitRefItem>(&item)) {
            for (const CommitCopy& range : commit->copies) {
                commits.push_back(
                    {commit->sequence, range.copy_id, bytes_of(range.first_key),
                     bytes_of(range.last_key),
                     section_handle(commit->sequence, range.copy_id)});
            }
        } else if (const auto* page = std::get_if<PageRefItem>(&item)) {
            for (const PageSection& section : page->sections) {
                commits.push_back(
                    {page->sequence, section.copy_id,
                     bytes_of(section.first_key), bytes_of(section.last_key),
                     section_handle(page->sequence, section.copy_id)});
            }
        }
    }
    auto directory =
        build_scope_directory(namespaces, scopes, copies, drops, commits);
    if (!directory) {
        return dstd::Unexpected<dstd::Error>(directory.error());
    }

    // The directory: blocks of entries in key order, then one separator per
    // child while the table is too large for the footer, then the top table.
    auto leaves =
        split_slot_table(bytes_of(*directory), DIRECTORY_BLOCK_ENTRIES);
    if (!leaves) return dstd::Unexpected<dstd::Error>(leaves.error());
    dstd::Vector<BlockHandle> children;
    children.reserve(leaves->size());
    for (const TableBlock& leaf : *leaves) {
        children.push_back(place(leaf.bytes));
    }
    std::uint32_t levels = 1;
    auto child_blocks = std::move(*leaves);
    auto top = encode_separator_table(child_blocks, children);
    if (!top) return dstd::Unexpected<dstd::Error>(top.error());
    dstd::Vector<std::uint8_t> top_table = std::move(*top);
    while (top_table.size() > DIRECTORY_TOP_BYTES && child_blocks.size() > 1) {
        auto middles =
            split_slot_table(bytes_of(top_table), DIRECTORY_BLOCK_ENTRIES);
        if (!middles) return dstd::Unexpected<dstd::Error>(middles.error());
        dstd::Vector<BlockHandle> handles_at;
        handles_at.reserve(middles->size());
        for (const TableBlock& middle : *middles) {
            handles_at.push_back(place(middle.bytes));
        }
        auto next = encode_separator_table(*middles, handles_at);
        if (!next) return dstd::Unexpected<dstd::Error>(next.error());
        top_table = std::move(*next);
        child_blocks = std::move(*middles);
        ++levels;
    }

    const BlockHandle items_block = place(encode_items(items));

    FileTail tail;
    tail.format_version = FORMAT_VERSION;
    tail.page_size = page_size_;
    tail.writer_random = writer_random_;
    tail.pid = pid_;
    tail.host = host_;
    tail.label = label_;
    tail.creation_time = creation_time_;
    tail.items = items_block;
    tail.page_count = static_cast<std::uint32_t>(pages.size());
    tail.directory_levels = levels;
    tail.top_table = std::move(top_table);
    return tail;
}

void DataFileWriter::add_commit_refs() {
    // One item per section, sorted by sequence and copy so each copy of each
    // commit is one run: one allocation, and each key copied once per copy.
    struct Item {
        std::uint64_t sequence;
        std::uint32_t copy_id;
        const PageSection* section;
    };
    dstd::Vector<Item> items;
    dstd::Vector<CommitRefItem> refs;
    for (const RecordItem& item : pending_items_) {
        const auto* page = std::get_if<PageRefItem>(&item);
        if (page == nullptr) continue;
        std::size_t at = 0;
        while (at < refs.size() && refs[at].sequence != page->sequence) ++at;
        if (at == refs.size()) {
            CommitRefItem fresh;
            fresh.sequence = page->sequence;
            fresh.record_offset = tail_offset_;
            fresh.record_crc = tail_crc_;
            fresh.synced = 1;
            refs.push_back(std::move(fresh));
        }
        if (page->synced == 0) refs[at].synced = 0;
        for (const PageSection& section : page->sections) {
            items.push_back({page->sequence, section.copy_id, &section});
        }
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.sequence != b.sequence) return a.sequence < b.sequence;
        return a.copy_id < b.copy_id;
    });
    for (std::size_t at = 0; at < items.size();) {
        const PageSection* low = items[at].section;
        const PageSection* high = low;
        std::size_t end = at + 1;
        while (end < items.size() &&
               items[end].sequence == items[at].sequence &&
               items[end].copy_id == items[at].copy_id) {
            const PageSection& section = *items[end].section;
            if (section.first_key < low->first_key) low = &section;
            if (high->last_key < section.last_key) high = &section;
            ++end;
        }
        for (CommitRefItem& ref : refs) {
            if (ref.sequence != items[at].sequence) continue;
            ref.copies.push_back(
                {items[at].copy_id, low->first_key, high->last_key});
        }
        at = end;
    }
    for (CommitRefItem& ref : refs) all_items_.push_back(std::move(ref));
}

dstd::Expected<void, dstd::Error> DataFileWriter::write_record(
    RecordKind kind, const RecordItems& items) {
    if (latched()) return dstd::Unexpected<dstd::Error>(latched_result());
    const std::uint64_t sequence = last_sequence_ + 1;
    if (sequence == 0) return invalid_error();
    const bool checkpoint = sequence % CHECKPOINT_INTERVAL == 0;
    const RecordKind actual_kind = kind == RecordKind::Commit && checkpoint
                                       ? RecordKind::Checkpoint
                                       : kind;

    // A record names its own offset, so its size is measured at a fixed
    // placeholder offset and the range is reserved before the real bytes are
    // built. The item encoding, and so the size, does not depend on it.
    auto probe = encode_record(actual_kind, sequence, tail_offset_,
                               FILE_HEADER_BYTES, items);
    if (!probe) return dstd::Unexpected<dstd::Error>(probe.error());
    const std::uint64_t offset = reserve_range(probe->size(), FILE_HEADER_BYTES,
                                               fault_kind_of(actual_kind));
    dstd::Vector<std::uint8_t> encoded;
    if (offset == FILE_HEADER_BYTES) {
        encoded = std::move(*probe);
    } else {
        auto rebuilt =
            encode_record(actual_kind, sequence, tail_offset_, offset, items);
        if (!rebuilt) return dstd::Unexpected<dstd::Error>(rebuilt.error());
        encoded = std::move(*rebuilt);
    }

    auto written = pwrite_all(fd_, encoded.data(), encoded.size(), offset);
    if (!written) {
        latch(written.error());
        return written;
    }
    io_fault::on_write(fd_, fault_kind_of(actual_kind), offset, encoded.data(),
                       encoded.size());

    const std::uint32_t body_length = read_u32_le(encoded.data() + 12);
    tail_offset_ = offset;
    tail_kind_ = static_cast<std::uint32_t>(actual_kind);
    tail_crc_ = read_u32_le(encoded.data() + RECORD_HEADER_BYTES + body_length);
    last_sequence_ = sequence;
    return {};
}

dstd::Expected<void, dstd::Error> DataFileWriter::commit() {
    if (latched()) return dstd::Unexpected<dstd::Error>(latched_result());
    if (closed_.load(std::memory_order_acquire)) return invalid_error();
    const bool checkpoint = (last_sequence_ + 1) % CHECKPOINT_INTERVAL == 0;
    auto record = checkpoint ? write_record(RecordKind::Commit,
                                            items_with_pending_pages())
                             : write_record(RecordKind::Commit, pending_items_);
    if (!record) return record;
    for (const RecordItem& item : pending_items_) {
        if (const auto* page = std::get_if<PageRefItem>(&item)) {
            committed_pages_.push_back(*page);
        }
    }
    add_commit_refs();
    auto header = write_header();
    if (!header) return header;
    pending_items_.clear();
    return {};
}

dstd::Expected<void, dstd::Error> DataFileWriter::close() {
    if (latched()) return dstd::Unexpected<dstd::Error>(latched_result());
    if (closed_.load(std::memory_order_acquire)) return {};
    const RecordItems items = items_with_pending_pages();
    // Every block in one write, at a range whose offset the blocks name: the
    // range is taken only if no write took the end of the file meanwhile.
    dstd::Vector<std::uint8_t> blocks;
    dstd::Expected<FileTail, dstd::Error> tail = invalid_error();
    std::uint64_t base = 0;
    for (;;) {
        base = next_offset_.load(std::memory_order_acquire);
        blocks.clear();
        tail = build_blocks(items, base, blocks);
        if (!tail) {
            latch(tail.error());
            return dstd::Unexpected<dstd::Error>(tail.error());
        }
        std::uint64_t expected = base;
        if (next_offset_.compare_exchange_strong(expected, base + blocks.size(),
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            break;
        }
    }
    if (!blocks.empty()) {
        auto written = pwrite_all(fd_, blocks.data(), blocks.size(), base);
        if (!written) {
            latch(written.error());
            return written;
        }
        io_fault::on_write(fd_, io_fault::WriteKind::Footer, base,
                           blocks.data(), blocks.size());
    }
    // One sync covers the pages and every block above them, before the footer
    // that names them. Every page before the footer, so a reader trusts the
    // pages of a closed file one by one.
    auto pages_synced = sync_data();
    if (!pages_synced) {
        latch(pages_synced.error());
        return pages_synced;
    }
    // The footer holds the namespaces and the tail: the items block holds
    // every other item again, and only a full open reads it.
    RecordItems footer;
    for (const RecordItem& item : items) {
        if (std::holds_alternative<NamespaceItem>(item)) footer.push_back(item);
    }
    footer.push_back(std::move(*tail));
    auto record = write_record(RecordKind::Footer, footer);
    if (!record) return record;

    const std::uint64_t trailer_offset =
        reserve_range(FILE_TRAILER_BYTES, 1, io_fault::WriteKind::Trailer);
    const std::uint32_t footer_length =
        static_cast<std::uint32_t>(trailer_offset - tail_offset_);
    dstd::Vector<std::uint8_t> trailer(FILE_TRAILER_BYTES, 0);
    std::memcpy(trailer.data(), TRAILER_MAGIC.data(), TRAILER_MAGIC.size());
    write_u64_le(trailer.data() + 8, tail_offset_);
    write_u32_le(trailer.data() + 16, footer_length);
    const std::uint32_t trailer_crc =
        compress::gzip_crc32(0, trailer.data(), 20);
    write_u32_le(trailer.data() + 20, trailer_crc);
    auto written =
        pwrite_all(fd_, trailer.data(), trailer.size(), trailer_offset);
    if (!written) {
        latch(written.error());
        return written;
    }
    io_fault::on_write(fd_, io_fault::WriteKind::Trailer, trailer_offset,
                       trailer.data(), trailer.size());

    // No sync after the footer: a footer lost to a crash leaves a file that
    // reads as not closed, whose commits are checked whole.
    auto header = write_header();
    ::close(fd_);
    fd_ = -1;
    closed_.store(true, std::memory_order_release);
    if (!header) return header;
    return {};
}

dstd::Expected<void, dstd::Error> DataFileWriter::sync_data() {
    int sync_result = -1;
    do {
#if defined(__APPLE__)
        sync_result = ::fsync(fd_);
#else
        sync_result = ::fdatasync(fd_);
#endif
    } while (sync_result != 0 && errno == EINTR);
    io_fault::on_sync(fd_);
    if (sync_result != 0) {
        return dstd::Unexpected<dstd::Error>(dstd::Error(dstd::Condition::Io));
    }
    return {};
}

dstd::Expected<DataFileReader, dstd::Error> DataFileReader::open(
    dstd::StringView path, FooterItems footer) {
    const std::string path_std = path_string(path);
    const int fd = ::open(path_std.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return io_error();

    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return io_error();
    }
    if (st.st_size < 0) {
        ::close(fd);
        return parse_error();
    }
    const std::uint64_t file_size = static_cast<std::uint64_t>(st.st_size);
    if (file_size < FILE_HEADER_BYTES) {
        ::close(fd);
        return parse_error();
    }

    DataFileReader reader;
    reader.fd_ = fd;
    reader.file_size_ = file_size;

    // One bounded read of the file end: the trailer, the footer and the whole
    // directory top table. Nothing else is read until a query needs it.
    if (footer == FooterItems::Directory) {
        auto light = reader.open_light(file_size);
        if (!light) return dstd::Unexpected<dstd::Error>(light.error());
        if (*light) return reader;
    }

    const auto finish =
        [&footer, &reader]() -> dstd::Expected<DataFileReader, dstd::Error> {
        if (footer == FooterItems::All) {
            auto applied = reader.apply_items_block();
            if (!applied) {
                return dstd::Unexpected<dstd::Error>(applied.error());
            }
        }
        return std::move(reader);
    };

    auto header_result = read_header(fd);
    if (!header_result)
        return dstd::Unexpected<dstd::Error>(header_result.error());
    const HeaderInfo header = *header_result;
    if (header.magic_ok && header.format_version != FORMAT_VERSION) {
        return unsupported_error();
    }
    if (header.header_crc_ok) fill_header_fields(reader.index_, header);

    if (header.header_crc_ok && header.tail_kind == 0) {
        auto found = scan_records(fd, file_size, reader.records_read_);
        if (!found) return dstd::Unexpected<dstd::Error>(found.error());
        if (found->empty()) return finish();
        auto recovered =
            recover_from_scan(fd, file_size, *found, reader.records_read_);
        if (!recovered) return dstd::Unexpected<dstd::Error>(recovered.error());
        apply_chain(reader.index_, *recovered);
        return finish();
    }

    dstd::Vector<Record> chain;
    bool have_chain = false;
    if (header.header_crc_ok && header.tail_kind != 0 &&
        tail_layout_is_valid(file_size, header.tail_offset, header.tail_kind)) {
        auto tail = read_record_at(fd, file_size, header.tail_offset,
                                   reader.records_read_);
        if (!tail) {
            if (tail.error().is(dstd::Condition::Io)) {
                return dstd::Unexpected<dstd::Error>(tail.error());
            }
        } else if (static_cast<std::uint32_t>(tail->kind) == header.tail_kind &&
                   tail->crc == header.tail_crc) {
            auto walked = walk_back(fd, file_size, std::move(*tail),
                                    reader.records_read_);
            if (!walked) {
                if (walked.error().is(dstd::Condition::Io)) {
                    return dstd::Unexpected<dstd::Error>(walked.error());
                }
            } else {
                chain = std::move(*walked);
                have_chain = true;
            }
        }
    }

    if (!have_chain) {
        auto found = scan_records(fd, file_size, reader.records_read_);
        if (!found) return dstd::Unexpected<dstd::Error>(found.error());
        if (found->empty()) return parse_error();
        auto recovered =
            recover_from_scan(fd, file_size, *found, reader.records_read_);
        if (!recovered) return dstd::Unexpected<dstd::Error>(recovered.error());
        chain = std::move(*recovered);
    }

    apply_chain(reader.index_, chain);
    return finish();
}

dstd::Expected<bool, dstd::Error> DataFileReader::open_light(
    std::uint64_t file_size) {
    const std::uint64_t want =
        std::min<std::uint64_t>(file_size, TAIL_READ_BYTES);
    if (want < FILE_TRAILER_BYTES) return false;
    const std::uint64_t start = file_size - want;
    ++records_read_;
    auto buffer = pread_all(fd_, start, static_cast<std::size_t>(want));
    if (!buffer) return false;
    const ByteView bytes = bytes_of(*buffer);

    const std::size_t trailer_at =
        static_cast<std::size_t>(want) - FILE_TRAILER_BYTES;
    const auto* trailer =
        reinterpret_cast<const std::uint8_t*>(bytes.data() + trailer_at);
    if (std::memcmp(trailer, TRAILER_MAGIC.data(), TRAILER_MAGIC.size()) != 0) {
        return false;
    }
    if (read_u32_le(trailer + 20) !=
        compress::gzip_crc32(0, trailer, FILE_TRAILER_BYTES - 12)) {
        return false;
    }
    const std::uint64_t tail_offset = read_u64_le(trailer + 8);
    const std::uint32_t footer_length = read_u32_le(trailer + 16);
    if (tail_offset < FILE_HEADER_BYTES ||
        tail_offset % FILE_HEADER_BYTES != 0 ||
        tail_offset > file_size - FILE_TRAILER_BYTES ||
        footer_length < RECORD_HEADER_BYTES + 4) {
        return false;
    }

    dstd::Expected<Record, dstd::Error> decoded = parse_error();
    if (tail_offset >= start) {
        const std::size_t at = static_cast<std::size_t>(tail_offset - start);
        const std::size_t length = std::min<std::size_t>(
            footer_length, static_cast<std::size_t>(want) - at);
        decoded = decode_record_at(bytes.subspan(at, length), tail_offset);
    } else {
        decoded = read_record_at(fd_, file_size, tail_offset, records_read_);
    }
    if (!decoded || decoded->kind != RecordKind::Footer) return false;

    const FileTail* tail = nullptr;
    for (const RecordItem& item : decoded->items) {
        if (const auto* namespace_item = std::get_if<NamespaceItem>(&item)) {
            index_.namespaces.push_back(*namespace_item);
        } else if (const auto* file_tail = std::get_if<FileTail>(&item)) {
            tail = file_tail;
        }
    }
    if (tail == nullptr) return false;
    if (tail->format_version != FORMAT_VERSION) return unsupported_error();
    // Pages start at multiples of the page size, one page to each.
    if (tail->page_count > file_size / tail->page_size + 1) return false;

    index_.page_size = tail->page_size;
    index_.writer_random = tail->writer_random;
    index_.pid = tail->pid;
    index_.host = tail->host;
    index_.label = tail->label;
    index_.creation_time = tail->creation_time;
    index_.last_sequence = decoded->sequence;
    index_.closed = true;
    index_.tail = *tail;
    return true;
}

dstd::Expected<void, dstd::Error> DataFileReader::apply_items_block() {
    if (!index_.tail.has_value()) return {};
    auto bytes = read_block(index_.tail->items, nullptr);
    if (!bytes) return dstd::Unexpected<dstd::Error>(bytes.error());
    auto items = decode_items(bytes_of(*bytes));
    if (!items) return dstd::Unexpected<dstd::Error>(items.error());
    Record record;
    record.kind = RecordKind::Footer;
    record.sequence = index_.last_sequence;
    record.items = std::move(*items);
    apply_record(index_, record);
    return {};
}

DataFileReader::DataFileReader(DataFileReader&& other) noexcept
    : fd_(other.fd_),
      file_size_(other.file_size_),
      records_read_(other.records_read_),
      index_(std::move(other.index_)) {
    other.fd_ = -1;
}

DataFileReader& DataFileReader::operator=(DataFileReader&& other) noexcept {
    if (this == &other) return *this;
    if (fd_ >= 0) ::close(fd_);
    fd_ = other.fd_;
    file_size_ = other.file_size_;
    records_read_ = other.records_read_;
    index_ = std::move(other.index_);
    other.fd_ = -1;
    return *this;
}

DataFileReader::~DataFileReader() {
    if (fd_ >= 0) ::close(fd_);
}

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error>
DataFileReader::read_page(const PageRef& ref) const {
    if (ref.length == 0 || ref.offset > file_size_ ||
        ref.length > file_size_ - ref.offset) {
        return parse_error();
    }
    auto bytes = pread_all(fd_, ref.offset, ref.length);
    if (!bytes) return dstd::Unexpected<dstd::Error>(bytes.error());
    const std::uint32_t crc =
        compress::gzip_crc32(0, bytes->data(), bytes->size());
    if (crc != ref.crc) return parse_error();
    return bytes;
}

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error>
DataFileReader::read_block(const BlockHandle& handle, BlockReads* reads) const {
    if (handle.length == 0 && handle.offset == 0 && handle.crc == 0) {
        return dstd::Vector<std::uint8_t>{};
    }
    if (handle.length == 0 || handle.offset > file_size_ ||
        handle.length > file_size_ - handle.offset) {
        return parse_error();
    }
    if (reads != nullptr) {
        ++reads->calls;
        reads->bytes += handle.length;
    }
    auto bytes = pread_all(fd_, handle.offset, handle.length);
    if (!bytes) return dstd::Unexpected<dstd::Error>(bytes.error());
    if (compress::gzip_crc32(0, bytes->data(), bytes->size()) != handle.crc) {
        return parse_error();
    }
    return bytes;
}

dstd::Expected<void, dstd::Error> DataFileReader::verify_block(
    const PageRef& ref, BlockReads* reads) const {
    if (ref.length == 0 || ref.offset > file_size_ ||
        ref.length > file_size_ - ref.offset) {
        return parse_error();
    }
    dstd::Vector<std::uint8_t> chunk(
        std::min<std::size_t>(VERIFY_CHUNK_BYTES, ref.length));
    std::uint32_t crc = 0;
    std::uint64_t done = 0;
    while (done < ref.length) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk.size(), ref.length - done));
        const ssize_t got = ::pread(fd_, chunk.data(), want,
                                    static_cast<off_t>(ref.offset + done));
        if (got < 0) {
            if (errno == EINTR) continue;
            return io_error();
        }
        if (got == 0) return parse_error();
        if (reads != nullptr) {
            ++reads->calls;
            reads->bytes += static_cast<std::uint64_t>(got);
        }
        crc = compress::gzip_crc32(crc, chunk.data(),
                                   static_cast<std::size_t>(got));
        done += static_cast<std::uint64_t>(got);
    }
    if (crc != ref.crc) return parse_error();
    return {};
}

dstd::Expected<BlockReads, dstd::Error> DataFileReader::read_pages(
    dstd::Span<const PageRef> refs,
    dstd::Span<const PageTarget> targets) const {
    const auto in_file = [this](const PageRef& ref) {
        return ref.length != 0 && ref.offset <= file_size_ &&
               ref.length <= file_size_ - ref.offset;
    };
    const auto adjacent = [](const PageRef& previous, const PageRef& next) {
        return next.offset == previous.offset + previous.length;
    };

    BlockReads counts;
    std::vector<struct iovec> vecs;
    std::size_t at = 0;
    while (at < refs.size()) {
        if (!in_file(refs[at])) {
            *targets[at].bad = 1;
            ++at;
            continue;
        }
        // One group is a head ref and every ref after it that is readable and
        // starts where that ref leaves off, up to what one preadv takes.
        std::size_t end = at + 1;
        while (end < refs.size() && end - at < IOV_MAX && in_file(refs[end]) &&
               adjacent(refs[end - 1U], refs[end])) {
            ++end;
        }

        vecs.resize(end - at);
        std::uint64_t group_bytes = 0;
        for (std::size_t i = at; i < end; ++i) {
            const PageRef& ref = refs[i];
            *targets[i].bytes =
                dstd::Vector<std::uint8_t>(ref.length, std::uint8_t{0});
            vecs[i - at].iov_base = targets[i].bytes->data();
            vecs[i - at].iov_len = targets[i].bytes->size();
            group_bytes += ref.length;
        }

        std::size_t served = 0;
        std::uint64_t done = 0;
        while (done < group_bytes) {
            struct iovec* cur = &vecs[served];
            const ssize_t got =
                ::preadv(fd_, cur, static_cast<int>(vecs.size() - served),
                         static_cast<off_t>(refs[at].offset + done));
            if (got < 0) {
                if (errno == EINTR) continue;
                return io_error();
            }
            if (got == 0) break;
            ++counts.calls;
            counts.bytes += static_cast<std::uint64_t>(got);
            done += static_cast<std::uint64_t>(got);
            std::size_t left = static_cast<std::size_t>(got);
            while (left > 0) {
                struct iovec* page = &vecs[served];
                if (left < page->iov_len) {
                    page->iov_base = static_cast<char*>(page->iov_base) + left;
                    page->iov_len -= left;
                    left = 0;
                } else {
                    left -= page->iov_len;
                    ++served;
                }
            }
        }

        for (std::size_t i = at; i < end; ++i) {
            const PageRef& ref = refs[i];
            dstd::Vector<std::uint8_t>& bytes = *targets[i].bytes;
            const bool complete = i < at + served;
            if (complete && compress::gzip_crc32(0, bytes.data(),
                                                 bytes.size()) == ref.crc) {
                continue;
            }
            // A page past the end of the file, or one whose crc fails, keeps no
            // bytes: the codes of a copy reach the caller through its state.
            *targets[i].bad = 1;
            dstd::Vector<std::uint8_t>().swap(bytes);
        }
        at = end;
    }
    return counts;
}

dstd::Expected<dstd::Vector<PageRefItem>, dstd::Error>
DataFileReader::read_commit(const CommitRefItem& ref, BlockReads* reads) const {
    auto length = read_record_length(fd_, file_size_, ref.record_offset);
    if (!length) return dstd::Unexpected<dstd::Error>(length.error());
    auto bytes = pread_all(fd_, ref.record_offset, *length);
    if (!bytes) return dstd::Unexpected<dstd::Error>(bytes.error());
    if (reads != nullptr) {
        reads->calls += 2;
        reads->bytes += RECORD_HEADER_BYTES + *length;
    }
    auto record = decode_record_at(bytes_of(*bytes), ref.record_offset);
    if (!record) return dstd::Unexpected<dstd::Error>(record.error());
    if (record->crc != ref.record_crc) return parse_error();
    dstd::Vector<PageRefItem> refs;
    for (RecordItem& item : record->items) {
        auto* page = std::get_if<PageRefItem>(&item);
        if (page != nullptr && page->sequence == ref.sequence) {
            refs.push_back(std::move(*page));
        }
    }
    return refs;
}

dstd::Expected<dstd::Vector<IndexedPageRef>, dstd::Error>
DataFileReader::all_page_refs() const {
    dstd::Vector<IndexedPageRef> refs = index_.page_refs;
    for (const CommitRefItem& commit : index_.commit_refs) {
        auto listed = read_commit(commit, nullptr);
        if (!listed) return dstd::Unexpected<dstd::Error>(listed.error());
        for (PageRefItem& page : *listed) {
            refs.push_back({commit.sequence, std::move(page)});
        }
    }
    std::stable_sort(refs.begin(), refs.end(),
                     [](const IndexedPageRef& a, const IndexedPageRef& b) {
                         return a.sequence < b.sequence;
                     });
    return refs;
}

}  // namespace dftracer::utils::index::store::engine
