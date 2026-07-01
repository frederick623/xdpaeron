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
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <poll.h>
#include <sys/ioctl.h>
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

// ── TX helpers ────────────────────────────────────────────────────────────────

// Return the hardware (MAC) address of a local interface.
// mac must point to a 6-byte buffer.  Returns true on success.
inline bool getMacAddr(const std::string& iface, uint8_t mac[6]) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    const bool ok = (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0);
    if (ok) std::memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    ::close(fd);
    return ok;
}

// Return the primary IPv4 address of a local interface in network byte order.
// Returns 0 on failure.
inline uint32_t getIfaceIpv4(const std::string& iface) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    uint32_t ip = 0;
    if (ioctl(fd, SIOCGIFADDR, &ifr) == 0)
        ip = reinterpret_cast<const sockaddr_in*>(&ifr.ifr_addr)->sin_addr.s_addr;
    ::close(fd);
    return ip;
}

// Look up the MAC address for a given network-order IPv4 address in the
// kernel ARP table (/proc/net/arp).  Only returns entries that are complete
// (ATF_COM = 0x2).  Returns true and fills mac[6] on success.
inline bool lookupArpMac(uint32_t netOrderIp, uint8_t mac[6]) {
    char ipStr[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &netOrderIp, ipStr, sizeof ipStr);

    FILE* f = std::fopen("/proc/net/arp", "r");
    if (!f) return false;

    char line[256];
    std::fgets(line, sizeof line, f);           // skip header line
    while (std::fgets(line, sizeof line, f)) {
        char addr[32]{}, hwAddr[32]{};
        int  hwType = 0, flags = 0;
        if (std::sscanf(line, "%31s %x %x %31s", addr, &hwType, &flags, hwAddr) != 4)
            continue;
        if (std::strcmp(addr, ipStr) != 0 || !(flags & 0x2)) // ATF_COM
            continue;
        unsigned m[6]{};
        if (std::sscanf(hwAddr, "%02x:%02x:%02x:%02x:%02x:%02x",
                        &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
            for (int i = 0; i < 6; ++i) mac[i] = static_cast<uint8_t>(m[i]);
            std::fclose(f);
            return true;
        }
    }
    std::fclose(f);
    return false;
}

// Compute the one's-complement checksum over hdrLen bytes (must be even).
// Reads bytes in big-endian (network) order — endian-neutral implementation.
// The checksum field in the header should be zeroed before calling.
// Returns the checksum in host byte order; store with the high byte first
// (network order) by writing cksum>>8 then cksum&0xFF.
// Verification: recompute with the stored checksum included — returns 0x0000
// when the header is valid.
inline uint16_t ipv4Checksum(const void* header, std::size_t hdrLen) {
    const auto* b = static_cast<const uint8_t*>(header);
    uint32_t sum = 0;
    for (std::size_t i = 0; i + 1 < hdrLen; i += 2)
        sum += (uint32_t(b[i]) << 8) | b[i + 1];
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

// Build a complete L2 (Ethernet/IPv4/UDP) TX frame from a raw payload.
// srcIp / dstIp are in network byte order.
// srcPort / dstPort are in host byte order.
// Returns the assembled frame as a byte vector.
inline std::vector<uint8_t> buildTxFrame(
    const uint8_t srcMac[6], const uint8_t dstMac[6],
    uint32_t srcIp, uint32_t dstIp,
    uint16_t srcPort, uint16_t dstPort,
    const uint8_t* payload, uint16_t payloadLen)
{
    const uint16_t udpLen   = static_cast<uint16_t>(8u + payloadLen);
    const uint16_t ipTotal  = static_cast<uint16_t>(20u + udpLen);

    std::vector<uint8_t> f;
    f.reserve(14u + 20u + 8u + payloadLen);

    // Ethernet header (14 bytes)
    f.insert(f.end(), dstMac, dstMac + 6);
    f.insert(f.end(), srcMac, srcMac + 6);
    f.push_back(0x08); f.push_back(0x00);  // EtherType = IPv4

    // IPv4 header (20 bytes, no options)
    const std::size_t ipOff = f.size();
    f.push_back(0x45u);                                    // version=4, IHL=5
    f.push_back(0x00u);                                    // DSCP/ECN
    f.push_back(static_cast<uint8_t>(ipTotal >> 8));
    f.push_back(static_cast<uint8_t>(ipTotal & 0xFFu));
    f.push_back(0x00u); f.push_back(0x01u);               // identification
    f.push_back(0x40u); f.push_back(0x00u);               // flags=DF, frag=0
    f.push_back(64u);                                      // TTL
    f.push_back(0x11u);                                    // protocol = UDP
    f.push_back(0x00u); f.push_back(0x00u);               // checksum (filled below)
    const auto* srcBytes = reinterpret_cast<const uint8_t*>(&srcIp);
    const auto* dstBytes = reinterpret_cast<const uint8_t*>(&dstIp);
    f.insert(f.end(), srcBytes, srcBytes + 4);
    f.insert(f.end(), dstBytes, dstBytes + 4);

    // Fill in IPv4 checksum over the 20-byte header (stored in network byte order).
    const uint16_t cksum = ipv4Checksum(f.data() + ipOff, 20u);
    f[ipOff + 10] = static_cast<uint8_t>(cksum >> 8);
    f[ipOff + 11] = static_cast<uint8_t>(cksum & 0xFFu);

    // UDP header (8 bytes); leave checksum as 0 (optional in IPv4)
    f.push_back(static_cast<uint8_t>(srcPort >> 8));
    f.push_back(static_cast<uint8_t>(srcPort & 0xFFu));
    f.push_back(static_cast<uint8_t>(dstPort >> 8));
    f.push_back(static_cast<uint8_t>(dstPort & 0xFFu));
    f.push_back(static_cast<uint8_t>(udpLen >> 8));
    f.push_back(static_cast<uint8_t>(udpLen & 0xFFu));
    f.push_back(0x00u); f.push_back(0x00u);               // checksum disabled

    // Payload
    f.insert(f.end(), payload, payload + payloadLen);
    return f;
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

// ── UdpSink ───────────────────────────────────────────────────────────────────
//  Reverse path: accept a UDP payload and transmit it as a raw UDP datagram
//  via the AF_XDP TX ring (kernel-bypass egress).
//
//  Constructor resolves remote IP, reads the local interface MAC/IP, looks up
//  the ARP table for the destination MAC (falls back to a broadcast MAC with a
//  warning if not found), sets up UMEM + XSK TX ring, and is then ready for
//  repeated send() calls from any thread.
//
//  Usage:
//      UdpSink sink("192.168.1.10", 9000, /*srcPort=*/9001, "eth0");
//      sink.send(data, len);   // called e.g. from AeronIpcSource callback
class UdpSink {
public:
    // remoteHost  — destination IP (dotted-decimal or hostname)
    // remotePort  — destination UDP port (host byte order)
    // srcPort     — source UDP port (0 = use remotePort as source too)
    // nic         — NIC interface name; defaults to "eth0"
    UdpSink(std::string remoteHost, uint16_t remotePort,
            uint16_t srcPort = 0, std::string nic = "")
        : remoteHost_(std::move(remoteHost)), remotePort_(remotePort),
          srcPort_(srcPort != 0 ? srcPort : remotePort),
          nic_(detail::pickNic(nic)) {
        setup();
    }

    ~UdpSink() { teardown(); }

    // Not copyable/movable (manages OS resources).
    UdpSink(const UdpSink&)            = delete;
    UdpSink& operator=(const UdpSink&) = delete;

    // Transmit one UDP datagram via the AF_XDP TX ring.
    // Returns true if the frame was queued; false on error (no free buffers,
    // socket not set up, etc.).
    bool send(const uint8_t* data, uint16_t len) {
        if (!xsk_ || len > maxPayload_) return false;

        // Drain the completion ring to recover buffers sent by the kernel.
        drainComp();

        if (freeFrames_.empty()) {
            std::cerr << "[xdp-sink] TX ring full; dropping frame\n";
            return false;
        }

        // Build the full L2 frame.
        const std::vector<uint8_t> frame =
            detail::buildTxFrame(srcMac_, dstMac_, srcIp_, dstIp_,
                                 srcPort_, remotePort_, data, len);

        const uint64_t addr = freeFrames_.back();
        freeFrames_.pop_back();

        // Copy frame into UMEM.
        std::memcpy(static_cast<uint8_t*>(area_) + addr,
                    frame.data(), frame.size());

        // Post to TX ring.
        uint32_t idx = 0;
        if (xsk_ring_prod__reserve(&tx_, 1, &idx) == 0) {
            freeFrames_.push_back(addr);  // return buffer
            std::cerr << "[xdp-sink] TX ring reserve failed\n";
            return false;
        }
        xdp_desc* desc = xsk_ring_prod__tx_desc(&tx_, idx);
        desc->addr = addr;
        desc->len  = static_cast<uint32_t>(frame.size());
        xsk_ring_prod__submit(&tx_, 1);

        // Kick the kernel if required (XDP_USE_NEED_WAKEUP).
        if (xsk_ring_prod__needs_wakeup(&tx_)) {
            const int fd = xsk_socket__fd(xsk_);
            sendto(fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
        }
        return true;
    }

private:
    // ── AF_XDP TX setup ──────────────────────────────────────────────────────
    static constexpr uint32_t FRAME_SIZE = XSK_UMEM__DEFAULT_FRAME_SIZE;
    static constexpr uint32_t NUM_FRAMES = 4096;
    // Maximum UDP payload that fits in one AF_XDP frame.
    static constexpr uint16_t maxPayload_ =
        static_cast<uint16_t>(FRAME_SIZE - 14u - 20u - 8u);

    void setup() {
        // Resolve destination IP.
        dstIp_ = detail::resolveIpv4(remoteHost_);
        if (dstIp_ == 0) {
            std::cerr << "[xdp-sink] cannot resolve \"" << remoteHost_ << "\"\n";
            return;
        }

        // Local interface MAC and IP.
        if (!detail::getMacAddr(nic_, srcMac_)) {
            std::cerr << "[xdp-sink] cannot get MAC of \"" << nic_ << "\"\n";
            return;
        }
        srcIp_ = detail::getIfaceIpv4(nic_);
        if (srcIp_ == 0) {
            std::cerr << "[xdp-sink] cannot get IP of \"" << nic_ << "\"\n";
            return;
        }

        // Destination MAC via ARP table; fall back to broadcast.
        if (!detail::lookupArpMac(dstIp_, dstMac_)) {
            char ipStr[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &dstIp_, ipStr, sizeof ipStr);
            std::cerr << "[xdp-sink] ARP miss for " << ipStr
                      << " — using broadcast MAC\n";
            std::memset(dstMac_, 0xFF, 6);
        }

        // Lift locked-memory limit (UMEM is pinned).
        rlimit rl{RLIM_INFINITY, RLIM_INFINITY};
        setrlimit(RLIMIT_MEMLOCK, &rl);

        const std::size_t bufSz = std::size_t(NUM_FRAMES) * FRAME_SIZE;
        area_ = mmap(nullptr, bufSz, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (area_ == MAP_FAILED) { perror("[xdp-sink] mmap"); area_ = nullptr; return; }
        bufSz_ = bufSz;

        xsk_umem_config ucfg{};
        ucfg.fill_size      = XSK_RING_PROD__DEFAULT_NUM_DESCS;
        ucfg.comp_size      = XSK_RING_CONS__DEFAULT_NUM_DESCS;
        ucfg.frame_size     = FRAME_SIZE;
        ucfg.frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM;
        if (int err = xsk_umem__create(&umem_, area_, bufSz,
                                       &fill_, &comp_, &ucfg)) {
            std::cerr << "[xdp-sink] xsk_umem__create: "
                      << std::strerror(-err) << "\n";
            return;
        }

        xsk_socket_config scfg{};
        scfg.rx_size      = 0;   // TX-only socket: no RX ring
        scfg.tx_size      = XSK_RING_PROD__DEFAULT_NUM_DESCS;
        scfg.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD; // no XDP prog needed for TX
        scfg.xdp_flags    = XDP_FLAGS_UPDATE_IF_NOEXIST;
        scfg.bind_flags   = XDP_USE_NEED_WAKEUP;
        if (int err = xsk_socket__create(&xsk_, nic_.c_str(), /*queueId=*/0,
                                         umem_, nullptr, &tx_, &scfg)) {
            std::cerr << "[xdp-sink] xsk_socket__create on " << nic_
                      << ": " << std::strerror(-err) << "\n";
            return;
        }

        // Populate the free-frame list (all frames are available for TX).
        freeFrames_.reserve(NUM_FRAMES);
        for (uint32_t i = 0; i < NUM_FRAMES; ++i)
            freeFrames_.push_back(uint64_t(i) * FRAME_SIZE);

        char srcIpStr[INET_ADDRSTRLEN]{}, dstIpStr[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &srcIp_, srcIpStr, sizeof srcIpStr);
        inet_ntop(AF_INET, &dstIp_, dstIpStr, sizeof dstIpStr);
        std::cout << "[xdp-sink] " << srcIpStr << ":" << srcPort_
                  << " -> " << dstIpStr << ":" << remotePort_
                  << " via " << nic_ << "\n";
    }

    void teardown() {
        if (xsk_)  { xsk_socket__delete(xsk_);  xsk_  = nullptr; }
        if (umem_) { xsk_umem__delete(umem_);   umem_ = nullptr; }
        if (area_ && bufSz_) {
            munmap(area_, bufSz_);
            area_ = nullptr; bufSz_ = 0;
        }
    }

    // Return completed TX buffers to the free list.
    void drainComp() {
        constexpr uint32_t BATCH = 64;
        uint32_t idx = 0;
        const uint32_t done = xsk_ring_cons__peek(&comp_, BATCH, &idx);
        for (uint32_t i = 0; i < done; ++i)
            freeFrames_.push_back(*xsk_ring_cons__comp_addr(&comp_, idx + i));
        if (done) xsk_ring_cons__release(&comp_, done);
    }

    // ── Member data ──────────────────────────────────────────────────────────
    std::string  remoteHost_;
    uint16_t     remotePort_;
    uint16_t     srcPort_;
    std::string  nic_;

    void*        area_  = nullptr;
    std::size_t  bufSz_ = 0;
    xsk_umem*    umem_  = nullptr;
    xsk_socket*  xsk_   = nullptr;
    xsk_ring_prod fill_{};
    xsk_ring_cons comp_{};
    xsk_ring_prod tx_{};

    uint32_t     srcIp_    = 0;
    uint32_t     dstIp_    = 0;
    uint8_t      srcMac_[6]{};
    uint8_t      dstMac_[6]{};

    std::vector<uint64_t> freeFrames_;
};
