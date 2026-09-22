#include <dftracer/utils/core/common/filesystem.h>  // fs:: portability alias
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/spill.h>
#include <dftracer/utils/dataframe/types.h>
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

// Approximate in-memory bytes of a set of FLAT columns (spool spill trigger).
std::size_t columns_bytes(const std::vector<Series>& cols) {
    std::size_t total = 0;
    for (const Series& c : cols) {
        const std::int64_t n = c.length();
        const TypeId t = c.type();
        if (is_wide_offset_type(t)) {  // LargeString / LargeBinary
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
    // FixedSizeList and Map are nested (Map is physically a List of
    // key/value structs) like List/LargeList/Struct; all five must refuse
    // here rather than fall into the FixedSizeBinary/String byte paths
    // below, which would misread their null offsets/child data.
    if (t == TypeId::List || t == TypeId::LargeList || t == TypeId::Struct ||
        t == TypeId::FixedSizeList || t == TypeId::Map)
        throw std::invalid_argument("spill: nested columns unsupported");
    if (t == TypeId::Decimal128 || t == TypeId::Decimal256) {
        // read_f64 divides by 10^decimal_scale, so it must round-trip too.
        const DataType dt = s.data_type();
        put_pod<std::int32_t>(out, dt.decimal_precision);
        put_pod<std::int32_t>(out, dt.decimal_scale);
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
}

Series get_series(const std::uint8_t*& p, const std::uint8_t* end) {
    const auto type = static_cast<TypeId>(get_pod<std::int32_t>(p, end));
    const std::int64_t n = get_pod<std::int64_t>(p, end);
    const std::int64_t nulls = get_pod<std::int64_t>(p, end);
    const std::uint8_t* validity = nullptr;
    if (nulls > 0)
        validity = take_bytes(p, end, static_cast<std::size_t>((n + 7) / 8));
    const auto dt = static_cast<dftu_dtype>(static_cast<std::int32_t>(type));
    if (type == TypeId::Decimal128 || type == TypeId::Decimal256) {
        const std::int32_t precision = get_pod<std::int32_t>(p, end);
        const std::int32_t scale = get_pod<std::int32_t>(p, end);
        const std::size_t bytes = buffer_bytes(type, n);
        const std::uint8_t* data = take_bytes(p, end, bytes);
        auto* col = new dftu_series();
        col->type = type;
        col->encoding = Encoding::Flat;
        col->length = n;
        col->decimal_precision = precision;
        col->decimal_scale = scale;
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
        col->fixed_size = width;
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
        col->offsets64 = Buffer::allocate(static_cast<std::size_t>(n + 1) *
                                          sizeof(std::int64_t));
        std::memcpy(col->offsets64->data(), offs,
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

Dir::Dir() {
    fs::path base =
        fs::temp_directory_path() / ("dftu_lazy_" + std::to_string(::getpid()) +
                                     "_" + std::to_string(next_seq()));
    std::error_code ec;
    fs::create_directories(base, ec);
    dir_ = base.string();
}

Dir::~Dir() {
    std::error_code ec;
    fs::remove_all(dir_, ec);
}

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

Spool::Spool(std::uint64_t budget) : budget_(budget) {}
Spool::~Spool() = default;

void Spool::add(std::vector<Series> columns, std::int64_t rows) {
    if (!spilling_) {
        bytes_ += columns_bytes(columns);
        Morsel m;
        m.rows = rows;
        m.columns = std::move(columns);
        mem_.push_back(std::move(m));
        if (bytes_ > budget_) {  // overflow: subsequent morsels go to disk
            dir_ = std::make_unique<Dir>();
            writer_ = std::make_unique<Writer>(dir_->run_path(0));
            spilling_ = true;
        }
        return;
    }
    writer_->write(columns, rows);
}

std::unique_ptr<Cursor> Spool::reader() {
    if (writer_) writer_->close();
    return std::make_unique<SpoolReader>(
        &mem_, dir_ ? dir_->run_path(0) : std::string());
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
