// SPDX-License-Identifier: MIT
// The register calls and their arguments follow liburing,
// https://github.com/axboe/liburing (MIT), Copyright 2020 Jens Axboe, files
// src/register.c and src/include/liburing/io_uring.h.
#ifndef DFTRACER_UTILS_RUNTIME_IO_IO_URING_REGISTER_H
#define DFTRACER_UTILS_RUNTIME_IO_IO_URING_REGISTER_H
#ifdef DFTRACER_UTILS_HAVE_IO_URING

#include <dftracer/utils/runtime/io/io_uring_wrapper.h>

#include <cstddef>
#include <cstdint>

#ifndef IORING_ENTER_GETEVENTS
#define IORING_ENTER_GETEVENTS (1U << 0)
#endif
#ifndef IORING_ENTER_ABS_TIMER
#define IORING_ENTER_ABS_TIMER (1U << 5)
#endif
#ifndef IORING_ENTER_EXT_ARG_REG
#define IORING_ENTER_EXT_ARG_REG (1U << 6)
#endif
#ifndef IORING_FEAT_REG_REG_RING
#define IORING_FEAT_REG_REG_RING (1U << 13)
#endif
#ifndef IORING_RSRC_REGISTER_SPARSE
#define IORING_RSRC_REGISTER_SPARSE (1U << 0)
#endif
#ifndef IORING_REGISTER_FILES_SKIP
#define IORING_REGISTER_FILES_SKIP (-2)
#endif

namespace dftracer::utils::runtime::io::uring {

// The numbers of enum io_uring_register_op.
constexpr unsigned REG_BUFFERS = 0;
constexpr unsigned REG_UNREGISTER_BUFFERS = 1;
constexpr unsigned REG_FILES = 2;
constexpr unsigned REG_UNREGISTER_FILES = 3;
constexpr unsigned REG_EVENTFD = 4;
constexpr unsigned REG_UNREGISTER_EVENTFD = 5;
constexpr unsigned REG_FILES_UPDATE = 6;
constexpr unsigned REG_EVENTFD_ASYNC = 7;
constexpr unsigned REG_PROBE = 8;
constexpr unsigned REG_PERSONALITY = 9;
constexpr unsigned REG_UNREGISTER_PERSONALITY = 10;
constexpr unsigned REG_RESTRICTIONS = 11;
constexpr unsigned REG_ENABLE_RINGS = 12;
constexpr unsigned REG_FILES2 = 13;
constexpr unsigned REG_FILES_UPDATE2 = 14;
constexpr unsigned REG_BUFFERS2 = 15;
constexpr unsigned REG_BUFFERS_UPDATE = 16;
constexpr unsigned REG_IOWQ_AFF = 17;
constexpr unsigned REG_UNREGISTER_IOWQ_AFF = 18;
constexpr unsigned REG_IOWQ_MAX_WORKERS = 19;
constexpr unsigned REG_RING_FDS = 20;
constexpr unsigned REG_UNREGISTER_RING_FDS = 21;
constexpr unsigned REG_PBUF_RING = 22;
constexpr unsigned REG_UNREGISTER_PBUF_RING = 23;
constexpr unsigned REG_SYNC_CANCEL = 24;
constexpr unsigned REG_FILE_ALLOC_RANGE = 25;
constexpr unsigned REG_PBUF_STATUS = 26;
constexpr unsigned REG_NAPI = 27;
constexpr unsigned REG_UNREGISTER_NAPI = 28;
constexpr unsigned REG_CLOCK = 29;
constexpr unsigned REG_CLONE_BUFFERS = 30;
constexpr unsigned REG_SEND_MSG_RING = 31;
constexpr unsigned REG_ZCRX_IFQ = 32;
constexpr unsigned REG_RESIZE_RINGS = 33;
constexpr unsigned REG_MEM_REGION = 34;
constexpr unsigned REG_QUERY = 35;
constexpr unsigned REG_ZCRX_CTRL = 36;
constexpr unsigned REG_BPF_FILTER = 37;

// Flags and values of the arguments.
constexpr unsigned RESTRICTION_REGISTER_OP = 0;
constexpr unsigned RESTRICTION_SQE_OP = 1;
constexpr unsigned RESTRICTION_SQE_FLAGS_ALLOWED = 2;
constexpr unsigned RESTRICTION_SQE_FLAGS_REQUIRED = 3;
constexpr unsigned CLONE_SRC_REGISTERED = 1U << 0;
constexpr unsigned CLONE_DST_REPLACE = 1U << 1;
constexpr std::uint8_t NAPI_OP_REGISTER = 0;
constexpr std::uint8_t NAPI_OP_STATIC_ADD_ID = 1;
constexpr std::uint8_t NAPI_OP_STATIC_DEL_ID = 2;
constexpr std::uint32_t NAPI_TRACKING_DYNAMIC = 0;
constexpr std::uint32_t NAPI_TRACKING_STATIC = 1;
constexpr std::uint32_t NAPI_TRACKING_INACTIVE = 255;
constexpr unsigned PBUF_RING_MMAP = 1;
constexpr unsigned PBUF_RING_INC = 2;
// The mmap offset of a ring that the kernel allocated: OFFSET | (bgid <<
// SHIFT).
constexpr std::uint64_t PBUF_RING_OFFSET = 0x80000000ULL;
constexpr unsigned PBUF_RING_OFFSET_SHIFT = 16;
constexpr std::uint32_t REG_WAIT_TS = 1U << 0;
constexpr std::uint64_t MEM_REGION_TYPE_USER = 1;
constexpr std::uint64_t MEM_REGION_REG_WAIT_ARG = 1;
constexpr unsigned ZCRX_REG_IMPORT = 1;
constexpr unsigned ZCRX_REG_NODEV = 2;
constexpr std::uint32_t ZCRX_CTRL_FLUSH_RQ = 0;
constexpr std::uint32_t ZCRX_CTRL_EXPORT = 1;
constexpr std::uint32_t QUERY_OPCODES = 0;
constexpr std::uint32_t QUERY_ZCRX = 1;
constexpr std::uint32_t QUERY_SCQ = 2;
constexpr std::uint16_t URING_BPF_CMD_FILTER = 1;
constexpr std::uint32_t URING_BPF_FILTER_DENY_REST = 1U << 0;
constexpr std::uint32_t URING_BPF_FILTER_SZ_STRICT = 1U << 1;

// The kernel's argument structs. Each static_assert is the size the kernel
// copies.

/// struct io_uring_rsrc_register.
struct RsrcRegister {
    std::uint32_t nr = 0;
    std::uint32_t flags = 0;
    std::uint64_t resv2 = 0;
    std::uint64_t data = 0;
    std::uint64_t tags = 0;
};
static_assert(sizeof(RsrcRegister) == 32);

/// struct io_uring_rsrc_update2.
struct RsrcUpdate2 {
    std::uint32_t offset = 0;
    std::uint32_t resv = 0;
    std::uint64_t data = 0;
    std::uint64_t tags = 0;
    std::uint32_t nr = 0;
    std::uint32_t resv2 = 0;
};
static_assert(sizeof(RsrcUpdate2) == 32);

/// struct io_uring_restriction. `value` is the register op, the SQE op or the
/// SQE flags, as `opcode` says.
struct Restriction {
    std::uint16_t opcode = 0;
    std::uint8_t value = 0;
    std::uint8_t resv = 0;
    std::uint32_t resv2[3] = {0, 0, 0};
};
static_assert(sizeof(Restriction) == 16);

/// struct io_uring_clock_register.
struct ClockRegister {
    std::uint32_t clockid = 0;
    std::uint32_t resv[3] = {0, 0, 0};
};
static_assert(sizeof(ClockRegister) == 16);

/// struct io_uring_clone_buffers.
struct CloneBuffers {
    std::uint32_t src_fd = 0;
    std::uint32_t flags = 0;
    std::uint32_t src_off = 0;
    std::uint32_t dst_off = 0;
    std::uint32_t nr = 0;
    std::uint32_t pad[3] = {0, 0, 0};
};
static_assert(sizeof(CloneBuffers) == 32);

/// struct io_uring_buf, one entry of a provided buffer ring.
struct ProvidedBuf {
    std::uint64_t addr;
    std::uint32_t len;
    std::uint16_t bid;
    std::uint16_t resv;
};
static_assert(sizeof(ProvidedBuf) == 16);

/// struct io_uring_buf_reg.
struct BufReg {
    std::uint64_t ring_addr = 0;
    std::uint32_t ring_entries = 0;
    std::uint16_t bgid = 0;
    std::uint16_t flags = 0;
    std::uint32_t min_left = 0;
    std::uint32_t resv[5] = {0, 0, 0, 0, 0};
};
static_assert(sizeof(BufReg) == 40);

/// struct io_uring_buf_status.
struct BufStatus {
    std::uint32_t buf_group = 0;
    std::uint32_t head = 0;
    std::uint32_t resv[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};
static_assert(sizeof(BufStatus) == 40);

/// struct io_uring_napi. `opcode` is NAPI_OP_*. For NAPI_OP_REGISTER,
/// `op_param` is a NAPI_TRACKING_* value; for the static id ops it is the napi
/// id.
struct Napi {
    std::uint32_t busy_poll_to = 0;
    std::uint8_t prefer_busy_poll = 0;
    std::uint8_t opcode = 0;
    std::uint8_t pad[2] = {0, 0};
    std::uint32_t op_param = 0;
    std::uint32_t resv = 0;
};
static_assert(sizeof(Napi) == 16);

/// struct io_uring_file_index_range.
struct FileIndexRange {
    std::uint32_t off = 0;
    std::uint32_t len = 0;
    std::uint64_t resv = 0;
};
static_assert(sizeof(FileIndexRange) == 16);

/// struct io_uring_sync_cancel_reg. A `timeout` of -1 seconds and -1
/// nanoseconds waits without a limit.
struct SyncCancelReg {
    std::uint64_t addr = 0;
    std::int32_t fd = 0;
    std::uint32_t flags = 0;
    KernelTimespec timeout = {0, 0};
    std::uint8_t opcode = 0;
    std::uint8_t pad[7] = {0, 0, 0, 0, 0, 0, 0};
    std::uint64_t pad2[3] = {0, 0, 0};
};
static_assert(sizeof(SyncCancelReg) == 64);

/// struct io_uring_region_desc.
struct RegionDesc {
    std::uint64_t user_addr = 0;
    std::uint64_t size = 0;
    std::uint32_t flags = 0;
    std::uint32_t id = 0;
    std::uint64_t mmap_offset = 0;
    std::uint64_t resv[4] = {0, 0, 0, 0};
};
static_assert(sizeof(RegionDesc) == 64);

/// struct io_uring_mem_region_reg. `region_uptr` points to a @ref RegionDesc.
struct MemRegionReg {
    std::uint64_t region_uptr = 0;
    std::uint64_t flags = 0;
    std::uint64_t resv[2] = {0, 0};
};
static_assert(sizeof(MemRegionReg) == 32);

/// struct io_uring_reg_wait, one entry of a region registered with
/// MEM_REGION_REG_WAIT_ARG.
struct RegWait {
    KernelTimespec ts = {0, 0};
    std::uint32_t min_wait_usec = 0;
    std::uint32_t flags = 0;
    std::uint64_t sigmask = 0;
    std::uint32_t sigmask_sz = 0;
    std::uint32_t pad[3] = {0, 0, 0};
    std::uint64_t pad2[2] = {0, 0};
};
static_assert(sizeof(RegWait) == 64);

/// struct io_uring_zcrx_offsets.
struct ZcrxOffsets {
    std::uint32_t head = 0;
    std::uint32_t tail = 0;
    std::uint32_t rqes = 0;
    std::uint32_t resv2 = 0;
    std::uint64_t resv[2] = {0, 0};
};
static_assert(sizeof(ZcrxOffsets) == 32);

/// struct io_uring_zcrx_area_reg.
struct ZcrxAreaReg {
    std::uint64_t addr = 0;
    std::uint64_t len = 0;
    std::uint64_t rq_area_token = 0;
    std::uint32_t flags = 0;
    std::uint32_t dmabuf_fd = 0;
    std::uint64_t resv2[2] = {0, 0};
};
static_assert(sizeof(ZcrxAreaReg) == 48);

/// struct io_uring_zcrx_ifq_reg. `area_ptr` points to a @ref ZcrxAreaReg and
/// `region_ptr` to a @ref RegionDesc.
struct ZcrxIfqReg {
    std::uint32_t if_idx = 0;
    std::uint32_t if_rxq = 0;
    std::uint32_t rq_entries = 0;
    std::uint32_t flags = 0;
    std::uint64_t area_ptr = 0;
    std::uint64_t region_ptr = 0;
    ZcrxOffsets offsets;
    std::uint32_t zcrx_id = 0;
    std::uint32_t rx_buf_len = 0;
    std::uint64_t resv[3] = {0, 0, 0};
};
static_assert(sizeof(ZcrxIfqReg) == 96);

struct ZcrxCtrlFlushRq {
    std::uint64_t resv[6] = {0, 0, 0, 0, 0, 0};
};

struct ZcrxCtrlExport {
    std::uint32_t zcrx_fd = 0;
    std::uint32_t resv1[11] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
};

/// struct zcrx_ctrl. `op` is ZCRX_CTRL_*.
struct ZcrxCtrl {
    std::uint32_t zcrx_id = 0;
    std::uint32_t op = 0;
    std::uint64_t resv[2] = {0, 0};
    union {
        ZcrxCtrlExport zc_export;
        ZcrxCtrlFlushRq zc_flush;
    };

    ZcrxCtrl() : zc_flush() {}
};
static_assert(sizeof(ZcrxCtrl) == 72);

// The query and BPF filter structs come from the kernel's query.h and
// bpf_filter.h in include/uapi/linux/io_uring/.

/// struct io_uring_query_hdr. Entries chain through `next_entry`; `query_data`
/// points to the struct that `query_op` names.
struct QueryHdr {
    std::uint64_t next_entry = 0;
    std::uint64_t query_data = 0;
    std::uint32_t query_op = 0;
    std::uint32_t size = 0;
    std::int32_t result = 0;
    std::uint32_t resv[3] = {0, 0, 0};
};
static_assert(sizeof(QueryHdr) == 40);

/// struct io_uring_query_opcode, for QUERY_OPCODES.
struct QueryOpcode {
    std::uint32_t nr_request_opcodes = 0;
    std::uint32_t nr_register_opcodes = 0;
    std::uint64_t feature_flags = 0;
    std::uint64_t ring_setup_flags = 0;
    std::uint64_t enter_flags = 0;
    std::uint64_t sqe_flags = 0;
    std::uint32_t nr_query_opcodes = 0;
    std::uint32_t pad = 0;
};
static_assert(sizeof(QueryOpcode) == 48);

/// struct io_uring_query_zcrx, for QUERY_ZCRX.
struct QueryZcrx {
    std::uint64_t register_flags = 0;
    std::uint64_t area_flags = 0;
    std::uint32_t nr_ctrl_opcodes = 0;
    std::uint32_t features = 0;
    std::uint32_t rq_hdr_size = 0;
    std::uint32_t rq_hdr_alignment = 0;
    std::uint64_t resv2 = 0;
};
static_assert(sizeof(QueryZcrx) == 40);

/// struct io_uring_query_scq, for QUERY_SCQ.
struct QueryScq {
    std::uint64_t hdr_size = 0;
    std::uint64_t hdr_alignment = 0;
};
static_assert(sizeof(QueryScq) == 16);

/// struct io_uring_bpf_filter. `filter_ptr` points to `filter_len` classic BPF
/// instructions.
struct BpfFilter {
    std::uint32_t opcode = 0;
    std::uint32_t flags = 0;
    std::uint32_t filter_len = 0;
    std::uint8_t pdu_size = 0;
    std::uint8_t resv[3] = {0, 0, 0};
    std::uint64_t filter_ptr = 0;
    std::uint64_t resv2[5] = {0, 0, 0, 0, 0};
};
static_assert(sizeof(BpfFilter) == 64);

/// struct io_uring_bpf. `cmd_type` is URING_BPF_CMD_*.
struct Bpf {
    std::uint16_t cmd_type = 0;
    std::uint16_t cmd_flags = 0;
    std::uint32_t resv = 0;
    BpfFilter filter;
};
static_assert(sizeof(Bpf) == 72);

// ============================================================================
// Register calls. Each gives 0 or the value the kernel gives, or a negative
// errno. A call that takes a pointer needs what it points to alive until the
// call returns.
// ============================================================================

/// Makes the next register call on the calling thread, on any ring, go through
/// the registered ring index `index` that @ref register_ring_fd gave, with
/// REGISTER_USE_REGISTERED_RING. The call after that uses the ring's own
/// descriptor again.
void use_registered_ring_once(unsigned index);

/// Fixed buffers. Pinned memory is charged to the memlock limit before kernel
/// 5.12.
int register_buffers(Ring& ring, const struct iovec* iovecs, unsigned nr);
int unregister_buffers(Ring& ring);

/// As register_buffers, with `tags` (or none) and RSRC flags: pass
/// IORING_RSRC_REGISTER_SPARSE and null `iovecs` for `nr` empty slots.
int register_buffers2(Ring& ring, const struct iovec* iovecs,
                      const std::uint64_t* tags, unsigned nr,
                      unsigned flags = 0);

/// Gives the count updated.
int update_buffers(Ring& ring, unsigned offset, const struct iovec* iovecs,
                   const std::uint64_t* tags, unsigned nr);

/// Fixed files. An entry of -1 is an empty slot.
int register_files(Ring& ring, const int* fds, unsigned nr);
int unregister_files(Ring& ring);

/// As register_files, with `tags` (or none) and RSRC flags: pass
/// IORING_RSRC_REGISTER_SPARSE and null `fds` for `nr` empty slots.
int register_files2(Ring& ring, const int* fds, const std::uint64_t* tags,
                    unsigned nr, unsigned flags = 0);

/// Both give the count updated. An entry of IORING_REGISTER_FILES_SKIP keeps
/// its slot.
int update_files(Ring& ring, unsigned offset, const int* fds, unsigned nr);
int update_files2(Ring& ring, unsigned offset, const int* fds,
                  const std::uint64_t* tags, unsigned nr);

/// Takes the slots in `[off, off + len)` for requests that ask for
/// IORING_FILE_INDEX_ALLOC.
int register_file_alloc_range(Ring& ring, unsigned off, unsigned len);

/// An eventfd that every completion signals; the async form only for those
/// that end on a kernel worker.
int register_eventfd(Ring& ring, int event_fd);
int register_eventfd_async(Ring& ring, int event_fd);
int unregister_eventfd(Ring& ring);

/// Fills `probe` with the opcodes the kernel has.
int register_probe(Ring& ring, Probe& probe);

/// Gives an id above 0 for the credentials of the calling thread. A request
/// uses it through set_personality().
int register_personality(Ring& ring);
int unregister_personality(Ring& ring, int id);

/// Limits what a ring started with IORING_SETUP_R_DISABLED accepts, then
/// @ref enable_rings starts it.
int register_restrictions(Ring& ring, const Restriction* restrictions,
                          unsigned nr);
int enable_rings(Ring& ring);

/// The CPUs of the kernel workers, as `size` bytes of a cpu_set_t.
int register_iowq_aff(Ring& ring, const void* cpu_mask, std::size_t size);
int unregister_iowq_aff(Ring& ring);

/// `values` are the limits of the bounded and the unbounded workers; 0 leaves
/// one as it is. It gives the old limits back in `values`.
int register_iowq_max_workers(Ring& ring, unsigned (&values)[2]);

/// Puts the ring descriptor in the table of registered descriptors and gives
/// its index.
int register_ring_fd(Ring& ring, unsigned& index);
int unregister_ring_fd(Ring& ring, unsigned index);

/// A ring of provided buffers. See @ref BufRing for the memory.
int register_pbuf_ring(Ring& ring, const BufReg& reg);
int unregister_pbuf_ring(Ring& ring, unsigned bgid);

/// The kernel's head of the buffer ring `bgid`.
int pbuf_status(Ring& ring, unsigned bgid, unsigned& head);

/// Cancels the requests `reg` names and waits until they end.
int sync_cancel(Ring& ring, const SyncCancelReg& reg);

int register_napi(Ring& ring, Napi& napi);
/// Gives the old setting back in `napi`.
int unregister_napi(Ring& ring, Napi& napi);

/// The clock the ring uses for timeouts: CLOCK_MONOTONIC or CLOCK_BOOTTIME.
int register_clock(Ring& ring, const ClockRegister& clock);

/// Copies the registered buffers of the ring `src_fd` to `dst`. An `nr` of 0
/// takes all. `flags` are CLONE_*.
int clone_buffers(Ring& dst, int src_fd, unsigned dst_off, unsigned src_off,
                  unsigned nr, unsigned flags);

/// Runs one MSG_RING request without a ring of its own. `sqe` is a prepared
/// entry, which the call reads.
int send_msg_ring(const struct io_uring_sqe& sqe);

int register_zcrx_ifq(Ring& ring, ZcrxIfqReg& reg);
int zcrx_ctrl(Ring& ring, ZcrxCtrl& ctrl);

/// Changes the sizes of the rings to the entries in `params`. The kernel
/// replaces the ring memory, so a @ref Ring that held the old mappings must be
/// destroyed or mapped again by the caller.
int resize_rings(Ring& ring, struct io_uring_params& params);

int register_mem_region(Ring& ring, const MemRegionReg& reg);

/// Asks the kernel about itself. Needs no ring.
int register_query(QueryHdr& query);

int register_bpf_filter(Ring& ring, const Bpf& bpf);
/// Filters the requests of the calling task instead of one ring.
int register_bpf_filter_task(const Bpf& bpf);

/// The memory of a ring of provided buffers. The kernel takes buffers from the
/// ring in the order they were added. Add buffers, then advance(), so the
/// kernel sees them. Destroy it before its @ref Ring.
class BufRing {
   public:
    BufRing() = default;
    ~BufRing() { release(); }

    BufRing(const BufRing&) = delete;
    BufRing& operator=(const BufRing&) = delete;

    /// Maps the memory for `entries` buffers, a power of two up to 32768, and
    /// registers it as group `bgid` with PBUF_RING_* `flags`. With
    /// PBUF_RING_MMAP the kernel allocates the memory and the ring fd must be
    /// the descriptor, so a ring known only by its registered index gets
    /// -EINVAL. Gives 0 or a negative errno.
    int setup(Ring& ring, unsigned entries, unsigned bgid, unsigned flags = 0);

    /// Writes a buffer at `offset` entries after the tail the kernel last saw.
    void add(void* addr, unsigned len, unsigned bid, unsigned offset);

    /// Shows the `count` buffers that add() wrote to the kernel.
    void advance(unsigned count);

    /// Unregisters the group and unmaps the memory.
    void release();

    unsigned entries() const { return entries_; }
    unsigned bgid() const { return bgid_; }

   private:
    Ring* ring_ = nullptr;
    void* memory_ = nullptr;
    std::size_t memory_size_ = 0;
    unsigned entries_ = 0;
    unsigned bgid_ = 0;
    std::uint16_t tail_ = 0;
};

}  // namespace dftracer::utils::runtime::io::uring

#endif  // DFTRACER_UTILS_HAVE_IO_URING
#endif  // DFTRACER_UTILS_RUNTIME_IO_IO_URING_REGISTER_H
