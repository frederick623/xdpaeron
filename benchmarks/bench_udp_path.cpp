// benchmarks/bench_udp_path.cpp
//
// Compares the per-packet cost of two receive paths:
//
//   BM_XdpParseAndFilter  — models the kernel-bypass (AF_XDP) path:
//       the kernel delivers a raw Ethernet frame to userspace; we must
//       parse Ethernet/IPv4/UDP headers and apply a port+IP filter before
//       forwarding the payload.  Benchmarks parseUdp() + the filter closure
//       on a pre-built in-memory frame.  No syscall involved; this is the
//       hot path cost per frame after AF_XDP delivers it.
//
//   BM_UdpKernelRecv      — models the ordinary kernel-UDP path:
//       a background sender thread sends datagrams over loopback; the
//       benchmark loop calls recvfrom() once per iteration.  This pays the
//       full syscall + kernel networking stack cost per packet.
//
// Build & run:
//   cmake -S . -B build && cmake --build build --target xdpaeron_bench
//   ./build/xdpaeron_bench [--benchmark_format=json] [--benchmark_filter=...]
//
// Payload sizes tested: 64, 512, 1400 bytes.
//
// Interpretation:
//   XDP path  = header-parsing overhead only (user-space, no syscall).
//   Kernel UDP = syscall + kernel TCP/IP stack overhead per packet.
//   The gap between the two illustrates the latency saving AF_XDP provides
//   on top of the user-space processing that both paths still perform.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <benchmark/benchmark.h>

#include "xdpudp.h"

using mde::feed::detail::UdpView;
using mde::feed::detail::parseUdp;

// ── Frame builder (identical to the one in tests/test_parse.cpp) ─────────────

static void push16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x & 0xFFu));
}

static void push32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >>  8) & 0xFFu));
    v.push_back(static_cast<uint8_t>(x & 0xFFu));
}

static void pushIp(std::vector<uint8_t>& v, uint32_t netOrderIp) {
    uint8_t b[4];
    std::memcpy(b, &netOrderIp, 4);
    v.insert(v.end(), b, b + 4);
}

// Build Ethernet/IPv4/UDP frame with |payloadSize| bytes of payload.
static std::vector<uint8_t> buildUdpFrame(std::size_t payloadSize,
                                          uint16_t    dstPort = 50000) {
    std::vector<uint8_t> payload(payloadSize, 0xABu);
    const uint32_t srcIp = inet_addr("192.168.1.1");
    const uint32_t dstIp = inet_addr("239.1.2.3");

    std::vector<uint8_t> f;
    f.reserve(14 + 20 + 8 + payloadSize);

    // Ethernet header.
    f.insert(f.end(), {0x01, 0x00, 0x5e, 0x01, 0x02, 0x03}); // dst (mcast)
    f.insert(f.end(), {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}); // src
    push16(f, 0x0800u);                                        // IPv4

    // IPv4 header (no options, IHL=5).
    const uint16_t udpLen   = 8u + static_cast<uint16_t>(payloadSize);
    const uint16_t ipTotal  = 20u + udpLen;
    f.push_back(0x45u);            // version=4, IHL=5
    f.push_back(0x00u);            // DSCP/ECN
    push16(f, ipTotal);
    push16(f, 0x0001u);            // identification
    push16(f, 0x0000u);            // flags + frag offset
    f.push_back(64u);              // TTL
    f.push_back(0x11u);            // protocol = UDP
    push16(f, 0x0000u);            // checksum (not validated by parser)
    pushIp(f, srcIp);
    pushIp(f, dstIp);

    // UDP header.
    push16(f, 12345u);   // src port
    push16(f, dstPort);  // dst port
    push16(f, udpLen);
    push16(f, 0x0000u);  // checksum

    // Payload.
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

// ── BM_XdpParseAndFilter ─────────────────────────────────────────────────────
//
// Models the AF_XDP hot path: receive one raw frame from the UMEM ring,
// call parseUdp, and apply a dst-port + dst-IP filter.
// No syscall, no kernel involvement — pure user-space parsing cost.

static void BM_XdpParseAndFilter(benchmark::State& state) {
    const std::size_t payloadSize = static_cast<std::size_t>(state.range(0));
    const uint16_t    wantPort    = 50000;
    const uint32_t    wantDstIp   = inet_addr("239.1.2.3");

    const auto frame = buildUdpFrame(payloadSize, wantPort);
    const uint8_t* data = frame.data();
    const uint32_t len  = static_cast<uint32_t>(frame.size());

    // The filter closure mirrors UdpSource::readLoop's lambda.
    auto filter = [=](const UdpView& v) noexcept {
        return v.dstPort == wantPort && v.dstIp == wantDstIp;
    };

    for (auto _ : state) {
        UdpView v{};
        bool ok = parseUdp(data, len, v);
        benchmark::DoNotOptimize(ok);
        bool pass = ok && filter(v);
        benchmark::DoNotOptimize(pass);
        benchmark::ClobberMemory();
    }

    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) *
        static_cast<int64_t>(len));
    state.SetLabel("xdp-parse+filter");
}

// ── BM_UdpKernelRecv ─────────────────────────────────────────────────────────
//
// Models the ordinary kernel-UDP path: the benchmark loop calls recvfrom()
// on a loopback SOCK_DGRAM socket.  A background sender thread saturates
// the socket's receive buffer so the benchmark never blocks — we measure
// steady-state per-packet kernel delivery cost, not idle wakeup latency.

class UdpLoopbackFixture {
public:
    explicit UdpLoopbackFixture(std::size_t payloadSize) {
        // Create receiver socket, bind to loopback on an ephemeral port.
        recvFd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (recvFd_ < 0) throw std::runtime_error("socket(recv)");

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0; // let kernel pick
        if (::bind(recvFd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0)
            throw std::runtime_error("bind");

        socklen_t addrLen = sizeof addr;
        ::getsockname(recvFd_, reinterpret_cast<sockaddr*>(&addr), &addrLen);

        // Create sender socket, connect to the receiver.
        sendFd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sendFd_ < 0) throw std::runtime_error("socket(send)");
        if (::connect(sendFd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0)
            throw std::runtime_error("connect");

        // Boost socket receive buffer so the sender can pre-fill it.
        int rcvbuf = 4 * 1024 * 1024;
        ::setsockopt(recvFd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);

        payload_.resize(payloadSize, 0xBBu);

        // Background sender: keeps the recv buffer full.
        stop_.store(false, std::memory_order_relaxed);
        senderThread_ = std::thread([this] {
            while (!stop_.load(std::memory_order_relaxed)) {
                ::send(sendFd_, payload_.data(),
                       static_cast<int>(payload_.size()), MSG_DONTWAIT);
            }
        });
    }

    ~UdpLoopbackFixture() {
        stop_.store(true, std::memory_order_relaxed);
        senderThread_.join();
        ::close(sendFd_);
        ::close(recvFd_);
    }

    int recvFd() const { return recvFd_; }
    std::size_t payloadSize() const { return payload_.size(); }

private:
    int              recvFd_ = -1;
    int              sendFd_ = -1;
    std::vector<uint8_t> payload_;
    std::atomic<bool> stop_{false};
    std::thread      senderThread_;
};

static void BM_UdpKernelRecv(benchmark::State& state) {
    const std::size_t payloadSize = static_cast<std::size_t>(state.range(0));
    UdpLoopbackFixture fixture(payloadSize);

    // Give the sender time to pre-fill the receive buffer.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::vector<uint8_t> buf(65536);
    for (auto _ : state) {
        ssize_t n = ::recvfrom(fixture.recvFd(),
                               buf.data(), buf.size(), 0,
                               nullptr, nullptr);
        benchmark::DoNotOptimize(n);
        benchmark::ClobberMemory();
    }

    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) *
        static_cast<int64_t>(payloadSize));
    state.SetLabel("kernel-udp-recv");
}

// ── Registration ─────────────────────────────────────────────────────────────

BENCHMARK(BM_XdpParseAndFilter)
    ->Arg(64)
    ->Arg(512)
    ->Arg(1400)
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BM_UdpKernelRecv)
    ->Arg(64)
    ->Arg(512)
    ->Arg(1400)
    ->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
