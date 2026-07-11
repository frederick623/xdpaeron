// benchmarks/bench_shm_path.cpp
//
// Benchmarks for the shared-memory IPC receive path and the full
// UDP → shared-memory → consumer pipeline stack.
//
//   BM_ShmIpcPublish        — isolated ShmIpcPublisher::publish() throughput;
//                             a background consumer drains the ring continuously.
//
//   BM_ShmIpcRecv           — isolated shared-memory consumer throughput;
//                             a background producer keeps the ring saturated.
//
//   BM_UdpKernelPlusShmRecv — full pipeline: a background sender saturates a
//                             loopback UDP socket; a background relayer thread
//                             calls recvfrom() then publish() — exactly the hot
//                             path of shm_udp_relayer; the benchmark iteration
//                             drains one shm slot, measuring end-to-end
//                             throughput through the entire UDP + shared-memory
//                             receiver stack.
//
// Build & run:
//   cmake -S . -B build && cmake --build build --target xdpaeron_bench_shm
//   ./build/xdpaeron_bench_shm [--benchmark_format=json] [--benchmark_filter=...]
//
// Payload sizes tested: 64, 512, 1400 bytes.
//
// Interpretation:
//   BM_ShmIpcPublish        = cost of a single ring write (memcpy + atomic store).
//   BM_ShmIpcRecv           = cost of a single ring drain (atomic load + memcpy +
//                             atomic store); includes cross-CPU cache-line transfer
//                             latency since producer and consumer run on separate
//                             threads.
//   BM_UdpKernelPlusShmRecv = full pipeline latency per message: kernel UDP recv
//                             (recvfrom syscall) + ring write + ring drain.

#include <atomic>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <benchmark/benchmark.h>

#include "shmipc.h"

// ── ShmConsumerView ──────────────────────────────────────────────────────────
//
// Lightweight helper that opens an existing POSIX shm segment (already created
// by ShmIpcPublisher) and exposes direct slot-level access.  Avoids the full
// signal-handling event loop inside ShmIpcSource::run() so the benchmark tight
// loop only pays the ring-protocol cost.

class ShmConsumerView {
public:
    explicit ShmConsumerView(const std::string& name)
        : sz_(detail::shmTotalSize()) {
        // Wait up to 200 ms for the publisher to create the segment.
        int fd = -1;
        for (int i = 0; i < 200; ++i) {
            fd = shm_open(name.c_str(), O_RDWR, 0);
            if (fd >= 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (fd < 0)
            throw std::runtime_error("ShmConsumerView: cannot open " + name);

        base_ = mmap(nullptr, sz_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (base_ == MAP_FAILED)
            throw std::runtime_error("ShmConsumerView: mmap failed");

        // Wait for publisher to finish initialising the header.
        auto* hdr = static_cast<detail::ShmRingHeader*>(base_);
        for (int i = 0; i < 200; ++i) {
            if (hdr->initialized.load(std::memory_order_acquire) == 1u) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (hdr->initialized.load(std::memory_order_relaxed) != 1u) {
            munmap(base_, sz_);
            throw std::runtime_error("ShmConsumerView: init timeout for " + name);
        }
    }

    ~ShmConsumerView() {
        if (base_ && base_ != MAP_FAILED)
            munmap(base_, sz_);
    }

    ShmConsumerView(const ShmConsumerView&)            = delete;
    ShmConsumerView& operator=(const ShmConsumerView&) = delete;

    // Non-blocking: returns 0 if the next slot is still empty.
    uint16_t tryDrainOne(uint32_t& pos, uint8_t* buf) noexcept {
        detail::ShmSlot* slot = slotAt(pos);
        const uint16_t len = slot->len.load(std::memory_order_acquire);
        if (len == 0) return 0;
        const uint16_t n = std::min<uint16_t>(len, detail::SHM_MAX_PAYLOAD);
        if (buf) std::memcpy(buf, slot->data, n);
        slot->len.store(0u, std::memory_order_release);
        pos = (pos + 1u) % detail::SHM_CAPACITY;
        return n;
    }

    // Blocking: busy-polls until the next slot is filled, then drains it.
    // This is the hot path measured by the *Recv benchmarks.
    uint16_t drainOne(uint32_t& pos, uint8_t* buf) noexcept {
        detail::ShmSlot* slot = slotAt(pos);
        uint16_t len = 0;
        while ((len = slot->len.load(std::memory_order_acquire)) == 0)
            ; // spin — includes cross-CPU cache-coherence latency
        const uint16_t n = std::min<uint16_t>(len, detail::SHM_MAX_PAYLOAD);
        if (buf) std::memcpy(buf, slot->data, n);
        slot->len.store(0u, std::memory_order_release);
        pos = (pos + 1u) % detail::SHM_CAPACITY;
        return n;
    }

private:
    detail::ShmSlot* slotAt(uint32_t pos) noexcept {
        return reinterpret_cast<detail::ShmSlot*>(
            static_cast<uint8_t*>(base_) + sizeof(detail::ShmRingHeader) +
            pos * sizeof(detail::ShmSlot));
    }

    void*       base_ = nullptr;
    std::size_t sz_   = 0;
};

// ── BM_ShmIpcPublish ─────────────────────────────────────────────────────────
//
// Measures steady-state ShmIpcPublisher::publish() throughput.
// A background drain thread keeps the ring continuously empty so the publisher
// never encounters back-pressure; we isolate only the write-side cost.

class ShmPublishFixture {
public:
    explicit ShmPublishFixture(std::size_t payloadSize)
        : payloadSize_(payloadSize),
          pub_("bench_pub", 2001),
          view_(detail::shmName("bench_pub", 2001)) {
        payload_.resize(payloadSize, 0xCCu);
        stop_.store(false, std::memory_order_relaxed);
        // Drain thread: non-blocking poll keeps the ring empty.
        drainThread_ = std::thread([this] {
            uint32_t rpos = 0;
            std::vector<uint8_t> buf(detail::SHM_MAX_PAYLOAD);
            while (!stop_.load(std::memory_order_relaxed))
                view_.tryDrainOne(rpos, buf.data());
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ~ShmPublishFixture() {
        stop_.store(true, std::memory_order_relaxed);
        drainThread_.join();
    }

    // Publish one message; retries briefly on back-pressure.
    void doPublish() noexcept {
        while (!pub_.publish(payload_.data(),
                             static_cast<uint16_t>(payloadSize_)))
            ; // ring full — spin until drain thread catches up
    }

    std::size_t payloadSize() const { return payloadSize_; }

private:
    std::size_t          payloadSize_;
    ShmIpcPublisher      pub_;
    ShmConsumerView      view_;
    std::vector<uint8_t> payload_;
    std::atomic<bool>    stop_{false};
    std::thread          drainThread_;
};

static void BM_ShmIpcPublish(benchmark::State& state) {
    const std::size_t payloadSize = static_cast<std::size_t>(state.range(0));
    ShmPublishFixture fixture(payloadSize);

    for (auto _ : state) {
        fixture.doPublish();
        benchmark::ClobberMemory();
    }

    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) *
        static_cast<int64_t>(payloadSize));
    state.SetLabel("shm-publish");
}

// ── BM_ShmIpcRecv ────────────────────────────────────────────────────────────
//
// Measures steady-state shared-memory consumer throughput.
// A background producer thread keeps the ring saturated; each benchmark
// iteration busy-polls one slot, copies the payload, and releases the slot.

class ShmRecvFixture {
public:
    explicit ShmRecvFixture(std::size_t payloadSize)
        : payloadSize_(payloadSize),
          pub_("bench_recv", 2002),
          view_(detail::shmName("bench_recv", 2002)) {
        payload_.resize(payloadSize, 0xDDu);
        stop_.store(false, std::memory_order_relaxed);
        // Producer thread: publish in a tight loop.
        producerThread_ = std::thread([this] {
            while (!stop_.load(std::memory_order_relaxed)) {
                if (!pub_.publish(payload_.data(),
                                  static_cast<uint16_t>(payloadSize_)))
                    std::this_thread::yield(); // ring full — let consumer catch up
            }
        });
        // Let the producer warm up and partially pre-fill the ring.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ~ShmRecvFixture() {
        stop_.store(true, std::memory_order_relaxed);
        producerThread_.join();
    }

    ShmConsumerView& view() { return view_; }
    std::size_t payloadSize() const { return payloadSize_; }

private:
    std::size_t          payloadSize_;
    ShmIpcPublisher      pub_;
    ShmConsumerView      view_;
    std::vector<uint8_t> payload_;
    std::atomic<bool>    stop_{false};
    std::thread          producerThread_;
};

static void BM_ShmIpcRecv(benchmark::State& state) {
    const std::size_t payloadSize = static_cast<std::size_t>(state.range(0));
    ShmRecvFixture fixture(payloadSize);

    uint32_t rpos = 0;
    std::vector<uint8_t> buf(detail::SHM_MAX_PAYLOAD);
    for (auto _ : state) {
        uint16_t n = fixture.view().drainOne(rpos, buf.data());
        benchmark::DoNotOptimize(n);
        benchmark::ClobberMemory();
    }

    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) *
        static_cast<int64_t>(payloadSize));
    state.SetLabel("shm-recv");
}

// ── BM_UdpKernelPlusShmRecv ──────────────────────────────────────────────────
//
// Models the full production receive stack implemented by shm_udp_relayer:
//
//   [UDP sender] ──loopback──> recvfrom() ──> publish() ──shm ring──> consumer
//
// A background sender saturates a loopback SOCK_DGRAM socket.  A background
// relayer thread calls recvfrom() and then ShmIpcPublisher::publish() — the
// same two operations in shm_udp_relayer's run() callback.  The benchmark
// iteration drains one shm slot, measuring steady-state throughput through
// the entire UDP + shared-memory receiver pipeline.

class UdpShmStackFixture {
public:
    explicit UdpShmStackFixture(std::size_t payloadSize)
        : payloadSize_(payloadSize),
          pub_("bench_udpshm", 2003),
          view_(detail::shmName("bench_udpshm", 2003)) {
        // Receiver UDP socket bound to loopback on an ephemeral port.
        recvFd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (recvFd_ < 0) throw std::runtime_error("socket(recv)");

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        if (::bind(recvFd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0)
            throw std::runtime_error("bind");

        socklen_t addrLen = sizeof addr;
        ::getsockname(recvFd_, reinterpret_cast<sockaddr*>(&addr), &addrLen);

        // Sender socket connected to the receiver.
        sendFd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sendFd_ < 0) throw std::runtime_error("socket(send)");
        if (::connect(sendFd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0)
            throw std::runtime_error("connect");

        // Large receive buffer so the sender can pre-fill the socket queue.
        int rcvbuf = 4 * 1024 * 1024;
        ::setsockopt(recvFd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);

        udpPayload_.resize(payloadSize, 0xEEu);
        stop_.store(false, std::memory_order_relaxed);

        // Background UDP sender: saturates the loopback socket.
        senderThread_ = std::thread([this] {
            while (!stop_.load(std::memory_order_relaxed)) {
                ::send(sendFd_, udpPayload_.data(),
                       static_cast<int>(udpPayload_.size()), MSG_DONTWAIT);
            }
        });

        // Background relayer: recvfrom() → publish() — mirrors shm_udp_relayer.
        relayerThread_ = std::thread([this] {
            std::vector<uint8_t> buf(65536);
            while (!stop_.load(std::memory_order_relaxed)) {
                const ssize_t n = ::recvfrom(recvFd_, buf.data(), buf.size(),
                                             MSG_DONTWAIT, nullptr, nullptr);
                if (n > 0)
                    pub_.publish(buf.data(), static_cast<uint16_t>(n));
            }
        });

        // Allow the pipeline to pre-fill the shm ring before benchmarking starts.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ~UdpShmStackFixture() {
        stop_.store(true, std::memory_order_relaxed);
        senderThread_.join();
        relayerThread_.join();
        ::close(sendFd_);
        ::close(recvFd_);
    }

    ShmConsumerView& view() { return view_; }
    std::size_t payloadSize() const { return payloadSize_; }

private:
    std::size_t          payloadSize_;
    ShmIpcPublisher      pub_;
    ShmConsumerView      view_;
    std::vector<uint8_t> udpPayload_;
    int                  recvFd_ = -1;
    int                  sendFd_ = -1;
    std::atomic<bool>    stop_{false};
    std::thread          senderThread_;
    std::thread          relayerThread_;
};

static void BM_UdpKernelPlusShmRecv(benchmark::State& state) {
    const std::size_t payloadSize = static_cast<std::size_t>(state.range(0));
    UdpShmStackFixture fixture(payloadSize);

    uint32_t rpos = 0;
    std::vector<uint8_t> buf(detail::SHM_MAX_PAYLOAD);
    for (auto _ : state) {
        uint16_t n = fixture.view().drainOne(rpos, buf.data());
        benchmark::DoNotOptimize(n);
        benchmark::ClobberMemory();
    }

    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) *
        static_cast<int64_t>(payloadSize));
    state.SetLabel("udp+shm-recv");
}

// ── Registration ─────────────────────────────────────────────────────────────

BENCHMARK(BM_ShmIpcPublish)
    ->Arg(64)
    ->Arg(512)
    ->Arg(1400)
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BM_ShmIpcRecv)
    ->Arg(64)
    ->Arg(512)
    ->Arg(1400)
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BM_UdpKernelPlusShmRecv)
    ->Arg(64)
    ->Arg(512)
    ->Arg(1400)
    ->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
