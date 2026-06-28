#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  feed/xdp_udp.h  –  AF_XDP (kernel-bypass) drop-in replacement for udp.h
//
//  Same public surface as feed/udp.h (UdpSource / UdpConnector), so existing
//  call sites compile unchanged.  Instead of Boost.Asio + the kernel UDP stack,
//  this binds an AF_XDP socket directly to a NIC queue.  RX frames are redirected
//  to user space by libxdp's built-in default XDP program (no custom .o to ship),
//  parsed (Ethernet/IPv4/UDP) here, filtered, and the UDP *payload* is forwarded
//  to sink(data, len) — identical to PcapSource / TextFileSource / the old udp.h.
//
//  Build (Ubuntu 24):
//      sudo apt install libxdp-dev libbpf-dev
//      g++ -std=c++20 ... -lxdp -lbpf -lpthread
//  Run with privileges (CAP_NET_RAW + CAP_BPF + CAP_SYS_ADMIN); easiest: sudo.
//  Pin the NIC to a single RX queue first so all traffic lands on queue 0:
//      sudo ethtool -L eth0 combined 1
//
//  ── Constructor-argument remapping vs. udp.h ────────────────────────────────
//    AF_XDP attaches to a NIC *interface name* (e.g. "eth0"), which udp.h never
//    needed.  To keep the signatures byte-for-byte identical:
//
//      UdpSource(listenAddr, port, nic="")
//          listenAddr – dst IP filter ("" or "0.0.0.0" = any; multicast OK)
//          port       – dst UDP port filter
//          nic        – NIC name to attach to.  If empty, defaults to "eth0".
//                       (Was the multicast iface IP in udp.h — under XDP it
//                       is the interface *name*.)
//
//      UdpConnector(remoteHost, remotePort, localPort=0, nic="")
//          remoteHost – src IP filter (DNS name or dotted-decimal)
//          remotePort – src UDP port filter
//          localPort  – if non-zero, also require dst port == localPort
//          nic        – NIC name to attach to.  If empty, defaults to "eth0".
//
//  NOTE: On Colima's virtio NIC zero-copy is typically unavailable; the socket
//  falls back to copy/SKB mode automatically.  Functionally correct, but it is
//  not a true latency bypass there — that requires real hardware.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/if_link.h>   // XDP_FLAGS_*
#include <xdp/xsk.h>         // libxdp; if you only have libbpf use <bpf/xsk.h>

namespace detail {

// ── Stop flag driven by SIGINT/SIGTERM (mirrors udp.h's signal handling) ──────
inline std::atomic<bool>& xdpStop() {
    static std::atomic<bool> s{false};
    return s;
}
inline void xdpOnSignal(int) { xdpStop().store(true, std::memory_order_relaxed); }
inline void xdpInstallSignals() {
    struct sigaction sa{};
    sa.sa_handler = xdpOnSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// ── Parsed view of one UDP datagram inside a raw L2 frame ─────────────────────
struct UdpView {
    uint32_t       srcIp = 0, dstIp = 0;   // network byte order
    uint16_t       srcPort = 0, dstPort = 0; // host byte order
    const uint8_t* payload = nullptr;
    uint16_t       payloadLen = 0;
};

// Parse Ethernet [+ one VLAN tag] / IPv4 / UDP.  Returns false on anything else.
inline bool parseUdp(const uint8_t* f, uint32_t len, UdpView& v) {
    if (len < sizeof(ether_header)) return false;
    const auto* eth = reinterpret_cast<const ether_header*>(f);
    uint16_t et = ntohs(eth->ether_type);
    uint32_t off = sizeof(ether_header);

    if (et == ETHERTYPE_VLAN) {                 // 802.1Q
        if (len < off + 4) return false;
        et = ntohs(*reinterpret_cast<const uint16_t*>(f + off + 2));
        off += 4;
    }
    if (et != ETHERTYPE_IP) return false;
    if (len < off + sizeof(struct ip)) return false;

    const auto* iph = reinterpret_cast<const struct ip*>(f + off);
    if (iph->ip_v != 4 || iph->ip_p != IPPROTO_UDP) return false;
    const uint32_t ihl = static_cast<uint32_t>(iph->ip_hl) * 4u;
    if (ihl < 20 || len < off + ihl) return false;

    const uint32_t uoff = off + ihl;
    if (len < uoff + sizeof(struct udphdr)) return false;
    const auto* uh = reinterpret_cast<const struct udphdr*>(f + uoff);

    const uint32_t poff = uoff + sizeof(struct udphdr);
    uint16_t ulen = ntohs(uh->len);
    if (ulen < sizeof(struct udphdr)) return false;
    uint32_t plen = ulen - sizeof(struct udphdr);
    if (poff + plen > len) {                    // truncated capture: clamp
        if (poff > len) return false;
        plen = len - poff;
    }

    v.srcIp      = iph->ip_src.s_addr;
    v.dstIp      = iph->ip_dst.s_addr;
    v.srcPort    = ntohs(uh->source);
    v.dstPort    = ntohs(uh->dest);
    v.payload    = f + poff;
    v.payloadLen = static_cast<uint16_t>(plen);
    return true;
}

// Resolve a host (dotted-decimal or DNS) to a network-order IPv4. 0 on failure.
inline uint32_t resolveIpv4(const std::string& host) {
    in_addr a{};
    if (inet_pton(AF_INET, host.c_str(), &a) == 1) return a.s_addr;
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
        uint32_t ip = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(res);
        return ip;
    }
    return 0;
}

inline std::string pickNic(const std::string& explicitNic) {
    if (!explicitNic.empty()) return explicitNic;
    return "eth0";
}

// ── The AF_XDP RX loop ────────────────────────────────────────────────────────
//  filter(const UdpView&) -> bool : keep this datagram?
//  sink(const uint8_t*, uint16_t) : forward the UDP payload (the pumpThreaded
//                                   reader-thread lambda copies it into a slot).
//  Blocks until SIGINT/SIGTERM sets the stop flag, then tears everything down.
template <class Filter, class Sink>
inline void xdpReceiveLoop(const std::string& iface, uint32_t queueId,
                           Filter&& filter, Sink&& sink) {
    if (if_nametoindex(iface.c_str()) == 0) {
        std::cerr << "[xdp] unknown interface \"" << iface << "\"\n";
        return;
    }

    // Lift the locked-memory limit (UMEM is pinned).
    rlimit rl{RLIM_INFINITY, RLIM_INFINITY};
    setrlimit(RLIMIT_MEMLOCK, &rl);

    constexpr uint32_t FRAME_SIZE = XSK_UMEM__DEFAULT_FRAME_SIZE;   // 4096
    constexpr uint32_t NUM_FRAMES = 4096;
    constexpr uint32_t FILL_DESCS = XSK_RING_PROD__DEFAULT_NUM_DESCS; // 2048
    constexpr uint32_t BATCH      = 64;
    const size_t bufSz = size_t(NUM_FRAMES) * FRAME_SIZE;

    void* area = mmap(nullptr, bufSz, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (area == MAP_FAILED) { perror("[xdp] mmap"); return; }

    xsk_umem*       umem = nullptr;
    xsk_ring_prod   fill{};
    xsk_ring_cons   comp{};
    xsk_umem_config ucfg{};
    ucfg.fill_size      = FILL_DESCS;
    ucfg.comp_size      = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    ucfg.frame_size     = FRAME_SIZE;
    ucfg.frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM;
    ucfg.flags          = 0;
    if (int err = xsk_umem__create(&umem, area, bufSz, &fill, &comp, &ucfg)) {
        std::cerr << "[xdp] xsk_umem__create: " << std::strerror(-err) << "\n";
        munmap(area, bufSz);
        return;
    }

    xsk_socket*       xsk = nullptr;
    xsk_ring_cons     rx{};
    xsk_ring_prod     tx{};
    xsk_socket_config scfg{};
    scfg.rx_size      = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    scfg.tx_size      = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    scfg.libbpf_flags = 0;                          // load libxdp default prog
    scfg.xdp_flags    = XDP_FLAGS_UPDATE_IF_NOEXIST;
    scfg.bind_flags   = XDP_USE_NEED_WAKEUP;        // mode auto: zc → drv → skb
    if (int err = xsk_socket__create(&xsk, iface.c_str(), queueId, umem,
                                     &rx, &tx, &scfg)) {
        std::cerr << "[xdp] xsk_socket__create on " << iface << " q" << queueId
                  << ": " << std::strerror(-err)
                  << "  (need root? wrong queue? try: ethtool -L " << iface
                  << " combined 1)\n";
        xsk_umem__delete(umem);
        munmap(area, bufSz);
        return;
    }

    // Seed the fill ring with frames so the kernel has RX buffers.
    uint32_t idx = 0;
    uint32_t reserved = xsk_ring_prod__reserve(&fill, FILL_DESCS, &idx);
    for (uint32_t i = 0; i < reserved; ++i)
        *xsk_ring_prod__fill_addr(&fill, idx++) = uint64_t(i) * FRAME_SIZE;
    xsk_ring_prod__submit(&fill, reserved);

    const int fd = xsk_socket__fd(xsk);
    std::cout << "[xdp] receiving on " << iface << " queue " << queueId
              << " (Ctrl-C to stop)\n";

    auto& stop = xdpStop();
    stop.store(false, std::memory_order_relaxed);

    while (!stop.load(std::memory_order_relaxed)) {
        // Kernel wants a kick to refill; a poll covers both wakeup and wait.
        pollfd pfd{fd, POLLIN, 0};
        poll(&pfd, 1, 200); // 0 timeout: kick and immediately return

        uint32_t idxRx = 0;
        unsigned rcvd = xsk_ring_cons__peek(&rx, BATCH, &idxRx);
        if (rcvd == 0) continue;

        // Reserve the same count in the fill ring to recycle the frames.
        uint32_t idxFill = 0;
        unsigned got = xsk_ring_prod__reserve(&fill, rcvd, &idxFill);
        while (got < rcvd) {
            if (xsk_ring_prod__needs_wakeup(&fill))
                recvfrom(fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
            got = xsk_ring_prod__reserve(&fill, rcvd, &idxFill);
        }

        for (unsigned i = 0; i < rcvd; ++i) {
            const xdp_desc* d = xsk_ring_cons__rx_desc(&rx, idxRx + i);
            const uint64_t  orig = xsk_umem__extract_addr(d->addr);
            const uint64_t  daddr = xsk_umem__add_offset_to_addr(d->addr);
            const uint8_t*  frame =
                static_cast<const uint8_t*>(xsk_umem__get_data(area, daddr));

            UdpView v;
            if (parseUdp(frame, d->len, v) && filter(v))
                sink(v.payload, v.payloadLen);

            *xsk_ring_prod__fill_addr(&fill, idxFill + i) = orig;  // recycle
        }
        xsk_ring_prod__submit(&fill, rcvd);
        xsk_ring_cons__release(&rx, rcvd);
    }

    std::cout << "[xdp] stopped\n";
    xsk_socket__delete(xsk);
    xsk_umem__delete(umem);
    munmap(area, bufSz);
}

} // namespace detail

// ── UdpSource ─────────────────────────────────────────────────────────────────
//  Bind-and-listen analog (unicast or multicast) over AF_XDP.
class UdpSource {
public:
    UdpSource(std::string listenAddr, uint16_t port, std::string nic = "")
        : listenAddr_(std::move(listenAddr)), port_(port),
          nic_(std::move(nic)) {}

    template <class OnPacket>
    void run(OnPacket&& onPacket) {
        readLoop(std::forward<OnPacket>(onPacket));
    }

private:
    template <class Sink>
    void readLoop(Sink&& sink) {
        const std::string nic = detail::pickNic(nic_);

        // Optional dst-IP filter.
        uint32_t wantDstIp = 0;
        bool     haveDstIp = false;
        in_addr  a{};
        if (!listenAddr_.empty() && listenAddr_ != "0.0.0.0" &&
            inet_pton(AF_INET, listenAddr_.c_str(), &a) == 1) {
            wantDstIp = a.s_addr;
            haveDstIp = true;
        }
        const uint16_t wantPort = port_;

        // For multicast we still issue an IGMP join through a normal socket so
        // the switch forwards the group and the NIC accepts it; the XDP program
        // then steals the frames before the kernel UDP stack sees them.
        int joinFd = -1;
        const bool isMcast =
            haveDstIp && ((ntohl(wantDstIp) >> 28) == 0xE);  // 224.0.0.0/4
        if (isMcast) {
            joinFd = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (joinFd >= 0) {
                ip_mreq mreq{};
                mreq.imr_multiaddr.s_addr = wantDstIp;
                mreq.imr_interface.s_addr = INADDR_ANY;
                if (setsockopt(joinFd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                               &mreq, sizeof mreq) < 0)
                    std::cerr << "[xdp] IP_ADD_MEMBERSHIP: "
                              << std::strerror(errno) << "\n";
                std::cout << "[xdp] joined " << listenAddr_ << ":" << port_
                          << " on " << nic << "\n";
            }
        } else {
            std::cout << "[xdp] filtering "
                      << (haveDstIp ? listenAddr_ : std::string("*"))
                      << ":" << port_ << " on " << nic << "\n";
        }

        auto filter = [=](const detail::UdpView& v) {
            if (v.dstPort != wantPort) return false;
            if (haveDstIp && v.dstIp != wantDstIp) return false;
            return true;
        };

        detail::xdpInstallSignals();
        detail::xdpReceiveLoop(nic, /*queueId=*/0, filter,
                               std::forward<Sink>(sink));

        if (joinFd >= 0) ::close(joinFd);
    }

private:
    std::string listenAddr_;
    uint16_t    port_;
    std::string nic_;
};

// ── UdpConnector ──────────────────────────────────────────────────────────────
//  "Connect" analog: accept only datagrams from a specific remote src endpoint.
//  AF_XDP has no kernel connect() filter, so we filter by src IP/port in parse.
class UdpConnector {
public:
    UdpConnector(std::string remoteHost, uint16_t remotePort,
                 uint16_t localPort = 0, std::string nic = "")
        : host_(std::move(remoteHost)), remotePort_(remotePort),
          localPort_(localPort), nic_(std::move(nic)) {}

    template <class OnPacket>
    void run(OnPacket&& onPacket) {
        readLoop(std::forward<OnPacket>(onPacket));
    }

private:
    template <class Sink>
    void readLoop(Sink&& sink) {
        const std::string nic   = detail::pickNic(nic_);
        const uint32_t wantSrc  = detail::resolveIpv4(host_);
        if (wantSrc == 0) {
            std::cerr << "[xdp-conn] cannot resolve \"" << host_ << "\"\n";
            return;
        }
        const uint16_t wantSrcPort = remotePort_;
        const uint16_t wantDstPort = localPort_;   // 0 = don't care

        char ipStr[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &wantSrc, ipStr, sizeof ipStr);
        std::cout << "[xdp-conn] accepting from " << ipStr << ":" << remotePort_
                  << " on " << nic << "\n";

        auto filter = [=](const detail::UdpView& v) {
            if (v.srcIp != wantSrc)        return false;
            if (v.srcPort != wantSrcPort)  return false;
            if (wantDstPort && v.dstPort != wantDstPort) return false;
            return true;
        };

        detail::xdpInstallSignals();
        detail::xdpReceiveLoop(nic, /*queueId=*/0, filter,
                               std::forward<Sink>(sink));
    }

private:
    std::string host_;
    uint16_t    remotePort_;
    uint16_t    localPort_;
    std::string nic_;
};
