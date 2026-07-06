#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  shmipc.h  –  POSIX shared-memory SPSC ring-buffer IPC
//
//  Provides ShmIpcPublisher and ShmIpcSource with the same public API as
//  AeronIpcPublisher and AeronIpcSource in aeron.h, making the two
//  interchangeable as a zero-dependency alternative.
//
//  Usage:
//      ShmIpcPublisher pub("myapp", 1001);
//      pub.publish(data, len);           // non-blocking; false if ring full
//
//      ShmIpcSource src("myapp", 1001);
//      src.run([](const uint8_t* d, uint16_t n){ /* process */ });  // blocks
//
//  The shared-memory segment is created by ShmIpcPublisher and opened by
//  ShmIpcSource.  The POSIX shm name is derived from channel + streamId:
//      channel="myapp", streamId=1001  →  "/myapp_1001"
//
//  Ring layout inside the mapped region:
//      [ShmRingHeader — 64 B, cache-line aligned]
//      [slot_0        — SHM_SLOT_STRIDE B]
//      ...
//      [slot_{N-1}    — SHM_SLOT_STRIDE B]
//
//  Each slot:
//      atomic<uint16_t> len   – 0 = free; >0 = payload of that many bytes
//      uint8_t          data[SHM_MAX_PAYLOAD]
//
//  SPSC protocol: producer writes data then stores len (release); consumer
//  spins on len (acquire), reads data, stores len=0 (release).  Both sides
//  track their ring position in process-local variables; only the per-slot
//  len field is shared.
//
//  Build:  g++ -std=c++20 ... -lpthread   (Linux / macOS)
//          shm_open lives in glibc ≥ 2.17 (no -lrt needed on modern Linux).
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace detail {

// ── Ring-buffer constants ─────────────────────────────────────────────────────

static constexpr uint32_t SHM_MAGIC       = 0x584D5249u; // "XMRI"
static constexpr uint32_t SHM_CAPACITY    = 4096u;        // slots (power of 2)
static constexpr uint32_t SHM_SLOT_STRIDE = 8192u;        // bytes per slot
static constexpr uint16_t SHM_MAX_PAYLOAD = 8190u;        // slot_stride − 2

static_assert((SHM_CAPACITY & (SHM_CAPACITY - 1)) == 0,
              "SHM_CAPACITY must be a power of two");
static_assert(std::atomic<uint16_t>::is_always_lock_free,
              "atomic<uint16_t> must be lock-free for cross-process shm IPC");

// ── Shared-memory layout ──────────────────────────────────────────────────────

// Sits at the very start of the mmap'd segment (cache-line sized).
struct alignas(64) ShmRingHeader {
    uint32_t             magic;
    uint32_t             capacity;
    uint32_t             slot_stride;
    uint32_t             max_payload;
    std::atomic<uint32_t> initialized; // written last (release) by publisher
    uint32_t             _pad[11];
};
static_assert(sizeof(ShmRingHeader) == 64, "ShmRingHeader must be 64 bytes");

// One entry in the ring.  Exactly SHM_SLOT_STRIDE bytes.
struct ShmSlot {
    std::atomic<uint16_t> len;              // 0 = free, >0 = payload length
    uint8_t               data[SHM_MAX_PAYLOAD];
};
static_assert(sizeof(ShmSlot) == SHM_SLOT_STRIDE, "ShmSlot size mismatch");
static_assert(sizeof(std::atomic<uint16_t>) == 2,
              "atomic<uint16_t> must be 2 bytes for ShmSlot layout");

inline std::size_t shmTotalSize() noexcept {
    return sizeof(ShmRingHeader) + size_t{SHM_CAPACITY} * SHM_SLOT_STRIDE;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

// Derive a POSIX shm name from a channel string and stream ID.
// e.g. channel="aeron:ipc", streamId=1001  →  "/aeron_ipc_1001"
inline std::string shmName(const std::string& channel, int32_t streamId) {
    std::string name = channel;
    if (name.empty() || name[0] != '/')
        name = '/' + name;
    // Replace characters illegal in POSIX shm names (colons, slashes after /)
    for (std::size_t i = 1; i < name.size(); ++i)
        if (name[i] == ':' || name[i] == '/')
            name[i] = '_';
    name += '_';
    name += std::to_string(streamId);
    return name;
}

// Stop flag that ShmIpcSource::run() polls each iteration.
inline std::atomic<bool>& shmStop() {
    static std::atomic<bool> s{false};
    return s;
}
inline void shmOnSignal(int) { shmStop().store(true, std::memory_order_relaxed); }
inline void shmInstallSignals() {
    struct sigaction sa{};
    sa.sa_handler = shmOnSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

} // namespace detail

// ── ShmIpcPublisher ───────────────────────────────────────────────────────────
//
//  Creates (or re-creates) a POSIX shm segment and exposes a non-blocking
//  publish() call that drops the message if the ring is full.
class ShmIpcPublisher {
public:
    ShmIpcPublisher(std::string channel, int32_t streamId)
        : name_(detail::shmName(channel, streamId)) {
        createSegment();
    }

    ~ShmIpcPublisher() {
        if (base_ && base_ != MAP_FAILED)
            munmap(base_, detail::shmTotalSize());
        if (fd_ >= 0) {
            ::close(fd_);
            shm_unlink(name_.c_str());
        }
    }

    ShmIpcPublisher(const ShmIpcPublisher&)            = delete;
    ShmIpcPublisher& operator=(const ShmIpcPublisher&) = delete;

    // Offer one message to the ring.  Returns true on success, false if the
    // ring is full (back-pressure) or len exceeds SHM_MAX_PAYLOAD.
    bool publish(const uint8_t* data, uint16_t len) {
        if (!base_ || len == 0 || len > detail::SHM_MAX_PAYLOAD)
            return false;

        detail::ShmSlot* slot = slotAt(write_pos_);
        if (slot->len.load(std::memory_order_acquire) != 0)
            return false; // ring full — caller should retry or drop

        std::memcpy(slot->data, data, len);
        slot->len.store(len, std::memory_order_release);
        write_pos_ = (write_pos_ + 1u) % detail::SHM_CAPACITY;
        return true;
    }

private:
    void createSegment() {
        // Remove any stale segment from a previous run before creating a fresh one.
        shm_unlink(name_.c_str());

        fd_ = shm_open(name_.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd_ < 0)
            throw std::runtime_error("[shmipc] shm_open(O_CREAT) failed for " +
                                     name_ + ": " + std::strerror(errno));

        const std::size_t sz = detail::shmTotalSize();
        if (ftruncate(fd_, static_cast<off_t>(sz)) != 0) {
            ::close(fd_); fd_ = -1;
            shm_unlink(name_.c_str());
            throw std::runtime_error("[shmipc] ftruncate failed for " + name_ +
                                     ": " + std::strerror(errno));
        }

        base_ = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (base_ == MAP_FAILED) {
            base_ = nullptr;
            ::close(fd_); fd_ = -1;
            shm_unlink(name_.c_str());
            throw std::runtime_error("[shmipc] mmap failed for " + name_ +
                                     ": " + std::strerror(errno));
        }

        // Zero all slots so every len starts at 0 (free).
        std::memset(base_, 0, sz);

        // Write header metadata and mark as initialised last (release) so the
        // subscriber only reads it after all other writes are visible.
        auto* hdr         = header();
        hdr->magic        = detail::SHM_MAGIC;
        hdr->capacity     = detail::SHM_CAPACITY;
        hdr->slot_stride  = detail::SHM_SLOT_STRIDE;
        hdr->max_payload  = detail::SHM_MAX_PAYLOAD;
        hdr->initialized.store(1u, std::memory_order_release);
    }

    detail::ShmRingHeader* header() noexcept {
        return static_cast<detail::ShmRingHeader*>(base_);
    }

    detail::ShmSlot* slotAt(uint32_t pos) noexcept {
        return reinterpret_cast<detail::ShmSlot*>(
            static_cast<uint8_t*>(base_) + sizeof(detail::ShmRingHeader) +
            pos * sizeof(detail::ShmSlot));
    }

    std::string name_;
    int         fd_        = -1;
    void*       base_      = nullptr;
    uint32_t    write_pos_ = 0u;
};

// ── ShmIpcSource ──────────────────────────────────────────────────────────────
//
//  Opens an existing POSIX shm segment created by ShmIpcPublisher and reads
//  messages from it in a blocking loop until SIGINT/SIGTERM.
class ShmIpcSource {
public:
    ShmIpcSource(std::string channel, int32_t streamId, int32_t fragmentLimit = 64)
        : name_(detail::shmName(channel, streamId)),
          fragmentLimit_(fragmentLimit > 0 ? fragmentLimit : 64) {}

    template <class OnPacket>
    void run(OnPacket&& onPacket) {
        readLoop(std::forward<OnPacket>(onPacket));
    }

private:
    template <class Sink>
    void readLoop(Sink&& sink) {
        // Retry opening the segment until the publisher creates it.
        int fd = -1;
        for (int i = 0; i < 5000; ++i) {
            fd = shm_open(name_.c_str(), O_RDWR, 0);
            if (fd >= 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (fd < 0)
            throw std::runtime_error("[shmipc] timeout opening shm " + name_ +
                                     ": " + std::strerror(errno));

        const std::size_t sz = detail::shmTotalSize();
        void* base = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (base == MAP_FAILED) {
            ::close(fd);
            throw std::runtime_error("[shmipc] mmap failed for " + name_ +
                                     ": " + std::strerror(errno));
        }

        // Wait for the publisher to finish initialising the header.
        auto* hdr = static_cast<detail::ShmRingHeader*>(base);
        for (int i = 0; i < 5000; ++i) {
            if (hdr->initialized.load(std::memory_order_acquire) == 1u) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (hdr->initialized.load(std::memory_order_relaxed) != 1u) {
            munmap(base, sz); ::close(fd);
            throw std::runtime_error("[shmipc] timeout waiting for init on " + name_);
        }
        if (hdr->magic != detail::SHM_MAGIC) {
            munmap(base, sz); ::close(fd);
            throw std::runtime_error("[shmipc] bad magic in " + name_);
        }

        std::cout << "[shmipc] subscribed " << name_ << " (Ctrl-C to stop)\n";
        detail::shmInstallSignals();
        auto& stop = detail::shmStop();
        stop.store(false, std::memory_order_relaxed);

        uint32_t read_pos = 0u;
        while (!stop.load(std::memory_order_relaxed)) {
            int processed = 0;
            for (int i = 0; i < fragmentLimit_ && !stop.load(std::memory_order_relaxed); ++i) {
                auto* slot = reinterpret_cast<detail::ShmSlot*>(
                    static_cast<uint8_t*>(base) + sizeof(detail::ShmRingHeader) +
                    read_pos * sizeof(detail::ShmSlot));
                const uint16_t len = slot->len.load(std::memory_order_acquire);
                if (len == 0)
                    break; // no more data in this burst
                const uint16_t n = std::min<uint16_t>(len, detail::SHM_MAX_PAYLOAD);
                sink(slot->data, n);
                slot->len.store(0u, std::memory_order_release); // free slot
                read_pos = (read_pos + 1u) % detail::SHM_CAPACITY;
                ++processed;
            }
            if (processed == 0)
                std::this_thread::sleep_for(std::chrono::microseconds(200));
        }

        munmap(base, sz);
        ::close(fd);
        std::cout << "[shmipc] stopped\n";
    }

    std::string name_;
    int32_t     fragmentLimit_;
};
