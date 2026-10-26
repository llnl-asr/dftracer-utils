#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/index/store/engine/data_file.h>
#include <dftracer/utils/index/store/engine/io_fault.h>
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
constexpr std::uint64_t CHECKPOINT_INTERVAL = 64;
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
        }
    }
    index.last_sequence = record.sequence;
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
    all_items_.push_back(item);
    pending_items_.push_back(std::move(item));
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
    auto record = write_record(RecordKind::Commit,
                               checkpoint ? all_items_ : pending_items_);
    if (!record) return record;
    auto header = write_header();
    if (!header) return header;
    pending_items_.clear();
    return {};
}

dstd::Expected<void, dstd::Error> DataFileWriter::close() {
    if (latched()) return dstd::Unexpected<dstd::Error>(latched_result());
    if (closed_.load(std::memory_order_acquire)) return {};
    auto record = write_record(RecordKind::Footer, all_items_);
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

    auto header = write_header();
    if (!header) return header;

    int sync_result = -1;
    do {
#if defined(__APPLE__)
        sync_result = ::fsync(fd_);
#else
        sync_result = ::fdatasync(fd_);
#endif
    } while (sync_result != 0 && errno == EINTR);
    io_fault::on_sync(fd_);
    const bool synced = sync_result == 0;
    ::close(fd_);
    fd_ = -1;
    closed_.store(true, std::memory_order_release);
    if (!synced) {
        const dstd::Error error(dstd::Condition::Io);
        latch(error);
        return dstd::Unexpected<dstd::Error>(error);
    }
    return {};
}

dstd::Expected<DataFileReader, dstd::Error> DataFileReader::open(
    dstd::StringView path) {
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
        if (found->empty()) return reader;
        auto recovered =
            recover_from_scan(fd, file_size, *found, reader.records_read_);
        if (!recovered) return dstd::Unexpected<dstd::Error>(recovered.error());
        apply_chain(reader.index_, *recovered);
        return reader;
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
    return reader;
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

}  // namespace dftracer::utils::index::store::engine
