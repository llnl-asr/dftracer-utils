// SPDX-License-Identifier: MIT
// The register calls follow liburing, https://github.com/axboe/liburing (MIT),
// Copyright 2020 Jens Axboe, file src/register.c.
#ifdef DFTRACER_UTILS_HAVE_IO_URING

#include <dftracer/utils/runtime/io/io_uring_register.h>
#include <sys/mman.h>

#include <atomic>
#include <cerrno>
#include <cstring>

namespace dftracer::utils::runtime::io::uring {

namespace {

// -1 when the next call uses the ring's own descriptor.
thread_local int t_registered_index = -1;

void* mutable_ptr(const void* p) { return const_cast<void*>(p); }

std::uint64_t to_u64(const void* p) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p));
}

int call_fd(int fd, unsigned opcode, void* arg, unsigned count) {
    const int ret = sys_io_uring_register(fd, opcode, arg, count);
    return ret < 0 ? -errno : ret;
}

int call(Ring& ring, unsigned opcode, const void* arg, unsigned count) {
    if (t_registered_index >= 0) {
        const int index = t_registered_index;
        t_registered_index = -1;
        return call_fd(index, opcode | REGISTER_USE_REGISTERED_RING,
                       mutable_ptr(arg), count);
    }
    const int ret = ring.register_call(opcode, mutable_ptr(arg), count);
    return ret < 0 ? -errno : ret;
}

}  // namespace

void use_registered_ring_once(unsigned index) {
    t_registered_index = static_cast<int>(index);
}

int register_buffers(Ring& ring, const struct iovec* iovecs, unsigned nr) {
    return call(ring, REG_BUFFERS, iovecs, nr);
}

int unregister_buffers(Ring& ring) {
    return call(ring, REG_UNREGISTER_BUFFERS, nullptr, 0);
}

int register_buffers2(Ring& ring, const struct iovec* iovecs,
                      const std::uint64_t* tags, unsigned nr, unsigned flags) {
    RsrcRegister reg;
    reg.nr = nr;
    reg.flags = flags;
    reg.data = to_u64(iovecs);
    reg.tags = to_u64(tags);
    return call(ring, REG_BUFFERS2, &reg, sizeof(reg));
}

int update_buffers(Ring& ring, unsigned offset, const struct iovec* iovecs,
                   const std::uint64_t* tags, unsigned nr) {
    RsrcUpdate2 up;
    up.offset = offset;
    up.data = to_u64(iovecs);
    up.tags = to_u64(tags);
    up.nr = nr;
    return call(ring, REG_BUFFERS_UPDATE, &up, sizeof(up));
}

int register_files(Ring& ring, const int* fds, unsigned nr) {
    return call(ring, REG_FILES, fds, nr);
}

int unregister_files(Ring& ring) {
    return call(ring, REG_UNREGISTER_FILES, nullptr, 0);
}

int register_files2(Ring& ring, const int* fds, const std::uint64_t* tags,
                    unsigned nr, unsigned flags) {
    RsrcRegister reg;
    reg.nr = nr;
    reg.flags = flags;
    reg.data = to_u64(fds);
    reg.tags = to_u64(tags);
    return call(ring, REG_FILES2, &reg, sizeof(reg));
}

int update_files(Ring& ring, unsigned offset, const int* fds, unsigned nr) {
    RsrcUpdate up;
    up.offset = offset;
    up.data = to_u64(fds);
    return call(ring, REG_FILES_UPDATE, &up, nr);
}

int update_files2(Ring& ring, unsigned offset, const int* fds,
                  const std::uint64_t* tags, unsigned nr) {
    RsrcUpdate2 up;
    up.offset = offset;
    up.data = to_u64(fds);
    up.tags = to_u64(tags);
    up.nr = nr;
    return call(ring, REG_FILES_UPDATE2, &up, sizeof(up));
}

int register_file_alloc_range(Ring& ring, unsigned off, unsigned len) {
    FileIndexRange range;
    range.off = off;
    range.len = len;
    return call(ring, REG_FILE_ALLOC_RANGE, &range, 0);
}

int register_eventfd(Ring& ring, int event_fd) {
    return call(ring, REG_EVENTFD, &event_fd, 1);
}

int register_eventfd_async(Ring& ring, int event_fd) {
    return call(ring, REG_EVENTFD_ASYNC, &event_fd, 1);
}

int unregister_eventfd(Ring& ring) {
    return call(ring, REG_UNREGISTER_EVENTFD, nullptr, 0);
}

int register_probe(Ring& ring, Probe& probe) {
    std::memset(&probe, 0, sizeof(probe));
    return call(ring, REG_PROBE, &probe, PROBE_OPS);
}

int register_personality(Ring& ring) {
    return call(ring, REG_PERSONALITY, nullptr, 0);
}

int unregister_personality(Ring& ring, int id) {
    return call(ring, REG_UNREGISTER_PERSONALITY, nullptr,
                static_cast<unsigned>(id));
}

int register_restrictions(Ring& ring, const Restriction* restrictions,
                          unsigned nr) {
    return call(ring, REG_RESTRICTIONS, restrictions, nr);
}

int enable_rings(Ring& ring) {
    return call(ring, REG_ENABLE_RINGS, nullptr, 0);
}

int register_iowq_aff(Ring& ring, const void* cpu_mask, std::size_t size) {
    if (size >= (1U << 31)) return -EINVAL;
    return call(ring, REG_IOWQ_AFF, cpu_mask, static_cast<unsigned>(size));
}

int unregister_iowq_aff(Ring& ring) {
    return call(ring, REG_UNREGISTER_IOWQ_AFF, nullptr, 0);
}

int register_iowq_max_workers(Ring& ring, unsigned (&values)[2]) {
    return call(ring, REG_IOWQ_MAX_WORKERS, values, 2);
}

int register_ring_fd(Ring& ring, unsigned& index) {
    RsrcUpdate up;
    up.offset = ~0U;
    up.data = static_cast<std::uint64_t>(ring.fd());
    const int ret = call(ring, REG_RING_FDS, &up, 1);
    if (ret < 0) return ret;
    if (ret != 1) return -EIO;
    index = up.offset;
    return 0;
}

int unregister_ring_fd(Ring& ring, unsigned index) {
    RsrcUpdate up;
    up.offset = index;
    const int ret = call(ring, REG_UNREGISTER_RING_FDS, &up, 1);
    if (ret < 0) return ret;
    return ret == 1 ? 0 : -EIO;
}

int register_pbuf_ring(Ring& ring, const BufReg& reg) {
    return call(ring, REG_PBUF_RING, &reg, 1);
}

int unregister_pbuf_ring(Ring& ring, unsigned bgid) {
    BufReg reg;
    reg.bgid = static_cast<std::uint16_t>(bgid);
    return call(ring, REG_UNREGISTER_PBUF_RING, &reg, 1);
}

int pbuf_status(Ring& ring, unsigned bgid, unsigned& head) {
    BufStatus status;
    status.buf_group = bgid;
    const int ret = call(ring, REG_PBUF_STATUS, &status, 1);
    if (ret == 0) head = status.head;
    return ret;
}

int sync_cancel(Ring& ring, const SyncCancelReg& reg) {
    return call(ring, REG_SYNC_CANCEL, &reg, 1);
}

int register_napi(Ring& ring, Napi& napi) {
    return call(ring, REG_NAPI, &napi, 1);
}

int unregister_napi(Ring& ring, Napi& napi) {
    return call(ring, REG_UNREGISTER_NAPI, &napi, 1);
}

int register_clock(Ring& ring, const ClockRegister& clock) {
    return call(ring, REG_CLOCK, &clock, 0);
}

int clone_buffers(Ring& dst, int src_fd, unsigned dst_off, unsigned src_off,
                  unsigned nr, unsigned flags) {
    CloneBuffers clone;
    clone.src_fd = static_cast<std::uint32_t>(src_fd);
    clone.flags = flags;
    clone.src_off = src_off;
    clone.dst_off = dst_off;
    clone.nr = nr;
    return call(dst, REG_CLONE_BUFFERS, &clone, 1);
}

int send_msg_ring(const struct io_uring_sqe& sqe) {
    return call_fd(-1, REG_SEND_MSG_RING, mutable_ptr(&sqe), 1);
}

int register_zcrx_ifq(Ring& ring, ZcrxIfqReg& reg) {
    return call(ring, REG_ZCRX_IFQ, &reg, 1);
}

int zcrx_ctrl(Ring& ring, ZcrxCtrl& ctrl) {
    return call(ring, REG_ZCRX_CTRL, &ctrl, 0);
}

int resize_rings(Ring& ring, struct io_uring_params& params) {
    std::memset(&params.sq_off, 0, sizeof(params.sq_off));
    std::memset(&params.cq_off, 0, sizeof(params.cq_off));
    return call(ring, REG_RESIZE_RINGS, &params, 1);
}

int register_mem_region(Ring& ring, const MemRegionReg& reg) {
    return call(ring, REG_MEM_REGION, &reg, 1);
}

int register_query(QueryHdr& query) {
    return call_fd(-1, REG_QUERY, &query, 0);
}

int register_bpf_filter(Ring& ring, const Bpf& bpf) {
    return call(ring, REG_BPF_FILTER, &bpf, 1);
}

int register_bpf_filter_task(const Bpf& bpf) {
    return call_fd(-1, REG_BPF_FILTER, mutable_ptr(&bpf), 1);
}

int BufRing::setup(Ring& ring, unsigned entries, unsigned bgid,
                   unsigned flags) {
    if (memory_ != nullptr) return -EBUSY;
    if ((flags & PBUF_RING_MMAP) != 0 && ring.is_fd_registered()) {
        return -EINVAL;
    }
    if (entries == 0 || entries > 32768 || (entries & (entries - 1)) != 0 ||
        bgid > 0xffffU) {
        return -EINVAL;
    }
    const std::size_t size =
        static_cast<std::size_t>(entries) * sizeof(ProvidedBuf);
    const bool is_kernel_memory = (flags & PBUF_RING_MMAP) != 0;
    void* memory = MAP_FAILED;
    if (!is_kernel_memory) {
        memory = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) return -errno;
    }
    BufReg reg;
    reg.ring_addr = is_kernel_memory ? 0 : to_u64(memory);
    reg.ring_entries = entries;
    reg.bgid = static_cast<std::uint16_t>(bgid);
    reg.flags = static_cast<std::uint16_t>(flags);
    const int ret = register_pbuf_ring(ring, reg);
    if (ret < 0) {
        if (!is_kernel_memory) ::munmap(memory, size);
        return ret;
    }
    if (is_kernel_memory) {
        const off_t offset = static_cast<off_t>(
            PBUF_RING_OFFSET |
            (static_cast<std::uint64_t>(bgid) << PBUF_RING_OFFSET_SHIFT));
        memory = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, ring.fd(), offset);
        if (memory == MAP_FAILED) {
            const int error = -errno;
            (void)unregister_pbuf_ring(ring, bgid);
            return error;
        }
    }
    ring_ = &ring;
    memory_ = memory;
    memory_size_ = size;
    entries_ = entries;
    bgid_ = bgid;
    tail_ = 0;
    return 0;
}

void BufRing::add(void* addr, unsigned len, unsigned bid, unsigned offset) {
    ProvidedBuf* const bufs = static_cast<ProvidedBuf*>(memory_);
    ProvidedBuf& buf =
        bufs[(static_cast<unsigned>(tail_) + offset) & (entries_ - 1)];
    buf.addr = to_u64(addr);
    buf.len = len;
    buf.bid = static_cast<std::uint16_t>(bid);
}

// The tail shares the bytes of the first entry's resv field, after the
// io_uring_buf_ring union of the kernel header.
void BufRing::advance(unsigned count) {
    tail_ = static_cast<std::uint16_t>(tail_ + count);
    void* const tail_bytes =
        static_cast<char*>(memory_) + offsetof(ProvidedBuf, resv);
    std::uint16_t* const tail = static_cast<std::uint16_t*>(tail_bytes);
    std::atomic_ref<std::uint16_t>(*tail).store(tail_,
                                                std::memory_order_release);
}

void BufRing::release() {
    if (memory_ == nullptr) return;
    (void)unregister_pbuf_ring(*ring_, bgid_);
    ::munmap(memory_, memory_size_);
    memory_ = nullptr;
    ring_ = nullptr;
    entries_ = 0;
    tail_ = 0;
}

}  // namespace dftracer::utils::runtime::io::uring

#endif  // DFTRACER_UTILS_HAVE_IO_URING
