#include <dftracer/utils/core/common/bits.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>  // fs:: portability alias
#include <dftracer/utils/core/common/scoped_fd.h>
#include <dftracer/utils/core/env.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/spill.h>
#include <dftracer/utils/dataframe/types.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <stdexcept>

namespace dftracer::utils::dataframe::spill {

namespace {

template <class T>
void put_pod(std::string& out, T v) {
    out.append(reinterpret_cast<const char*>(&v), sizeof(T));
}

template <class T>
T get_pod(const std::uint8_t*& p, const std::uint8_t* end) {
    if (p + sizeof(T) > end) throw std::runtime_error("spill: truncated read");
    T v;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
}

void put_bytes(std::string& out, const void* data, std::size_t n) {
    out.append(static_cast<const char*>(data), n);
}

const std::uint8_t* take_bytes(const std::uint8_t*& p, const std::uint8_t* end,
                               std::size_t n) {
    if (p + n > end) throw std::runtime_error("spill: truncated read");
    const std::uint8_t* start = p;
    p += n;
    return start;
}

// Validity bitmap (Arrow layout, 1 = valid) built from per-row null flags.
std::vector<std::uint8_t> validity_bitmap(const Series& s) {
    const std::int64_t n = s.length();
    std::vector<std::uint8_t> bits(static_cast<std::size_t>((n + 7) / 8), 0);
    for (std::int64_t i = 0; i < n; ++i)
        if (!s.is_null(i))
            bits[static_cast<std::size_t>(i >> 3)] |=
                static_cast<std::uint8_t>(1u << (i & 7));
    return bits;
}

// Atomic sequence so many spilling sinks in one process pick distinct dirs.
int next_seq() {
    static std::atomic<int> seq{0};
    return seq.fetch_add(1);
}

}  // namespace

namespace {
void add_new_bytes(const dftu_series* c, std::unordered_set<const void*>& seen,
                   std::size_t& total) {
    auto add = [&](const std::shared_ptr<Buffer>& b) {
        if (b && seen.insert(b.get()).second) total += b->size();
    };
    add(c->data);
    add(c->offsets);
    add(c->validity);
    // A view column's data buffers are not counted: for a scan they are the
    // string table's blocks, shared by every part and freed by no spill, and
    // walking their list per part cost a collect 37 ms.
    for (const auto& n : c->nested)
        if (n.series) add_new_bytes(n.series.get(), seen, total);
}
}  // namespace

std::size_t new_buffer_bytes(const std::vector<Series>& cols,
                             std::unordered_set<const void*>& seen) {
    std::size_t total = 0;
    for (const Series& c : cols)
        if (c.valid()) add_new_bytes(c.handle(), seen, total);
    return total;
}

std::size_t columns_bytes(const std::vector<Series>& cols) {
    std::size_t total = 0;
    for (const Series& c : cols) {
        const std::int64_t n = c.length();
        const TypeId t = c.type();
        if (c.encoding() == Encoding::Dictionary ||
            c.encoding() == Encoding::View ||
            c.encoding() == Encoding::Chunked) {
            total +=
                static_cast<std::size_t>(dftu_series_buffer_bytes(c.handle()));
        } else if (is_wide_offset_type(t)) {  // LargeString / LargeBinary
            const std::int64_t* offs = c.offsets64();
            total +=
                static_cast<std::size_t>(n + 1) * sizeof(std::int64_t) +
                (n > 0 && offs != nullptr ? static_cast<std::size_t>(offs[n])
                                          : 0);
        } else if (t == TypeId::String || t == TypeId::Binary) {
            const std::int32_t* offs = dftu_series_offsets(c.handle());
            total +=
                static_cast<std::size_t>(n + 1) * sizeof(std::int32_t) +
                (n > 0 && offs != nullptr ? static_cast<std::size_t>(offs[n])
                                          : 0);
        } else if (t == TypeId::FixedSizeBinary) {
            total +=
                static_cast<std::size_t>(n) *
                static_cast<std::size_t>(dftu_series_fixed_size(c.handle()));
        } else {
            total += buffer_bytes(t, n);
        }
    }
    return total;
}

namespace {

// Cursor replaying a Spool: in-memory morsels first, then the spilled run.
class SpoolReader : public Cursor {
   public:
    SpoolReader(const std::vector<Morsel>* mem, std::string spill_path)
        : mem_(mem) {
        if (!spill_path.empty()) disk_ = std::make_unique<Reader>(spill_path);
    }
    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        std::optional<Morsel> mem;
        if (try_next(max_rows, mem)) co_return mem;
        if (disk_) co_return co_await disk_->next(max_rows);
        co_return std::nullopt;
    }

    // True only while replaying the in-memory prefix, which shares buffers and
    // never suspends. Once that is drained the run continues on disk, where
    // next() must do the awaiting.
    bool try_next(std::int64_t, std::optional<Morsel>& out) override {
        if (pos_ >= mem_->size()) return false;
        const Morsel& m = (*mem_)[pos_++];
        Morsel o;
        o.rows = m.rows;
        o.columns.reserve(m.columns.size());
        for (const Series& c : m.columns) o.columns.push_back(c.share());
        out = std::move(o);
        return true;
    }

   private:
    const std::vector<Morsel>* mem_;
    std::size_t pos_ = 0;
    std::unique_ptr<Reader> disk_;
};

}  // namespace

namespace {

// The `count` values of `child` from `base`: `child` itself when they are
// all of it, else a gather (slice covers fixed-width types only).
Series list_values(Series child, std::int64_t base, std::int64_t count) {
    if (base == 0 && count == child.length()) return child;
    std::vector<std::int64_t> rows(static_cast<std::size_t>(count));
    for (std::int64_t i = 0; i < count; ++i)
        rows[static_cast<std::size_t>(i)] = base + i;
    Series out = child.take(rows);
    if (!out.valid())
        throw std::invalid_argument(std::string("spill: cannot gather a ") +
                                    std::string(type_name(child.type())) +
                                    " list child");
    return out;
}

void put_series_trailer(std::string& out, const Series& s) {
    const dftu_series& h = *s.handle();
    put_pod<std::uint8_t>(out, h.json() ? 1 : 0);
    put_pod<std::int32_t>(out, static_cast<std::int32_t>(h.time_unit()));
    put_pod<std::uint32_t>(out,
                           static_cast<std::uint32_t>(h.timezone().size()));
    put_bytes(out, h.timezone().data(), h.timezone().size());
}

}  // namespace

void put_series(std::string& out, const Series& s_in) {
    Series s =
        s_in.encoding() == Encoding::Flat ? s_in.share() : s_in.materialize();
    const TypeId t = s.type();
    const std::int64_t n = s.length();
    const std::int64_t nulls = s.null_count();
    put_pod<std::int32_t>(out, static_cast<std::int32_t>(t));
    put_pod<std::int64_t>(out, n);
    put_pod<std::int64_t>(out, nulls);
    if (nulls > 0) {
        std::vector<std::uint8_t> v = validity_bitmap(s);
        put_bytes(out, v.data(), v.size());
    }
    if (t == TypeId::FixedSizeList) {
        const std::int32_t width = s.handle()->fixed_size();
        put_pod<std::int32_t>(out, width);
        put_series(out, list_values(s.child(0), 0, n * width));
        put_series_trailer(out, s);
        return;
    }
    if (t == TypeId::List || t == TypeId::LargeList || t == TypeId::Map) {
        const bool wide = t == TypeId::LargeList;
        std::vector<std::int64_t> offs(static_cast<std::size_t>(n + 1));
        for (std::int64_t i = 0; i <= n; ++i)
            offs[static_cast<std::size_t>(i)] =
                wide ? s.offsets64()[i] : s.offsets()[i];
        const std::int64_t base = offs.front();
        for (auto& o : offs) o -= base;
        for (const std::int64_t o : offs)
            if (wide)
                put_pod<std::int64_t>(out, o);
            else
                put_pod<std::int32_t>(out, static_cast<std::int32_t>(o));
        put_series(out, list_values(s.child(0), base, offs.back()));
        put_series_trailer(out, s);
        return;
    }
    if (t == TypeId::Struct) {
        const std::int64_t fields = s.num_children();
        put_pod<std::int32_t>(out, static_cast<std::int32_t>(fields));
        for (std::int64_t i = 0; i < fields; ++i) {
            const std::string name = s.field_name(i);
            put_pod<std::uint32_t>(out,
                                   static_cast<std::uint32_t>(name.size()));
            put_bytes(out, name.data(), name.size());
            put_series(out, s.child(i));
        }
        put_series_trailer(out, s);
        return;
    }
    if (t == TypeId::Decimal128 || t == TypeId::Decimal256) {
        // read_f64 divides by 10^decimal_scale, so it must round-trip too.
        const DataType dt = s.data_type();
        put_pod<std::int32_t>(out, dt.decimal_precision());
        put_pod<std::int32_t>(out, dt.decimal_scale());
        put_bytes(out, dftu_series_data(s.handle()), buffer_bytes(t, n));
    } else if (t == TypeId::FixedSizeBinary) {
        const std::int32_t width = dftu_series_fixed_size(s.handle());
        put_pod<std::int32_t>(out, width);
        put_bytes(
            out, dftu_series_data(s.handle()),
            static_cast<std::size_t>(n) * static_cast<std::size_t>(width));
    } else if (is_wide_offset_type(t)) {  // LargeString / LargeBinary
        const std::int64_t* offs = s.offsets64();
        const std::int64_t nbytes = n > 0 ? offs[n] : 0;
        put_pod<std::int64_t>(out, nbytes);
        put_bytes(out, offs,
                  static_cast<std::size_t>(n + 1) * sizeof(std::int64_t));
        put_bytes(out, dftu_series_data(s.handle()),
                  static_cast<std::size_t>(nbytes));
    } else if (!byte_width(t)) {  // String / Binary
        const std::int32_t* offs = dftu_series_offsets(s.handle());
        const std::int64_t nbytes = n > 0 ? offs[n] : 0;
        put_pod<std::int64_t>(out, nbytes);
        put_bytes(out, offs,
                  static_cast<std::size_t>(n + 1) * sizeof(std::int32_t));
        put_bytes(out, dftu_series_data(s.handle()),
                  static_cast<std::size_t>(nbytes));
    } else {
        const std::size_t bytes = buffer_bytes(t, n);
        put_bytes(out, dftu_series_data(s.handle()), bytes);
    }
    put_series_trailer(out, s);
}

namespace {

Series get_series_data(const std::uint8_t*& p, const std::uint8_t* end);

}  // namespace

Series get_series(const std::uint8_t*& p, const std::uint8_t* end) {
    Series s = get_series_data(p, end);
    const bool json = get_pod<std::uint8_t>(p, end) != 0;
    const auto unit = static_cast<TimeUnit>(get_pod<std::int32_t>(p, end));
    const auto zone_bytes = get_pod<std::uint32_t>(p, end);
    const auto* zone = take_bytes(p, end, zone_bytes);
    dftu_series* h = s.handle();
    h->set_json(json);
    h->set_time_unit(unit);
    h->set_timezone(
        std::string_view(reinterpret_cast<const char*>(zone), zone_bytes));
    return s;
}

namespace {

Series get_series_data(const std::uint8_t*& p, const std::uint8_t* end) {
    const auto type = static_cast<TypeId>(get_pod<std::int32_t>(p, end));
    const std::int64_t n = get_pod<std::int64_t>(p, end);
    const std::int64_t nulls = get_pod<std::int64_t>(p, end);
    const std::uint8_t* validity = nullptr;
    if (nulls > 0)
        validity = take_bytes(p, end, static_cast<std::size_t>((n + 7) / 8));
    const auto dt = static_cast<dftu_dtype>(static_cast<std::int32_t>(type));
    if (type == TypeId::FixedSizeList) {
        const auto width = get_pod<std::int32_t>(p, end);
        Series child = get_series(p, end);
        auto* col = new dftu_series();
        col->type = type;
        col->encoding = Encoding::Flat;
        col->length = n;
        col->set_fixed_size(width);
        col->set_child(std::shared_ptr<dftu_series>(child.release()));
        if (validity != nullptr) {
            const std::size_t vbytes = static_cast<std::size_t>((n + 7) / 8);
            col->validity = Buffer::allocate(vbytes);
            std::memcpy(col->validity->data(), validity, vbytes);
            col->null_count = nulls;
        }
        return Series{col};
    }
    if (type == TypeId::List || type == TypeId::LargeList ||
        type == TypeId::Map) {
        const bool wide = type == TypeId::LargeList;
        const std::size_t width =
            wide ? sizeof(std::int64_t) : sizeof(std::int32_t);
        const std::size_t off_bytes = static_cast<std::size_t>(n + 1) * width;
        const std::uint8_t* offs = take_bytes(p, end, off_bytes);
        Series child = get_series(p, end);
        auto* col = new dftu_series();
        col->type = type;
        col->encoding = Encoding::Flat;
        col->length = n;
        auto buffer = Buffer::allocate(off_bytes);
        if (off_bytes != 0) std::memcpy(buffer->data(), offs, off_bytes);
        col->offsets = std::move(buffer);
        col->set_child(std::shared_ptr<dftu_series>(child.release()));
        if (validity != nullptr) {
            const std::size_t vbytes = static_cast<std::size_t>((n + 7) / 8);
            col->validity = Buffer::allocate(vbytes);
            std::memcpy(col->validity->data(), validity, vbytes);
            col->null_count = nulls;
        }
        return Series{col};
    }
    if (type == TypeId::Struct) {
        const auto fields = get_pod<std::int32_t>(p, end);
        auto* col = new dftu_series();
        col->type = type;
        col->encoding = Encoding::Flat;
        col->length = n;
        for (std::int32_t i = 0; i < fields; ++i) {
            const auto name_bytes = get_pod<std::uint32_t>(p, end);
            const auto* name = take_bytes(p, end, name_bytes);
            std::string field(reinterpret_cast<const char*>(name), name_bytes);
            Series child = get_series(p, end);
            col->nested.push_back(
                {std::shared_ptr<dftu_series>(child.release()),
                 std::move(field)});
        }
        if (validity != nullptr) {
            const std::size_t vbytes = static_cast<std::size_t>((n + 7) / 8);
            col->validity = Buffer::allocate(vbytes);
            std::memcpy(col->validity->data(), validity, vbytes);
            col->null_count = nulls;
        }
        return Series{col};
    }
    if (type == TypeId::Decimal128 || type == TypeId::Decimal256) {
        const std::int32_t precision = get_pod<std::int32_t>(p, end);
        const std::int32_t scale = get_pod<std::int32_t>(p, end);
        const std::size_t bytes = buffer_bytes(type, n);
        const std::uint8_t* data = take_bytes(p, end, bytes);
        auto* col = new dftu_series();
        col->type = type;
        col->encoding = Encoding::Flat;
        col->length = n;
        col->set_decimal(precision, scale);
        col->data = Buffer::allocate(bytes);
        if (bytes != 0) std::memcpy(col->data->data(), data, bytes);
        if (validity != nullptr) {
            const std::size_t vbytes = static_cast<std::size_t>((n + 7) / 8);
            col->validity = Buffer::allocate(vbytes);
            std::memcpy(col->validity->data(), validity, vbytes);
            col->null_count = nulls;
        }
        return Series{col};
    }
    if (type == TypeId::FixedSizeBinary) {
        const std::int32_t width = get_pod<std::int32_t>(p, end);
        const std::size_t bytes =
            static_cast<std::size_t>(n) * static_cast<std::size_t>(width);
        const std::uint8_t* data = take_bytes(p, end, bytes);
        auto* col = new dftu_series();
        col->type = type;
        col->encoding = Encoding::Flat;
        col->length = n;
        col->set_fixed_size(width);
        col->data = Buffer::allocate(bytes);
        if (bytes != 0) std::memcpy(col->data->data(), data, bytes);
        if (validity != nullptr) {
            const std::size_t vbytes = static_cast<std::size_t>((n + 7) / 8);
            col->validity = Buffer::allocate(vbytes);
            std::memcpy(col->validity->data(), validity, vbytes);
            col->null_count = nulls;
        }
        return Series{col};
    }
    if (is_wide_offset_type(type)) {  // LargeString / LargeBinary
        const std::int64_t nbytes = get_pod<std::int64_t>(p, end);
        const auto* offs = reinterpret_cast<const std::int64_t*>(take_bytes(
            p, end, static_cast<std::size_t>(n + 1) * sizeof(std::int64_t)));
        const std::uint8_t* data =
            take_bytes(p, end, static_cast<std::size_t>(nbytes));
        auto* col = new dftu_series();
        col->type = type;
        col->encoding = Encoding::Flat;
        col->length = n;
        col->offsets = Buffer::allocate(static_cast<std::size_t>(n + 1) *
                                        sizeof(std::int64_t));
        std::memcpy(col->offsets->data(), offs,
                    static_cast<std::size_t>(n + 1) * sizeof(std::int64_t));
        col->data = Buffer::allocate(static_cast<std::size_t>(nbytes));
        if (nbytes != 0)
            std::memcpy(col->data->data(), data,
                        static_cast<std::size_t>(nbytes));
        if (validity != nullptr) {
            const std::size_t vbytes = static_cast<std::size_t>((n + 7) / 8);
            col->validity = Buffer::allocate(vbytes);
            std::memcpy(col->validity->data(), validity, vbytes);
            col->null_count = nulls;
        }
        return Series{col};
    }
    if (!byte_width(type)) {  // String / Binary
        const std::int64_t nbytes = get_pod<std::int64_t>(p, end);
        const auto* offs = reinterpret_cast<const std::int32_t*>(take_bytes(
            p, end, static_cast<std::size_t>(n + 1) * sizeof(std::int32_t)));
        const std::uint8_t* data =
            take_bytes(p, end, static_cast<std::size_t>(nbytes));
        return Series{dftu_series_new_string(dt, offs, data, n, validity)};
    }
    const std::size_t bytes = buffer_bytes(type, n);
    const std::uint8_t* data = take_bytes(p, end, bytes);
    return Series{dftu_series_new_flat(dt, data, n, validity)};
}

}  // namespace

std::string spill_dir() {
    const auto env = Env::get(constants::SPILL_DIR_ENV);
    std::error_code ec;
    fs::path dir = env && !env->empty() ? fs::path(std::string(*env))
                                        : fs::temp_directory_path(ec);
    if (!ec) fs::create_directories(dir, ec);
    if (!ec && ::access(dir.c_str(), W_OK | X_OK) != 0)
        ec = std::make_error_code(std::errc::permission_denied);
    if (ec)
        throw std::runtime_error("spill: cannot use spill directory '" +
                                 dir.string() + "' (" + ec.message() +
                                 "); set " + constants::SPILL_DIR_ENV +
                                 " to a writable directory");
    return dir.string();
}

namespace {

constexpr std::size_t REGION_ALIGN = 64;

bool is_nested(TypeId t) {
    return t == TypeId::Struct || t == TypeId::List || t == TypeId::LargeList ||
           t == TypeId::FixedSizeList || t == TypeId::Map;
}

struct Piece {
    const void* src = nullptr;
    std::size_t bytes = 0;
    std::size_t at = 0;
    std::vector<std::uint8_t> owned;
};

struct ColumnPlan {
    Series flat;
    Piece validity, offsets, data;
};

void write_all(int fd, const void* src, std::size_t bytes, std::uint64_t at,
               const std::string& dir) {
    const auto* p = static_cast<const std::uint8_t*>(src);
    while (bytes > 0) {
        const ssize_t w = ::pwrite(fd, p, bytes, static_cast<off_t>(at));
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0)
            throw std::runtime_error(
                "spill: cannot write to the spill file in '" + dir + "' (" +
                std::strerror(errno) + "); set " + constants::SPILL_DIR_ENV +
                " to a directory with free space");
        p += w;
        at += static_cast<std::uint64_t>(w);
        bytes -= static_cast<std::size_t>(w);
    }
}

}  // namespace

PartFile::PartFile() : dir_(spill_dir()) {
    std::string path = (fs::path(dir_) / "dftu_collect_XXXXXX").string();
    fd_ = ScopedFd(::mkstemp(path.data()));
    if (fd_.get() < 0)
        throw std::runtime_error("spill: cannot create a spill file in '" +
                                 dir_ + "' (" + std::strerror(errno) +
                                 "); set " + constants::SPILL_DIR_ENV +
                                 " to a writable directory");
    // Unlinked at once so a crash or an early return never leaves the file;
    // the descriptor and the mappings keep the data.
    ::unlink(path.c_str());
}

PartFile::~PartFile() = default;

std::vector<Series> PartFile::append(const std::vector<Series>& cols) {
    std::vector<ColumnPlan> plans(cols.size());
    std::size_t at = 0;
    const auto place = [&at](Piece& p, const void* src, std::size_t bytes) {
        p.src = src;
        p.bytes = bytes;
        p.at = at;
        at = bits::align_up(at + bytes, REGION_ALIGN);
    };
    for (std::size_t i = 0; i < cols.size(); ++i) {
        const Series& c = cols[i];
        ColumnPlan& cp = plans[i];
        if (is_nested(c.type()) || c.length() == 0) continue;
        cp.flat = c.encoding() == Encoding::Flat ? c.share() : c.materialize();
        const Series& s = cp.flat;
        const TypeId t = s.type();
        const std::int64_t n = s.length();
        if (s.null_count() > 0) {
            cp.validity.owned = validity_bitmap(s);
            place(cp.validity, cp.validity.owned.data(),
                  cp.validity.owned.size());
        }
        std::size_t nbytes;
        if (is_wide_offset_type(t)) {
            const std::int64_t* offs = s.offsets64();
            place(cp.offsets, offs,
                  static_cast<std::size_t>(n + 1) * sizeof(std::int64_t));
            nbytes = static_cast<std::size_t>(offs[n]);
        } else if (!byte_width(t) && t != TypeId::FixedSizeBinary &&
                   t != TypeId::Decimal128 && t != TypeId::Decimal256) {
            const std::int32_t* offs = dftu_series_offsets(s.handle());
            place(cp.offsets, offs,
                  static_cast<std::size_t>(n + 1) * sizeof(std::int32_t));
            nbytes = static_cast<std::size_t>(offs[n]);
        } else if (t == TypeId::FixedSizeBinary) {
            nbytes =
                static_cast<std::size_t>(n) *
                static_cast<std::size_t>(dftu_series_fixed_size(s.handle()));
        } else {
            nbytes = buffer_bytes(t, n);
        }
        place(cp.data, dftu_series_data(s.handle()), nbytes);
    }

    const long page_size = ::sysconf(_SC_PAGESIZE);
    const std::size_t page =
        page_size > 0 ? static_cast<std::size_t>(page_size) : 4096;
    const std::size_t region_bytes = bits::align_up(at, page);
    std::vector<Series> out;
    out.reserve(cols.size());
    if (region_bytes == 0) {
        for (const Series& c : cols) out.push_back(c.share());
        return out;
    }
    const std::uint64_t base = end_;
    for (const ColumnPlan& cp : plans)
        for (const Piece* p : {&cp.validity, &cp.offsets, &cp.data})
            if (p->bytes != 0)
                write_all(fd_.get(), p->src, p->bytes, base + p->at, dir_);
    // The last bytes may be padding: extend so the mapped pages exist.
    if (::ftruncate(fd_.get(), static_cast<off_t>(base + region_bytes)) != 0)
        throw std::runtime_error("spill: cannot extend the spill file in '" +
                                 dir_ + "' (" + std::strerror(errno) +
                                 "); set " + constants::SPILL_DIR_ENV +
                                 " to a directory with free space");
    void* map = ::mmap(nullptr, region_bytes, PROT_READ, MAP_SHARED, fd_.get(),
                       static_cast<off_t>(base));
    if (map == MAP_FAILED)
        throw std::runtime_error("spill: cannot map the spill file in '" +
                                 dir_ + "' (" + std::strerror(errno) +
                                 "); set " + constants::SPILL_DIR_ENV +
                                 " to a usable directory");
    end_ = base + region_bytes;
    const std::shared_ptr<void> region(
        map, [region_bytes](void* m) { ::munmap(m, region_bytes); });
    const auto window = [&](const Piece& p) -> std::shared_ptr<Buffer> {
        if (p.bytes == 0) return Buffer::allocate(0);
        return Buffer::wrap(static_cast<std::uint8_t*>(map) + p.at, p.bytes,
                            [region](void*) {});
    };
    for (std::size_t i = 0; i < cols.size(); ++i) {
        const ColumnPlan& cp = plans[i];
        if (!cp.flat.valid()) {
            out.push_back(cols[i].share());
            continue;
        }
        const dftu_series& h = *cp.flat.handle();
        auto* col = new dftu_series();
        col->type = h.type;
        col->encoding = Encoding::Flat;
        col->length = h.length;
        col->null_count = h.null_count;
        col->params = h.params;
        col->data = window(cp.data);
        if (cp.offsets.bytes != 0) col->offsets = window(cp.offsets);
        if (cp.validity.bytes != 0) col->validity = window(cp.validity);
        out.emplace_back(col);
    }
    return out;
}

Dir::Dir() {
    fs::path base =
        fs::path(spill_dir()) / ("dftu_lazy_" + std::to_string(::getpid()) +
                                 "_" + std::to_string(next_seq()));
    std::error_code ec;
    fs::create_directories(base, ec);
    if (ec)
        throw std::runtime_error(
            "spill: cannot create '" + base.string() + "' (" + ec.message() +
            "); set " + constants::SPILL_DIR_ENV + " to a writable directory");
    dir_ = base.string();
}

Dir::~Dir() {
    std::error_code ec;
    fs::remove_all(dir_, ec);
}

int Dir::next_run() { return runs_.fetch_add(1); }

std::string Dir::run_path(int id) const {
    return (fs::path(dir_) / ("run_" + std::to_string(id) + ".bin")).string();
}

Writer::Writer(const std::string& path)
    : os_(path, std::ios::binary | std::ios::trunc) {
    if (!os_) throw std::runtime_error("spill: cannot open run file " + path);
}

void Writer::write(const std::vector<Series>& cols, std::int64_t rows) {
    std::string blob;
    put_pod<std::int64_t>(blob, rows);
    put_pod<std::int32_t>(blob, static_cast<std::int32_t>(cols.size()));
    for (const Series& c : cols) put_series(blob, c);
    const std::int64_t len = static_cast<std::int64_t>(blob.size());
    os_.write(reinterpret_cast<const char*>(&len), sizeof(len));
    os_.write(blob.data(), static_cast<std::streamsize>(blob.size()));
}

void Writer::close() { os_.close(); }

Reader::Reader(const std::string& path) : is_(path, std::ios::binary) {
    if (!is_) throw std::runtime_error("spill: cannot open run file " + path);
}

Spool::Spool(std::uint64_t budget, std::shared_ptr<Dir> dir)
    : budget_(budget), dir_(std::move(dir)) {}

Spool::~Spool() {
    writer_.reset();
    if (!run_.empty()) {
        std::error_code ec;
        fs::remove(run_, ec);
    }
}

void Spool::add(std::vector<Series> columns, std::int64_t rows) {
    if (!spilling_) {
        const std::size_t bytes = columns_bytes(columns);
        bytes_ += bytes;
        total_bytes_ += bytes;
        Morsel m;
        m.rows = rows;
        m.columns = std::move(columns);
        mem_.push_back(std::move(m));
        if (bytes_ > budget_) {  // overflow: subsequent morsels go to disk
            if (!dir_) dir_ = std::make_shared<Dir>();
            run_ = dir_->run_path(dir_->next_run());
            writer_ = std::make_unique<Writer>(run_);
            spilling_ = true;
        }
        return;
    }
    total_bytes_ += columns_bytes(columns);
    writer_->write(columns, rows);
}

std::unique_ptr<Cursor> Spool::reader() {
    if (writer_) writer_->close();
    return std::make_unique<SpoolReader>(&mem_, run_);
}

coro::CoroTask<std::optional<Morsel>> Reader::next(std::int64_t /*max_rows*/) {
    std::int64_t len = 0;
    is_.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (!is_ || is_.gcount() == 0) co_return std::nullopt;
    std::string blob(static_cast<std::size_t>(len), '\0');
    is_.read(blob.data(), static_cast<std::streamsize>(len));
    if (is_.gcount() != len) throw std::runtime_error("spill: short run read");
    const auto* p = reinterpret_cast<const std::uint8_t*>(blob.data());
    const std::uint8_t* pend = p + blob.size();
    Morsel m;
    m.rows = get_pod<std::int64_t>(p, pend);
    const std::int32_t ncols = get_pod<std::int32_t>(p, pend);
    m.columns.reserve(static_cast<std::size_t>(ncols));
    for (std::int32_t c = 0; c < ncols; ++c)
        m.columns.push_back(get_series(p, pend));
    co_return m;
}

void write_agg_run(AggState& state, const std::string& path) {
    agg_sort_groups(state);
    std::ofstream os(path, std::ios::binary);
    if (!os) throw std::runtime_error("agg spill: cannot open run " + path);
    const std::int64_t ng = agg_num_groups(state);
    for (std::int64_t g = 0; g < ng; ++g) {
        const std::string blob = agg_serialize(*agg_extract_group(state, g));
        const std::uint32_t len = static_cast<std::uint32_t>(blob.size());
        os.write(reinterpret_cast<const char*>(&len), sizeof(len));
        os.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    }
}

AggRunReader::AggRunReader(const std::string& path)
    : is_(path, std::ios::binary) {
    advance();
}

void AggRunReader::advance() {
    std::uint32_t len = 0;
    if (!is_.read(reinterpret_cast<char*>(&len), sizeof(len))) {
        valid_ = false;
        return;
    }
    std::string blob(len, '\0');
    is_.read(blob.data(), static_cast<std::streamsize>(len));
    cur_ = agg_deserialize(blob);
    valid_ = true;
}

}  // namespace dftracer::utils::dataframe::spill
