#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/timer.h>
#include <dftracer/utils/dstd/bits.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/store/crc.h>
#include <dftracer/utils/store/data_file.h>
#include <dftracer/utils/store/extent_codec.h>
#include <dftracer/utils/store/io_fault.h>
#include <dftracer/utils/store/scope_directory.h>
#include <dftracer/utils/store/scope_filter.h>
#include <dftracer/utils/store/varint.h>
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
#include <vector>

namespace dftracer::utils::store::engine {

namespace {

namespace dstd = dftracer::utils::dstd;

constexpr std::string_view FILE_MAGIC = "DFTUSTR0";
constexpr std::string_view TRAILER_MAGIC = "DFTUEND0";
constexpr std::string_view RECORD_MAGIC = "DFTUREC0";
constexpr std::uint32_t FORMAT_VERSION = 0;
constexpr std::uint64_t RECORD_HEADER_BYTES = 40;

struct HeaderInfo {
    bool magic_ok = false;
    bool header_crc_ok = false;
    std::uint32_t format_version = 0;
    std::uint32_t extent_size = 0;
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

dstd::ByteView bytes_of(const dstd::Vector<std::uint8_t>& bytes) {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

dstd::ByteView bytes_of(const dstd::RawBuffer<std::uint8_t>& bytes) {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

std::uint64_t random_writer_id() {
    const auto now = steady_now_ns();
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
    info.format_version = dstd::read_u32_le(data + 8);
    if (info.format_version != FORMAT_VERSION) return info;

    const std::uint32_t stored_crc = dstd::read_u32_le(data + 12);
    dstd::Vector<std::uint8_t> crc_bytes = *bytes;
    dstd::write_u32_le(crc_bytes.data() + 12, 0);
    const std::uint32_t crc = crc32(0, crc_bytes.data(), crc_bytes.size());
    info.header_crc_ok = stored_crc == crc;

    const std::uint32_t extent_size = dstd::read_u32_le(data + 16);
    const std::uint64_t creation_time = dstd::read_u64_le(data + 24);
    const std::uint64_t writer_random = dstd::read_u64_le(data + 32);
    const std::uint32_t pid = dstd::read_u32_le(data + 40);
    const std::uint16_t host_length = dstd::read_u16_le(data + 44);
    const std::uint16_t label_length = dstd::read_u16_le(data + 46);

    const std::uint64_t strings_end =
        static_cast<std::uint64_t>(48) + host_length + label_length + 16;
    if (host_length > 255 || label_length > 255 ||
        strings_end > FILE_HEADER_BYTES) {
        return info;
    }

    info.extent_size = extent_size;
    info.writer_random = writer_random;
    info.pid = pid;
    info.host.assign(reinterpret_cast<const char*>(data + 48), host_length);
    info.label.assign(reinterpret_cast<const char*>(data + 48 + host_length),
                      label_length);
    info.creation_time = creation_time;
    const std::size_t tail_at = 48 + host_length + label_length;
    info.tail_offset = dstd::read_u64_le(data + tail_at);
    info.tail_kind = dstd::read_u32_le(data + tail_at + 8);
    info.tail_crc = dstd::read_u32_le(data + tail_at + 12);
    return info;
}

void fill_header_fields(FileIndex& index, const HeaderInfo& info) {
    index.extent_size = info.extent_size;
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
    if (std::memcmp(data, RECORD_MAGIC.data(), RECORD_MAGIC.size()) != 0) {
        return parse_error();
    }
    const std::uint32_t kind = dstd::read_u32_le(data + 8);
    if (kind < 1 || kind > 3) return parse_error();
    const std::uint32_t body_length = dstd::read_u32_le(data + 12);
    const std::uint64_t crc_end = RECORD_HEADER_BYTES + body_length + 4;
    const std::uint64_t padded_size =
        dstd::bits::align_up(crc_end, FILE_HEADER_BYTES);
    if (padded_size < crc_end || padded_size > file_size - offset) {
        return parse_error();
    }
    return padded_size;
}

dstd::Expected<Record, dstd::Error> decode_record_at(dstd::ByteView bytes,
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
        index.copy_at.clear();
        index.drops.clear();
        index.erases.clear();
        index.extent_refs.clear();
        index.commit_refs.clear();
    }
    for (const RecordItem& item : record.items) {
        if (const auto* namespace_item = std::get_if<NamespaceItem>(&item)) {
            index.namespaces.push_back(*namespace_item);
        } else if (const auto* scope_item = std::get_if<ScopeItem>(&item)) {
            index.scopes.push_back(*scope_item);
        } else if (const auto* copy_item = std::get_if<CopyItem>(&item)) {
            // A later item of the same copy publishes it.
            const auto known = index.copy_at.try_emplace(copy_item->local_id,
                                                         index.copies.size());
            if (known.second) {
                index.copies.push_back(*copy_item);
            } else {
                index.copies[known.first->second] = *copy_item;
            }
        } else if (const auto* drop_item = std::get_if<DropItem>(&item)) {
            index.drops.push_back(*drop_item);
        } else if (const auto* erase_item = std::get_if<EraseItem>(&item)) {
            index.erases.push_back(*erase_item);
        } else if (const auto* extent_ref_item =
                       std::get_if<ExtentRefItem>(&item)) {
            index.extent_refs.push_back(
                {extent_ref_item->sequence, *extent_ref_item});
        } else if (const auto* commit_ref_item =
                       std::get_if<CommitRefItem>(&item)) {
            index.commit_refs.push_back(*commit_ref_item);
        } else if (const auto* file_tail = std::get_if<FileTailPtr>(&item)) {
            index.tail = **file_tail;
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
    out.append(DATA_FILE_SUFFIX);
    return out;
}

dstd::Expected<std::size_t, dstd::Error> move_data_files(dstd::StringView from,
                                                         dstd::StringView to) {
    const std::string from_dir = path_string(from);
    const std::string to_dir = path_string(to);
    auto io_error = [](const std::string& what, const std::error_code& ec) {
        return dstd::Unexpected<dstd::Error>(
            dstd::Error(dstd::Condition::Io, what + ": " + ec.message()));
    };
    std::error_code ec;
    fs::create_directories(to_dir, ec);
    if (ec) return io_error("store: cannot create the directory " + to_dir, ec);

    std::vector<std::string> names;
    fs::directory_iterator it(from_dir, ec);
    if (ec) return io_error("store: cannot list the directory " + from_dir, ec);
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec) || file_ec) continue;
        std::string name = it->path().filename().string();
        if (name.size() <= DATA_FILE_SUFFIX.size()) continue;
        if (name.compare(name.size() - DATA_FILE_SUFFIX.size(),
                         DATA_FILE_SUFFIX.size(), DATA_FILE_SUFFIX) != 0) {
            continue;
        }
        names.push_back(std::move(name));
    }
    if (ec) return io_error("store: cannot list the directory " + from_dir, ec);
    std::sort(names.begin(), names.end());

    for (const std::string& name : names) {
        const fs::path src = fs::path(from_dir) / name;
        fs::rename(src, fs::path(to_dir) / name, ec);
        if (ec) return io_error("store: cannot move " + src.string(), ec);
    }
    return names.size();
}

namespace {

constexpr std::string_view SUMMARY_MAGIC = "DFSM";
constexpr std::size_t SUMMARY_ENTRY_HEADER_BYTES = 12;

std::pair<std::string, std::string> split_path(dstd::StringView path) {
    const std::string whole(path.data(), path.size());
    const std::size_t slash = whole.find_last_of('/');
    if (slash == std::string::npos) return {".", whole};
    return {slash == 0 ? std::string("/") : whole.substr(0, slash),
            whole.substr(slash + 1)};
}

std::string summary_path(const std::string& dir) {
    std::string path = dir;
    if (!path.empty() && path.back() != '/') path.push_back('/');
    path.append(SUMMARY_FILE_NAME.data(), SUMMARY_FILE_NAME.size());
    return path;
}

void append_summary(dstd::StringView data_path, std::uint64_t needed_bytes,
                    std::uint64_t sequence, const RecordItems& footer) {
    const auto [dir, name] = split_path(data_path);
    dstd::Vector<std::uint8_t> payload;
    dstd::append_u32_le(payload, static_cast<std::uint32_t>(name.size()));
    payload.insert(payload.end(), name.begin(), name.end());
    dstd::append_u64_le(payload, needed_bytes);
    dstd::append_u64_le(payload, sequence);
    const dstd::Vector<std::uint8_t> raw_items = encode_items(footer);
    const dstd::Vector<std::uint8_t> items =
        encode_meta(bytes_of(raw_items), META_ZSTD_LEVEL);
    payload.insert(payload.end(), items.begin(), items.end());
    dstd::Vector<std::uint8_t> entry;
    entry.insert(entry.end(), SUMMARY_MAGIC.begin(), SUMMARY_MAGIC.end());
    dstd::append_u32_le(entry, static_cast<std::uint32_t>(payload.size()));
    dstd::append_u32_le(entry, crc32(0, payload.data(), payload.size()));
    entry.insert(entry.end(), payload.begin(), payload.end());
    const int fd = ::open(summary_path(dir).c_str(),
                          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    // One write, so writers that close at once append whole entries.
    (void)::write(fd, entry.data(), entry.size());
    ::close(fd);
}

}  // namespace

dstd::Expected<dstd::Vector<SummaryEntry>, dstd::Error> read_summary(
    dstd::StringView dir) {
    dstd::Vector<SummaryEntry> entries;
    const std::string path = summary_path(std::string(dir.data(), dir.size()));
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return entries;
        return io_error();
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < 0) {
        ::close(fd);
        return io_error();
    }
    auto bytes = pread_all(fd, 0, static_cast<std::size_t>(st.st_size));
    ::close(fd);
    if (!bytes) {
        if (bytes.error().is(dstd::Condition::Io)) {
            return dstd::Unexpected<dstd::Error>(bytes.error());
        }
        return entries;
    }
    const std::uint8_t* data = bytes->data();
    const std::size_t size = bytes->size();
    std::size_t at = 0;
    while (size - at >= SUMMARY_ENTRY_HEADER_BYTES) {
        if (std::memcmp(data + at, SUMMARY_MAGIC.data(),
                        SUMMARY_MAGIC.size()) != 0) {
            break;
        }
        const std::uint32_t length = dstd::read_u32_le(data + at + 4);
        const std::uint32_t crc = dstd::read_u32_le(data + at + 8);
        const std::size_t body = at + SUMMARY_ENTRY_HEADER_BYTES;
        if (length > size - body || crc32(0, data + body, length) != crc ||
            length < 20) {
            break;
        }
        const std::uint32_t name_length = dstd::read_u32_le(data + body);
        if (name_length > length - 20) break;
        SummaryEntry entry;
        entry.name.assign(reinterpret_cast<const char*>(data + body + 4),
                          name_length);
        entry.needed_bytes = dstd::read_u64_le(data + body + 4 + name_length);
        entry.sequence = dstd::read_u64_le(data + body + 12 + name_length);
        const std::size_t items_at = body + 20 + name_length;
        auto raw_items = decode_meta(
            dstd::ByteView{reinterpret_cast<const std::byte*>(data + items_at),
                           body + length - items_at});
        if (!raw_items) break;
        auto items = decode_items(bytes_of(*raw_items));
        if (!items) break;
        entry.footer = std::move(*items);
        entries.push_back(std::move(entry));
        at = body + length;
    }
    return entries;
}

dstd::Expected<DataFileWriter, dstd::Error> DataFileWriter::create(
    dstd::StringView path, std::uint32_t extent_size, dstd::StringView host,
    dstd::StringView label) {
    if (extent_size < FILE_HEADER_BYTES ||
        extent_size % FILE_HEADER_BYTES != 0) {
        return invalid_error();
    }
    if (host.size() > 255 || label.size() > 255) return invalid_error();

    const std::string path_std = path_string(path);
    const int fd =
        ::open(path_std.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return io_error();

    DataFileWriter writer;
    writer.fd_ = fd;
    writer.path_.assign(path.data(), path.size());
    writer.extent_size_ = extent_size;
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
      path_(std::move(other.path_)),
      extent_size_(other.extent_size_),
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
      copy_item_at_(std::move(other.copy_item_at_)),
      committed_extents_(std::move(other.committed_extents_)),
      closed_(other.closed_.load(std::memory_order_relaxed)),
      latched_(other.latched_.load(std::memory_order_relaxed)),
      latched_error_(std::move(other.latched_error_)) {
    other.fd_ = -1;
}

DataFileWriter& DataFileWriter::operator=(DataFileWriter&& other) noexcept {
    if (this == &other) return *this;
    if (fd_ >= 0) ::close(fd_);
    fd_ = other.fd_;
    path_ = std::move(other.path_);
    extent_size_ = other.extent_size_;
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
    copy_item_at_ = std::move(other.copy_item_at_);
    committed_extents_ = std::move(other.committed_extents_);
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
    dstd::write_u32_le(header.data() + 8, FORMAT_VERSION);
    dstd::write_u32_le(header.data() + 16, extent_size_);
    dstd::write_u64_le(header.data() + 24, creation_time_);
    dstd::write_u64_le(header.data() + 32, writer_random_);
    dstd::write_u32_le(header.data() + 40, pid_);
    dstd::write_u16_le(header.data() + 44,
                       static_cast<std::uint16_t>(host_.size()));
    dstd::write_u16_le(header.data() + 46,
                       static_cast<std::uint16_t>(label_.size()));
    std::memcpy(header.data() + 48, host_.data(), host_.size());
    std::memcpy(header.data() + 48 + host_.size(), label_.data(),
                label_.size());

    const std::size_t tail_at = 48 + host_.size() + label_.size();
    dstd::write_u64_le(header.data() + tail_at, tail_offset_);
    dstd::write_u32_le(header.data() + tail_at + 8, tail_kind_);
    dstd::write_u32_le(header.data() + tail_at + 12, tail_crc_);

    const std::uint32_t crc = crc32(0, header.data(), header.size());
    dstd::write_u32_le(header.data() + 12, crc);
    auto written = pwrite_all(fd_, header.data(), header.size(), 0);
    if (written) {
        io_fault::on_write(fd_, io_fault::WriteKind::Header, 0, header.data(),
                           header.size());
    } else {
        latch(written.error());
    }
    return written;
}

dstd::Expected<ExtentRef, dstd::Error> DataFileWriter::append_extent(
    dstd::ByteView bytes) {
    if (latched()) return dstd::Unexpected<dstd::Error>(latched_result());
    if (closed_.load(std::memory_order_acquire)) return invalid_error();
    if (bytes.empty() || bytes.size() > extent_size_ - EXTENT_HEADER_BYTES) {
        return invalid_error();
    }
    const SegmentSpan span{0, static_cast<std::uint32_t>(bytes.size())};
    const dstd::Vector<std::uint32_t> starts =
        page_starts(dstd::Span<const SegmentSpan>(&span, 1), span.length);
    auto stored = encode_extent(bytes, starts, EXTENT_ALIGN_BYTES);
    if (!stored) return dstd::Unexpected<dstd::Error>(stored.error());
    const std::uint64_t offset = reserve_range(
        stored->size(), EXTENT_ALIGN_BYTES, io_fault::WriteKind::Extent);
    ExtentRef ref{offset, static_cast<std::uint32_t>(stored->size()), 0};
    ref.crc = crc32(0, stored->data(), stored->size());
    auto written = pwrite_all(fd_, stored->data(), stored->size(), offset);
    if (!written) {
        latch(written.error());
        return dstd::Unexpected<dstd::Error>(written.error());
    }
    return ref;
}

dstd::Expected<DictRef, dstd::Error> DataFileWriter::append_dict(
    dstd::ByteView bytes) {
    if (latched()) return dstd::Unexpected<dstd::Error>(latched_result());
    if (closed_.load(std::memory_order_acquire)) return invalid_error();
    if (bytes.empty() || bytes.size() > PAGE_DICT_BYTES) return invalid_error();
    dstd::Vector<std::uint8_t> padded(
        dstd::bits::align_up(bytes.size(), EXTENT_ALIGN_BYTES),
        std::uint8_t{0});
    std::memcpy(padded.data(), bytes.data(), bytes.size());
    DictRef ref;
    ref.offset = reserve_range(padded.size(), EXTENT_ALIGN_BYTES,
                               io_fault::WriteKind::Extent);
    ref.length = static_cast<std::uint32_t>(bytes.size());
    ref.crc = crc32(0, padded.data(), bytes.size());
    auto written = pwrite_all(fd_, padded.data(), padded.size(), ref.offset);
    if (!written) {
        latch(written.error());
        return dstd::Unexpected<dstd::Error>(written.error());
    }
    return ref;
}

dstd::Expected<ExtentRef, dstd::Error> DataFileWriter::append_big(
    dstd::ByteView bytes) {
    if (latched()) return dstd::Unexpected<dstd::Error>(latched_result());
    if (closed_.load(std::memory_order_acquire)) return invalid_error();
    if (bytes.empty() ||
        bytes.size() >= std::numeric_limits<std::uint32_t>::max()) {
        return invalid_error();
    }
    dstd::Vector<std::uint8_t> packed;
    const bool compressed = compress_meta(bytes, BIG_ZSTD_LEVEL, packed);
    const std::uint8_t none = static_cast<std::uint8_t>(PageCodec::None);
    const std::size_t length = compressed ? packed.size() : 1 + bytes.size();
    const std::uint64_t offset =
        reserve_range(length, FILE_HEADER_BYTES, io_fault::WriteKind::Big);
    ExtentRef ref{offset, static_cast<std::uint32_t>(length), 0};
    const auto write = [this](const std::uint8_t* data, std::size_t size,
                              std::uint64_t at) {
        auto written = pwrite_all(fd_, data, size, at);
        if (!written) {
            latch(written.error());
            return written;
        }
        io_fault::on_write(fd_, io_fault::WriteKind::Big, at, data, size);
        return written;
    };
    if (compressed) {
        ref.crc = crc32(0, packed.data(), packed.size());
        auto written = write(packed.data(), packed.size(), offset);
        if (!written) return dstd::Unexpected<dstd::Error>(written.error());
        return ref;
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    ref.crc = crc32(crc32(0, &none, 1), data, bytes.size());
    auto head = write(&none, 1, offset);
    if (!head) return dstd::Unexpected<dstd::Error>(head.error());
    auto body = write(data, bytes.size(), offset + 1);
    if (!body) return dstd::Unexpected<dstd::Error>(body.error());
    return ref;
}

std::uint64_t DataFileWriter::reserve_range(std::uint64_t length,
                                            std::uint64_t align,
                                            io_fault::WriteKind kind) {
    std::uint64_t current = next_offset_.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint64_t start = dstd::bits::align_up(current, align);
        if (next_offset_.compare_exchange_weak(current, start + length,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
            if (kind == io_fault::WriteKind::Extent) {
                io_fault::on_write(fd_, kind, start, nullptr, length);
            }
            return start;
        }
    }
}

void DataFileWriter::add(RecordItem item) {
    if (latched() || closed_.load(std::memory_order_acquire)) return;
    // An extent ref lives in its commit's record only. Checkpoints and the
    // footer list the commit by summary.
    if (const auto* copy = std::get_if<CopyItem>(&item)) {
        // A later item of the same copy publishes it, so it is listed once.
        const auto known =
            copy_item_at_.try_emplace(copy->local_id, all_items_.size());
        if (!known.second) {
            all_items_[known.first->second] = *copy;
            pending_items_.push_back(std::move(item));
            return;
        }
    }
    if (!std::holds_alternative<ExtentRefItem>(item))
        all_items_.push_back(item);
    pending_items_.push_back(std::move(item));
}

RecordItems DataFileWriter::items_with_pending_extents() const {
    RecordItems items = all_items_;
    for (const RecordItem& item : pending_items_) {
        if (std::holds_alternative<ExtentRefItem>(item)) items.push_back(item);
    }
    return items;
}

dstd::Expected<FileTail, dstd::Error> DataFileWriter::build_blocks(
    const RecordItems& items, std::uint64_t base,
    dstd::Vector<std::uint8_t>& out) const {
    // An empty block takes no bytes and a zero handle.
    const auto place = [base, &out](const dstd::Vector<std::uint8_t>& raw) {
        if (raw.empty()) return BlockHandle{};
        const dstd::Vector<std::uint8_t> bytes =
            encode_meta(bytes_of(raw), META_ZSTD_LEVEL);
        BlockHandle handle;
        handle.offset = base + out.size();
        handle.length = static_cast<std::uint32_t>(bytes.size());
        handle.crc = crc32(0, bytes.data(), bytes.size());
        out.insert(out.end(), bytes.begin(), bytes.end());
        return handle;
    };
    // Sorted by offset and deduplicated, so an extent's index in this order
    // names it.
    dstd::Vector<const ExtentRefItem*> extents;
    for (const ExtentRefItem& extent : committed_extents_) {
        if (!extent.segments.empty()) extents.push_back(&extent);
    }
    for (const RecordItem& item : items) {
        const auto* extent = std::get_if<ExtentRefItem>(&item);
        if (extent != nullptr && !extent->segments.empty())
            extents.push_back(extent);
    }
    std::stable_sort(extents.begin(), extents.end(),
                     [](const ExtentRefItem* a, const ExtentRefItem* b) {
                         return a->offset < b->offset;
                     });
    std::size_t kept = 0;
    for (std::size_t i = 0; i < extents.size(); ++i) {
        if (kept > 0 && extents[kept - 1]->offset == extents[i]->offset)
            continue;
        extents[kept++] = extents[i];
    }
    extents.resize(kept);
    if (extents.size() > std::numeric_limits<std::uint32_t>::max()) {
        return invalid_error();
    }
    // Packed copy by copy. A block never spans copies and never splits a run,
    // so a run larger than the limit has a block of its own.
    struct SegmentKey {
        std::uint32_t copy_id = 0;
        std::uint64_t sequence = 0;
    };
    const auto less = [](const SegmentKey& a, const SegmentKey& b) {
        if (a.copy_id != b.copy_id) return a.copy_id < b.copy_id;
        return a.sequence < b.sequence;
    };
    struct Placed {
        SegmentKey key;
        std::uint32_t extent = 0;
        const ExtentRefItem* ref = nullptr;
        const ExtentSegment* segment = nullptr;
    };
    dstd::Vector<Placed> placed;
    for (std::size_t extent = 0; extent < extents.size(); ++extent) {
        for (const ExtentSegment& segment : extents[extent]->segments) {
            placed.push_back({{segment.copy_id, extents[extent]->sequence},
                              static_cast<std::uint32_t>(extent),
                              extents[extent],
                              &segment});
        }
    }
    std::sort(placed.begin(), placed.end(),
              [&less](const Placed& a, const Placed& b) {
                  if (less(a.key, b.key)) return true;
                  if (less(b.key, a.key)) return false;
                  if (a.extent != b.extent) return a.extent < b.extent;
                  return a.segment->offset_in_extent <
                         b.segment->offset_in_extent;
              });
    dstd::Vector<BlockHandle> block_handles;
    dstd::Vector<std::pair<SegmentKey, std::uint32_t>> runs;
    dstd::Vector<std::uint8_t> block;
    dstd::Vector<std::uint8_t> run_bytes;
    std::uint32_t block_entries = 0;
    std::uint32_t block_copy = 0;
    const auto flush = [&block, &block_handles, &block_entries, &place]() {
        dstd::Vector<std::uint8_t> bytes;
        dstd::append_u32_le(bytes, block_entries);
        bytes.insert(bytes.end(), block.begin(), block.end());
        block_handles.push_back(place(bytes));
        block.clear();
        block_entries = 0;
    };
    for (std::size_t first = 0; first < placed.size();) {
        std::size_t end = first + 1;
        while (end < placed.size() &&
               !less(placed[first].key, placed[end].key)) {
            ++end;
        }
        run_bytes.clear();
        for (std::size_t at = first; at < end; ++at) {
            const Placed& entry = placed[at];
            dstd::append_u32_le(run_bytes, entry.extent);
            dstd::append_u64_le(run_bytes, entry.key.sequence);
            dstd::append_u64_le(run_bytes, entry.ref->offset);
            dstd::append_u32_le(run_bytes, entry.ref->length);
            dstd::append_u32_le(run_bytes, entry.ref->crc);
            dstd::append_u32_le(run_bytes, entry.segment->offset_in_extent);
            dstd::append_u32_le(run_bytes, entry.segment->length);
            dstd::append_u64_le(run_bytes, entry.segment->entry_count);
            dstd::append_u64_le(run_bytes, entry.segment->ordinal);
            for (const auto* key :
                 {&entry.segment->first_key, &entry.segment->last_key}) {
                encode_varint(run_bytes, key->size());
                run_bytes.insert(run_bytes.end(), key->begin(), key->end());
            }
        }
        if (block_entries > 0 &&
            (block_copy != placed[first].key.copy_id ||
             4 + block.size() + run_bytes.size() > SEGMENT_BLOCK_BYTES)) {
            flush();
        }
        runs.push_back({placed[first].key,
                        static_cast<std::uint32_t>(block_handles.size())});
        block.insert(block.end(), run_bytes.begin(), run_bytes.end());
        block_entries += static_cast<std::uint32_t>(end - first);
        block_copy = placed[first].key.copy_id;
        first = end;
    }
    if (block_entries > 0) flush();
    dstd::Vector<std::pair<SegmentKey, BlockHandle>> handles;
    handles.reserve(runs.size());
    for (const std::pair<SegmentKey, std::uint32_t>& run : runs) {
        handles.push_back({run.first, block_handles[run.second]});
    }
    const auto segment_handle = [&handles, &less](std::uint64_t sequence,
                                                  std::uint32_t copy_id) {
        const SegmentKey wanted{copy_id, sequence};
        const auto at = std::lower_bound(
            handles.begin(), handles.end(), wanted,
            [&less](const std::pair<SegmentKey, BlockHandle>& entry,
                    const SegmentKey& key) { return less(entry.first, key); });
        if (at == handles.end() || less(wanted, at->first))
            return BlockHandle{};
        return at->second;
    };

    dstd::Vector<NamespaceItem> namespaces;
    dstd::Vector<ScopeItem> scopes;
    dstd::Vector<CopyItem> copies;
    dstd::Vector<DropItem> drops;
    dstd::Vector<EraseItem> erases;
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
        } else if (const auto* erase = std::get_if<EraseItem>(&item)) {
            erases.push_back(*erase);
        } else if (const auto* commit = std::get_if<CommitRefItem>(&item)) {
            for (const CommitCopy& range : commit->copies) {
                commits.push_back(
                    {commit->sequence, range.copy_id, bytes_of(range.first_key),
                     bytes_of(range.last_key),
                     segment_handle(commit->sequence, range.copy_id)});
            }
        } else if (const auto* extent = std::get_if<ExtentRefItem>(&item)) {
            for (const ExtentSegment& segment : extent->segments) {
                commits.push_back(
                    {extent->sequence, segment.copy_id,
                     bytes_of(segment.first_key), bytes_of(segment.last_key),
                     segment_handle(extent->sequence, segment.copy_id)});
            }
        }
    }
    auto directory = build_scope_directory(namespaces, scopes, copies, drops,
                                           erases, commits);
    if (!directory) {
        return dstd::Unexpected<dstd::Error>(directory.error());
    }
    auto entries = ScopeDirectory::open(bytes_of(*directory));
    if (!entries) return dstd::Unexpected<dstd::Error>(entries.error());
    dstd::Vector<std::uint64_t> scope_hashes;
    scope_hashes.reserve(entries->size());
    for (std::size_t i = 0; i < entries->size(); ++i) {
        auto key = entries->key(i);
        if (!key) return dstd::Unexpected<dstd::Error>(key.error());
        scope_hashes.push_back(
            scope_hash(key->name, key->format_version, key->kind,
                       dstd::Span<const dstd::StringView>(key->paths.data(),
                                                          key->paths.size())));
    }

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
    tail.extent_size = extent_size_;
    tail.writer_random = writer_random_;
    tail.pid = pid_;
    tail.host = host_;
    tail.label = label_;
    tail.creation_time = creation_time_;
    tail.items = items_block;
    tail.extent_count = static_cast<std::uint32_t>(extents.size());
    tail.directory_levels = levels;
    tail.top_table = std::move(top_table);
    tail.scope_filter = build_scope_filter(scope_hashes);
    return tail;
}

void DataFileWriter::add_commit_refs() {
    struct Item {
        std::uint64_t sequence;
        std::uint32_t copy_id;
        const ExtentSegment* segment;
    };
    dstd::Vector<Item> items;
    dstd::Vector<CommitRefItem> refs;
    for (const RecordItem& item : pending_items_) {
        const auto* extent = std::get_if<ExtentRefItem>(&item);
        if (extent == nullptr) continue;
        std::size_t at = 0;
        while (at < refs.size() && refs[at].sequence != extent->sequence) ++at;
        if (at == refs.size()) {
            CommitRefItem fresh;
            fresh.sequence = extent->sequence;
            fresh.record_offset = tail_offset_;
            fresh.record_crc = tail_crc_;
            fresh.synced = 1;
            refs.push_back(std::move(fresh));
        }
        if (extent->synced == 0) refs[at].synced = 0;
        for (const ExtentSegment& segment : extent->segments) {
            items.push_back({extent->sequence, segment.copy_id, &segment});
        }
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.sequence != b.sequence) return a.sequence < b.sequence;
        return a.copy_id < b.copy_id;
    });
    for (std::size_t at = 0; at < items.size();) {
        const ExtentSegment* low = items[at].segment;
        const ExtentSegment* high = low;
        std::size_t end = at + 1;
        while (end < items.size() &&
               items[end].sequence == items[at].sequence &&
               items[end].copy_id == items[at].copy_id) {
            const ExtentSegment& segment = *items[end].segment;
            if (segment.first_key < low->first_key) low = &segment;
            if (high->last_key < segment.last_key) high = &segment;
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

    // A record names its own offset, so its size is measured at a placeholder
    // offset first. The item encoding does not depend on it.
    auto probe = encode_record(actual_kind, sequence, tail_offset_,
                               FILE_HEADER_BYTES, items);
    if (!probe) return dstd::Unexpected<dstd::Error>(probe.error());
    const std::uint64_t offset = reserve_range(probe->size(), FILE_HEADER_BYTES,
                                               fault_kind_of(actual_kind));
    dstd::Vector<std::uint8_t> encoded = std::move(*probe);
    if (offset != FILE_HEADER_BYTES) set_record_offset(encoded, offset);

    auto written = pwrite_all(fd_, encoded.data(), encoded.size(), offset);
    if (!written) {
        latch(written.error());
        return written;
    }
    io_fault::on_write(fd_, fault_kind_of(actual_kind), offset, encoded.data(),
                       encoded.size());

    const std::uint32_t body_length = dstd::read_u32_le(encoded.data() + 12);
    tail_offset_ = offset;
    tail_kind_ = static_cast<std::uint32_t>(actual_kind);
    tail_crc_ =
        dstd::read_u32_le(encoded.data() + RECORD_HEADER_BYTES + body_length);
    last_sequence_ = sequence;
    return {};
}

dstd::Expected<void, dstd::Error> DataFileWriter::commit() {
    if (latched()) return dstd::Unexpected<dstd::Error>(latched_result());
    if (closed_.load(std::memory_order_acquire)) return invalid_error();
    const bool checkpoint = (last_sequence_ + 1) % CHECKPOINT_INTERVAL == 0;
    auto record = checkpoint ? write_record(RecordKind::Commit,
                                            items_with_pending_extents())
                             : write_record(RecordKind::Commit, pending_items_);
    if (!record) return record;
    for (const RecordItem& item : pending_items_) {
        if (const auto* extent = std::get_if<ExtentRefItem>(&item)) {
            committed_extents_.push_back(*extent);
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
    const RecordItems items = items_with_pending_extents();
    // The range is taken only if no write took the end of the file meanwhile.
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
    // Every extent is synced before the footer, so a reader trusts the extents
    // of a closed file one by one.
    auto extents_synced = sync_data();
    if (!extents_synced) {
        latch(extents_synced.error());
        return extents_synced;
    }
    // The items block holds every other item again, and only a full open reads
    // it.
    RecordItems footer;
    for (const RecordItem& item : items) {
        if (std::holds_alternative<NamespaceItem>(item)) footer.push_back(item);
    }
    footer.push_back(dstd::make_shared<FileTail>(std::move(*tail)));
    auto record = write_record(RecordKind::Footer, footer);
    if (!record) return record;

    const std::uint64_t trailer_offset =
        reserve_range(FILE_TRAILER_BYTES, 1, io_fault::WriteKind::Trailer);
    const std::uint32_t footer_length =
        static_cast<std::uint32_t>(trailer_offset - tail_offset_);
    dstd::Vector<std::uint8_t> trailer(FILE_TRAILER_BYTES, 0);
    std::memcpy(trailer.data(), TRAILER_MAGIC.data(), TRAILER_MAGIC.size());
    dstd::write_u64_le(trailer.data() + 8, tail_offset_);
    dstd::write_u32_le(trailer.data() + 16, footer_length);
    const std::uint32_t trailer_crc = crc32(0, trailer.data(), 20);
    dstd::write_u32_le(trailer.data() + 20, trailer_crc);
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
    // A failed append only leaves the file for readers to open as before.
    append_summary(path_, base + blocks.size(), last_sequence_, footer);
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
    reader.fd_.store(fd, std::memory_order_relaxed);
    reader.file_size_ = file_size;

    // One bounded read of the file end.
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
    auto buffer = pread_all(fd_.load(std::memory_order_relaxed), start,
                            static_cast<std::size_t>(want));
    if (!buffer) return false;
    const dstd::ByteView bytes = bytes_of(*buffer);

    const std::size_t trailer_at =
        static_cast<std::size_t>(want) - FILE_TRAILER_BYTES;
    const auto* trailer =
        reinterpret_cast<const std::uint8_t*>(bytes.data() + trailer_at);
    if (std::memcmp(trailer, TRAILER_MAGIC.data(), TRAILER_MAGIC.size()) != 0) {
        return false;
    }
    if (dstd::read_u32_le(trailer + 20) !=
        crc32(0, trailer, FILE_TRAILER_BYTES - 12)) {
        return false;
    }
    const std::uint64_t tail_offset = dstd::read_u64_le(trailer + 8);
    const std::uint32_t footer_length = dstd::read_u32_le(trailer + 16);
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
        decoded = read_record_at(fd_.load(std::memory_order_relaxed), file_size,
                                 tail_offset, records_read_);
    }
    if (!decoded || decoded->kind != RecordKind::Footer) return false;

    return apply_footer(decoded->items, decoded->sequence, file_size);
}

dstd::Expected<bool, dstd::Error> DataFileReader::apply_footer(
    const RecordItems& items, std::uint64_t sequence, std::uint64_t file_size) {
    const FileTail* tail = nullptr;
    for (const RecordItem& item : items) {
        if (const auto* namespace_item = std::get_if<NamespaceItem>(&item)) {
            index_.namespaces.push_back(*namespace_item);
        } else if (const auto* file_tail = std::get_if<FileTailPtr>(&item)) {
            tail = file_tail->get();
        }
    }
    if (tail == nullptr) return false;
    if (tail->format_version != FORMAT_VERSION) return unsupported_error();
    // Each extent takes EXTENT_ALIGN_BYTES of the file at least.
    if (tail->extent_count > file_size / EXTENT_ALIGN_BYTES + 1) return false;

    index_.extent_size = tail->extent_size;
    index_.writer_random = tail->writer_random;
    index_.pid = tail->pid;
    index_.host = tail->host;
    index_.label = tail->label;
    index_.creation_time = tail->creation_time;
    index_.last_sequence = sequence;
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
    : fd_(other.fd_.exchange(-1, std::memory_order_relaxed)),
      file_size_(other.file_size_),
      records_read_(other.records_read_),
      index_(std::move(other.index_)),
      path_(std::move(other.path_)),
      open_mutex_(std::move(other.open_mutex_)) {}

DataFileReader& DataFileReader::operator=(DataFileReader&& other) noexcept {
    if (this == &other) return *this;
    const int held =
        fd_.exchange(other.fd_.exchange(-1, std::memory_order_relaxed),
                     std::memory_order_relaxed);
    if (held >= 0) ::close(held);
    file_size_ = other.file_size_;
    records_read_ = other.records_read_;
    index_ = std::move(other.index_);
    path_ = std::move(other.path_);
    open_mutex_ = std::move(other.open_mutex_);
    return *this;
}

DataFileReader::~DataFileReader() {
    const int held = fd_.load(std::memory_order_relaxed);
    if (held >= 0) ::close(held);
}

dstd::Expected<int, dstd::Error> DataFileReader::descriptor() const {
    const int known = fd_.load(std::memory_order_acquire);
    if (known >= 0 || open_mutex_ == nullptr) return known;
    std::lock_guard<std::mutex> locked(*open_mutex_);
    const int again = fd_.load(std::memory_order_acquire);
    if (again >= 0) return again;
    const std::string path = path_string(path_);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return io_error();
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return io_error();
    }
    // Shorter than the summary says: not the file the entry lists.
    if (st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) < file_size_) {
        ::close(fd);
        return parse_error();
    }
    fd_.store(fd, std::memory_order_release);
    return fd;
}

dstd::Expected<DataFileReader, dstd::Error> DataFileReader::from_summary(
    dstd::StringView path, const SummaryEntry& entry) {
    DataFileReader reader;
    reader.file_size_ = entry.needed_bytes;
    reader.path_.assign(path.data(), path.size());
    reader.open_mutex_ = dstd::make_unique<std::mutex>();
    auto applied =
        reader.apply_footer(entry.footer, entry.sequence, entry.needed_bytes);
    if (!applied) return dstd::Unexpected<dstd::Error>(applied.error());
    if (!*applied) return parse_error();
    return reader;
}

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error>
DataFileReader::read_extent(const ExtentRef& ref) const {
    auto fd = descriptor();
    if (!fd) return dstd::Unexpected<dstd::Error>(fd.error());
    if (ref.length == 0 || ref.offset > file_size_ ||
        ref.length > file_size_ - ref.offset) {
        return parse_error();
    }
    auto bytes = pread_all(*fd, ref.offset, ref.length);
    if (!bytes) return dstd::Unexpected<dstd::Error>(bytes.error());
    const std::uint32_t crc = crc32(0, bytes->data(), bytes->size());
    if (crc != ref.crc) return parse_error();
    return bytes;
}

dstd::Expected<void, dstd::Error> DataFileReader::read_range(
    std::uint64_t offset, std::size_t length,
    dstd::RawBuffer<std::uint8_t>& out, BlockReads* reads) const {
    auto fd = descriptor();
    if (!fd) return dstd::Unexpected<dstd::Error>(fd.error());
    if (offset > file_size_ || length > file_size_ - offset) {
        return parse_error();
    }
    out.resize(length);
    std::size_t done = 0;
    while (done < length) {
        const ssize_t got = ::pread(*fd, out.data() + done, length - done,
                                    static_cast<off_t>(offset + done));
        if (got < 0) {
            if (errno == EINTR) continue;
            return io_error();
        }
        if (got == 0) return parse_error();
        if (reads != nullptr) {
            ++reads->calls;
            reads->bytes += static_cast<std::uint64_t>(got);
        }
        done += static_cast<std::size_t>(got);
    }
    return {};
}

dstd::Expected<void, dstd::Error> DataFileReader::warm_range(
    std::uint64_t offset, std::uint64_t length, BlockReads* reads) const {
    auto fd = descriptor();
    if (!fd) return dstd::Unexpected<dstd::Error>(fd.error());
    if (offset >= file_size_) return {};
    const std::uint64_t end = offset + std::min(length, file_size_ - offset);
    dstd::Vector<std::uint8_t> chunk(static_cast<std::size_t>(
        std::min<std::uint64_t>(VERIFY_CHUNK_BYTES, end - offset)));
    std::uint64_t at = offset;
    while (at < end) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk.size(), end - at));
        const ssize_t got =
            ::pread(*fd, chunk.data(), want, static_cast<off_t>(at));
        if (got < 0) {
            if (errno == EINTR) continue;
            return io_error();
        }
        if (got == 0) break;
        if (reads != nullptr) {
            ++reads->calls;
            reads->bytes += static_cast<std::uint64_t>(got);
        }
        at += static_cast<std::uint64_t>(got);
    }
    return {};
}

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error>
DataFileReader::read_block(const BlockHandle& handle, BlockReads* reads) const {
    auto fd = descriptor();
    if (!fd) return dstd::Unexpected<dstd::Error>(fd.error());
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
    auto bytes = pread_all(*fd, handle.offset, handle.length);
    if (!bytes) return dstd::Unexpected<dstd::Error>(bytes.error());
    if (crc32(0, bytes->data(), bytes->size()) != handle.crc) {
        return parse_error();
    }
    if (!bytes->empty() &&
        bytes->front() == static_cast<std::uint8_t>(PageCodec::None)) {
        bytes->erase(bytes->begin());
        return bytes;
    }
    return decode_meta(bytes_of(*bytes));
}

dstd::Expected<void, dstd::Error> DataFileReader::verify_block(
    const ExtentRef& ref, BlockReads* reads) const {
    auto fd = descriptor();
    if (!fd) return dstd::Unexpected<dstd::Error>(fd.error());
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
        const ssize_t got = ::pread(*fd, chunk.data(), want,
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
        crc = crc32(crc, chunk.data(), static_cast<std::size_t>(got));
        done += static_cast<std::uint64_t>(got);
    }
    if (crc != ref.crc) return parse_error();
    return {};
}

dstd::Expected<BlockReads, dstd::Error> DataFileReader::read_extents(
    dstd::Span<const ExtentRef> refs,
    dstd::Span<const ExtentTarget> targets) const {
    auto fd = descriptor();
    if (!fd) return dstd::Unexpected<dstd::Error>(fd.error());
    const auto in_file = [this](const ExtentRef& ref) {
        return ref.length != 0 && ref.offset <= file_size_ &&
               ref.length <= file_size_ - ref.offset;
    };
    const auto adjacent = [](const ExtentRef& previous, const ExtentRef& next) {
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
        std::size_t end = at + 1;
        while (end < refs.size() && end - at < IOV_MAX && in_file(refs[end]) &&
               adjacent(refs[end - 1U], refs[end])) {
            ++end;
        }

        vecs.resize(end - at);
        std::uint64_t group_bytes = 0;
        for (std::size_t i = at; i < end; ++i) {
            const ExtentRef& ref = refs[i];
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
                ::preadv(*fd, cur, static_cast<int>(vecs.size() - served),
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
                struct iovec* extent = &vecs[served];
                if (left < extent->iov_len) {
                    extent->iov_base =
                        static_cast<char*>(extent->iov_base) + left;
                    extent->iov_len -= left;
                    left = 0;
                } else {
                    left -= extent->iov_len;
                    ++served;
                }
            }
        }

        for (std::size_t i = at; i < end; ++i) {
            const ExtentRef& ref = refs[i];
            dstd::Vector<std::uint8_t>& bytes = *targets[i].bytes;
            const bool complete = i < at + served;
            if (complete && crc32(0, bytes.data(), bytes.size()) == ref.crc) {
                continue;
            }
            // A bad extent keeps no bytes.
            *targets[i].bad = 1;
            dstd::Vector<std::uint8_t>().swap(bytes);
        }
        at = end;
    }
    return counts;
}

dstd::Expected<dstd::Vector<ExtentRefItem>, dstd::Error>
DataFileReader::read_commit(const CommitRefItem& ref, BlockReads* reads) const {
    auto fd = descriptor();
    if (!fd) return dstd::Unexpected<dstd::Error>(fd.error());
    auto length = read_record_length(*fd, file_size_, ref.record_offset);
    if (!length) return dstd::Unexpected<dstd::Error>(length.error());
    auto bytes = pread_all(*fd, ref.record_offset, *length);
    if (!bytes) return dstd::Unexpected<dstd::Error>(bytes.error());
    if (reads != nullptr) {
        reads->calls += 2;
        reads->bytes += RECORD_HEADER_BYTES + *length;
    }
    auto record = decode_record_at(bytes_of(*bytes), ref.record_offset);
    if (!record) return dstd::Unexpected<dstd::Error>(record.error());
    if (record->crc != ref.record_crc) return parse_error();
    dstd::Vector<ExtentRefItem> refs;
    for (RecordItem& item : record->items) {
        auto* extent = std::get_if<ExtentRefItem>(&item);
        if (extent != nullptr && extent->sequence == ref.sequence) {
            refs.push_back(std::move(*extent));
        }
    }
    return refs;
}

dstd::Expected<dstd::Vector<IndexedExtentRef>, dstd::Error>
DataFileReader::all_extent_refs() const {
    dstd::Vector<IndexedExtentRef> refs = index_.extent_refs;
    for (const CommitRefItem& commit : index_.commit_refs) {
        auto listed = read_commit(commit, nullptr);
        if (!listed) return dstd::Unexpected<dstd::Error>(listed.error());
        for (ExtentRefItem& extent : *listed) {
            refs.push_back({commit.sequence, std::move(extent)});
        }
    }
    std::stable_sort(refs.begin(), refs.end(),
                     [](const IndexedExtentRef& a, const IndexedExtentRef& b) {
                         return a.sequence < b.sequence;
                     });
    return refs;
}

namespace {

namespace dstd = dftracer::utils::dstd;

/// One entry of a segment block: u32 extent index, u64 sequence, u64 offset,
/// u32 length, u32 crc, u32 offset in the extent, u32 segment length, u64 entry
/// count, u64 ordinal, then the first and the last key, each as a varint length
/// and its bytes.
constexpr std::size_t SEGMENT_ENTRY_BYTES = 52;

struct SpaceRange {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

bool space_range_before(const SpaceRange& a, const SpaceRange& b) {
    return a.offset < b.offset;
}

std::uint64_t space_range_end_at(dstd::Span<const SpaceRange> ranges,
                                 std::uint64_t offset) {
    const auto after =
        std::upper_bound(ranges.begin(), ranges.end(), offset,
                         [](std::uint64_t value, const SpaceRange& range) {
                             return value < range.offset;
                         });
    if (after == ranges.begin()) return 0;
    const SpaceRange& range = *(after - 1);
    return offset < range.offset + range.length ? range.offset + range.length
                                                : 0;
}

/// One space report being built: the bytes already classified, so the last
/// pass adds up what is left, and the numbers.
struct SpaceScan {
    const DataFileReader* reader = nullptr;
    SpaceReport report;
    dstd::Vector<SpaceRange> ranges;

    void cover(std::uint64_t offset, std::uint64_t length) {
        if (length != 0) ranges.push_back({offset, length});
    }
};

dstd::Expected<void, dstd::Error> space_extents(SpaceScan& scan) {
    auto refs = scan.reader->all_extent_refs();
    if (!refs) return dstd::Unexpected<dstd::Error>(refs.error());
    std::stable_sort(refs->begin(), refs->end(),
                     [](const IndexedExtentRef& a, const IndexedExtentRef& b) {
                         return a.ref.offset < b.ref.offset;
                     });
    dstd::RawBuffer<std::uint8_t> head;
    dstd::RawBuffer<std::uint8_t> table_bytes;
    dstd::Vector<DictRef> dicts;
    for (std::size_t at = 0; at < refs->size();) {
        const ExtentRefItem& item = (*refs)[at].ref;
        const ExtentRef ref{item.offset, item.length, item.crc};
        std::size_t end = at + 1;
        while (end < refs->size() && (*refs)[end].ref.offset == item.offset) {
            ++end;
        }
        at = end;
        if (ref.length == 0 || ref.offset > scan.report.file_bytes ||
            ref.length > scan.report.file_bytes - ref.offset) {
            return parse_error();
        }
        scan.cover(ref.offset, ref.length);
        // A run that holds a big value has no page table: its bytes stay in the
        // gap.
        if (item.segments.empty()) continue;
        ++scan.report.extents;
        scan.report.extent_bytes += ref.length;
        auto read = scan.reader->read_range(ref.offset, EXTENT_HEADER_BYTES,
                                            head, nullptr);
        if (!read) return dstd::Unexpected<dstd::Error>(read.error());
        const std::size_t table = ExtentPages::table_length(bytes_of(head));
        if (table <= EXTENT_HEADER_BYTES || table > ref.length) {
            return parse_error();
        }
        read = scan.reader->read_range(ref.offset, table, table_bytes, nullptr);
        if (!read) return dstd::Unexpected<dstd::Error>(read.error());
        auto pages = ExtentPages::open_table(bytes_of(table_bytes), ref.length);
        if (!pages) return dstd::Unexpected<dstd::Error>(pages.error());
        scan.report.extent_page_table_bytes += table - EXTENT_HEADER_BYTES;
        if (!pages->dict().empty()) dicts.push_back(pages->dict());
        std::uint64_t stored = 0;
        for (std::size_t page = 0; page < pages->size(); ++page) {
            const PageEntry& entry = pages->entry(page);
            ++scan.report.pages;
            scan.report.page_bytes += entry.stored_length;
            scan.report.page_raw_bytes += entry.raw_length;
            if (entry.codec == PageCodec::Zstd ||
                entry.codec == PageCodec::ZstdDict) {
                ++scan.report.pages_compressed;
            }
            if (entry.codec == PageCodec::ZstdDict) ++scan.report.pages_dict;
            stored += entry.stored_length;
        }
        if (stored > ref.length - table) return parse_error();
        scan.report.extent_padding_bytes += ref.length - table - stored;
    }
    std::sort(
        dicts.begin(), dicts.end(),
        [](const DictRef& a, const DictRef& b) { return a.offset < b.offset; });
    for (std::size_t at = 0; at < dicts.size(); ++at) {
        if (at > 0 && dicts[at - 1].offset == dicts[at].offset) continue;
        const std::uint64_t length =
            dstd::bits::align_up(dicts[at].length, EXTENT_ALIGN_BYTES);
        if (dicts[at].offset > scan.report.file_bytes ||
            length > scan.report.file_bytes - dicts[at].offset) {
            return parse_error();
        }
        ++scan.report.dicts;
        scan.report.dict_bytes += length;
        scan.cover(dicts[at].offset, length);
    }
    return {};
}

dstd::Expected<void, dstd::Error> space_segment_block(
    SpaceScan& scan, const BlockHandle& handle) {
    auto bytes = scan.reader->read_block(handle, nullptr);
    if (!bytes) return dstd::Unexpected<dstd::Error>(bytes.error());
    scan.report.segment_block_bytes += handle.length;
    scan.cover(handle.offset, handle.length);
    const dstd::Vector<std::uint8_t>& body = *bytes;
    if (body.size() < 4) return parse_error();
    const std::uint64_t count = dstd::read_u32_le(body.data());
    if (count > (body.size() - 4) / SEGMENT_ENTRY_BYTES) return parse_error();
    std::size_t at = 4;
    for (std::uint64_t i = 0; i < count; ++i) {
        if (SEGMENT_ENTRY_BYTES > body.size() - at) return parse_error();
        at += SEGMENT_ENTRY_BYTES;
        for (std::size_t key = 0; key < 2; ++key) {
            auto length = decode_varint(bytes_of(body), at);
            if (!length || *length > body.size() - at) return parse_error();
            scan.report.segment_key_bytes += *length;
            at += static_cast<std::size_t>(*length);
        }
        ++scan.report.segment_entries;
    }
    if (at != body.size()) return parse_error();
    return {};
}

/// Walks the directory from its top table down to its leaves. The segment
/// blocks the entries name leave by reference.
dstd::Expected<void, dstd::Error> space_directory(
    SpaceScan& scan, const FileTail& tail,
    dstd::Vector<BlockHandle>& segment_blocks) {
    struct Pending {
        BlockHandle handle;
        std::uint32_t level = 0;
    };
    const auto child_of = [](dstd::ByteView payload, BlockHandle& child) {
        std::size_t at = 0;
        return read_block_handle(payload, at, child) && at == payload.size();
    };
    auto top = SlotTable::open(bytes_of(tail.top_table));
    if (!top) return dstd::Unexpected<dstd::Error>(top.error());
    dstd::Vector<Pending> pending;
    pending.reserve(top->size());
    for (std::size_t i = 0; i < top->size(); ++i) {
        BlockHandle child;
        if (!child_of(top->payload(i), child)) return parse_error();
        pending.push_back({child, tail.directory_levels});
    }
    while (!pending.empty()) {
        const Pending block = pending.back();
        pending.pop_back();
        auto bytes = scan.reader->read_block(block.handle, nullptr);
        if (!bytes) return dstd::Unexpected<dstd::Error>(bytes.error());
        auto slots = SlotTable::open(bytes_of(*bytes));
        if (!slots) return dstd::Unexpected<dstd::Error>(slots.error());
        scan.cover(block.handle.offset, block.handle.length);
        if (block.level > 1) {
            scan.report.directory_separator_bytes += block.handle.length;
            for (std::size_t i = 0; i < slots->size(); ++i) {
                BlockHandle child;
                if (!child_of(slots->payload(i), child)) return parse_error();
                pending.push_back({child, block.level - 1});
            }
            continue;
        }
        scan.report.directory_leaf_bytes += block.handle.length;
        auto directory = ScopeDirectory::open(bytes_of(*bytes));
        if (!directory) return dstd::Unexpected<dstd::Error>(directory.error());
        for (std::size_t i = 0; i < directory->size(); ++i) {
            scan.report.directory_key_bytes += slots->key(i).size();
            auto entry = directory->entry(i);
            if (!entry) return dstd::Unexpected<dstd::Error>(entry.error());
            ++scan.report.directory_entries;
            for (const DirectoryCopy& copy : entry->copies) {
                for (const DirectoryCommit& commit : copy.commits) {
                    scan.report.directory_commit_key_bytes +=
                        commit.first_key.size() + commit.last_key.size();
                    if (!commit.segments.empty())
                        segment_blocks.push_back(commit.segments);
                }
            }
        }
    }
    return {};
}

/// Every commit and checkpoint record of the file, found by the record magic.
/// A record a block already covers is skipped.
dstd::Expected<void, dstd::Error> space_records(SpaceScan& scan) {
    std::stable_sort(scan.ranges.begin(), scan.ranges.end(),
                     space_range_before);
    const std::uint64_t file_bytes = scan.report.file_bytes;
    dstd::RawBuffer<std::uint8_t> head;
    std::uint64_t offset = FILE_HEADER_BYTES;
    while (offset <= file_bytes && file_bytes - offset >= RECORD_HEADER_BYTES) {
        const std::uint64_t covered = space_range_end_at(
            dstd::Span<const SpaceRange>(scan.ranges), offset);
        if (covered != 0) {
            offset = dstd::bits::align_up(covered, FILE_HEADER_BYTES);
            continue;
        }
        auto read =
            scan.reader->read_range(offset, RECORD_HEADER_BYTES, head, nullptr);
        if (!read) return dstd::Unexpected<dstd::Error>(read.error());
        const std::uint8_t* data = head.data();
        if (std::memcmp(data, RECORD_MAGIC.data(), RECORD_MAGIC.size()) != 0) {
            offset += FILE_HEADER_BYTES;
            continue;
        }
        const std::uint32_t kind = dstd::read_u32_le(data + 8);
        const std::uint32_t body_length = dstd::read_u32_le(data + 12);
        const std::uint64_t crc_end = RECORD_HEADER_BYTES + body_length + 4;
        const std::uint64_t padded =
            dstd::bits::align_up(crc_end, FILE_HEADER_BYTES);
        if (kind < 1 || kind > 3 || padded < crc_end ||
            padded > file_bytes - offset) {
            offset += FILE_HEADER_BYTES;
            continue;
        }
        if (kind == static_cast<std::uint32_t>(RecordKind::Commit) ||
            kind == static_cast<std::uint32_t>(RecordKind::Checkpoint)) {
            scan.report.record_bytes += padded;
            ++scan.report.commits;
            scan.cover(offset, padded);
        }
        offset += padded;
    }
    return {};
}

}  // namespace

dstd::Expected<SpaceReport, dstd::Error> space_report(dstd::StringView path) {
    auto opened = DataFileReader::open(path, FooterItems::All);
    if (!opened) return dstd::Unexpected<dstd::Error>(opened.error());
    const FileIndex& index = opened->index();
    if (!index.closed || !index.tail.has_value()) return parse_error();
    const FileTail& tail = *index.tail;
    SpaceScan scan;
    scan.reader = &*opened;
    scan.report.file_bytes = opened->file_size();
    scan.report.header_bytes = FILE_HEADER_BYTES;
    scan.report.filter_bytes = tail.scope_filter.size();
    scan.report.copies = index.copies.size();
    if (scan.report.file_bytes < FILE_HEADER_BYTES + FILE_TRAILER_BYTES) {
        return parse_error();
    }
    scan.cover(0, FILE_HEADER_BYTES);

    // The trailer names the footer, so both are classified first: the records
    // must not take the footer record for one of theirs.
    std::uint64_t trailer_offset = scan.report.file_bytes - FILE_TRAILER_BYTES;
    dstd::RawBuffer<std::uint8_t> trailer;
    auto trailer_read = opened->read_range(trailer_offset, FILE_TRAILER_BYTES,
                                           trailer, nullptr);
    if (!trailer_read) {
        return dstd::Unexpected<dstd::Error>(trailer_read.error());
    }
    if (std::memcmp(trailer.data(), TRAILER_MAGIC.data(),
                    TRAILER_MAGIC.size()) != 0) {
        return parse_error();
    }
    if (dstd::read_u32_le(trailer.data() + 20) !=
        crc32(0, trailer.data(), FILE_TRAILER_BYTES - 12)) {
        return parse_error();
    }
    const std::uint64_t footer_offset = dstd::read_u64_le(trailer.data() + 8);
    const std::uint32_t footer_length = dstd::read_u32_le(trailer.data() + 16);
    if (footer_offset < FILE_HEADER_BYTES ||
        footer_offset % FILE_HEADER_BYTES != 0 ||
        footer_length < RECORD_HEADER_BYTES + 4 ||
        footer_length > trailer_offset ||
        footer_offset != trailer_offset - footer_length) {
        return parse_error();
    }
    scan.report.footer_bytes = footer_length + FILE_TRAILER_BYTES;
    scan.cover(footer_offset, footer_length);
    scan.cover(trailer_offset, FILE_TRAILER_BYTES);

    if (!tail.items.empty()) {
        scan.report.items_block_bytes = tail.items.length;
        scan.cover(tail.items.offset, tail.items.length);
    }
    auto extents = space_extents(scan);
    if (!extents) return dstd::Unexpected<dstd::Error>(extents.error());
    dstd::Vector<BlockHandle> segment_blocks;
    auto directory = space_directory(scan, tail, segment_blocks);
    if (!directory) return dstd::Unexpected<dstd::Error>(directory.error());
    std::stable_sort(segment_blocks.begin(), segment_blocks.end(),
                     [](const BlockHandle& a, const BlockHandle& b) {
                         return a.offset < b.offset;
                     });
    for (std::size_t at = 0; at < segment_blocks.size();) {
        const BlockHandle handle = segment_blocks[at];
        std::size_t end = at + 1;
        while (end < segment_blocks.size() &&
               segment_blocks[end].offset == handle.offset) {
            ++end;
        }
        at = end;
        auto block = space_segment_block(scan, handle);
        if (!block) return dstd::Unexpected<dstd::Error>(block.error());
    }
    auto records = space_records(scan);
    if (!records) return dstd::Unexpected<dstd::Error>(records.error());
    if (scan.report.extents != tail.extent_count) return parse_error();

    std::stable_sort(scan.ranges.begin(), scan.ranges.end(),
                     space_range_before);
    std::uint64_t classified_end = 0;
    for (const SpaceRange& range : scan.ranges) {
        if (range.offset < classified_end) return parse_error();
        scan.report.gap_bytes += range.offset - classified_end;
        classified_end = range.offset + range.length;
    }
    if (classified_end < scan.report.file_bytes) {
        scan.report.gap_bytes += scan.report.file_bytes - classified_end;
    }
    return scan.report;
}

}  // namespace dftracer::utils::store::engine
