// Unit tests for mde::feed::detail parsing helpers in xdpudp.h.
//
// Covers:
//   parseUdp   — Ethernet/IPv4/UDP frame parser
//   pickNic    — NIC name resolution (explicit → env → default)
//   resolveIpv4 — dotted-decimal / DNS → network-order uint32_t
//
// Build (handled by CMakeLists.txt):
//   cmake -B build && cmake --build build --target xdpaeron_tests
//   ctest --test-dir build

#include <arpa/inet.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "xdpudp.h"

using mde::feed::detail::UdpView;
using mde::feed::detail::parseUdp;
using mde::feed::detail::pickNic;
using mde::feed::detail::resolveIpv4;

// ── Minimal test runner ───────────────────────────────────────────────────────
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (cond) {                                                      \
            ++g_pass;                                                    \
        } else {                                                         \
            ++g_fail;                                                    \
            std::fprintf(stderr, "FAIL  %s:%d  %s\n",                   \
                         __FILE__, __LINE__, #cond);                     \
        }                                                                \
    } while (0)

// ── Raw-frame builder helpers ─────────────────────────────────────────────────

// Append a 16-bit big-endian value.
static void push16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x & 0xFFu));
}

// Append a 32-bit big-endian value.
static void push32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >>  8) & 0xFFu));
    v.push_back(static_cast<uint8_t>(x & 0xFFu));
}

// Append a network-order IP address (accepts the same uint32_t that
// inet_addr() returns, i.e. already in network byte order).
static void pushIp(std::vector<uint8_t>& v, uint32_t netOrderIp) {
    uint8_t bytes[4];
    std::memcpy(bytes, &netOrderIp, 4);
    v.insert(v.end(), bytes, bytes + 4);
}

// Build a complete L2 frame: [Ethernet] [opt VLAN] [IPv4] [UDP] [payload].
//   srcIp / dstIp  — network-byte-order values (as returned by inet_addr)
//   ipProto        — IP protocol byte (0x11 = UDP, 0x06 = TCP, …)
//   udpLenOverride — if non-zero, write this value into the UDP length field
//                    instead of the real payload length (for truncation tests)
//   ihl            — IP header length field (normally 5 = 20 bytes)
//   etherType      — outermost EtherType (0x0800 = IPv4, 0x0806 = ARP, …)
//   vlan           — whether to insert an 802.1Q tag
static std::vector<uint8_t> buildFrame(
    uint32_t srcIp, uint32_t dstIp,
    uint16_t srcPort, uint16_t dstPort,
    const std::vector<uint8_t>& payload,
    uint8_t  ipProto          = 0x11,
    uint16_t udpLenOverride   = 0,
    uint8_t  ihl              = 5,
    uint16_t etherType        = 0x0800,
    bool     vlan             = false,
    uint16_t vlanId           = 1)
{
    // Ethernet destination and source MAC addresses.
    std::vector<uint8_t> f;
    f.insert(f.end(), {0x00, 0x01, 0x02, 0x03, 0x04, 0x05}); // dst MAC
    f.insert(f.end(), {0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b}); // src MAC

    if (vlan) {
        push16(f, 0x8100u);  // 802.1Q EtherType
        push16(f, vlanId);   // TCI (PCP=0, DEI=0, VID=vlanId)
    }
    push16(f, etherType);

    // IPv4 header.
    const uint16_t udpPayloadLen = static_cast<uint16_t>(payload.size());
    const uint16_t udpLen        = 8u + udpPayloadLen;
    const uint16_t ipTotal       = static_cast<uint16_t>(ihl * 4u) + udpLen;

    f.push_back(static_cast<uint8_t>((4u << 4) | ihl)); // version=4, IHL
    f.push_back(0x00u);                                  // DSCP/ECN
    push16(f, ipTotal);                                  // total length
    push16(f, 0x0001u);                                  // identification
    push16(f, 0x0000u);                                  // flags + frag offset
    f.push_back(64u);                                    // TTL
    f.push_back(ipProto);                                // protocol
    push16(f, 0x0000u);                                  // checksum (ignored by parser)
    pushIp(f, srcIp);
    pushIp(f, dstIp);

    // Pad IP options area when ihl > 5.
    for (uint8_t i = 5; i < ihl; ++i) {
        push32(f, 0u); // NOP option words
    }

    // UDP header.
    push16(f, srcPort);
    push16(f, dstPort);
    push16(f, udpLenOverride != 0 ? udpLenOverride : udpLen);
    push16(f, 0x0000u); // checksum (ignored)

    // Payload.
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

// ── Test cases ────────────────────────────────────────────────────────────────

static void test_parseUdp_basicValid() {
    // A normal UDP datagram with a 5-byte ASCII payload.
    const uint32_t srcIp   = inet_addr("192.168.1.1");
    const uint32_t dstIp   = inet_addr("239.1.2.3");
    const uint16_t srcPort = 12345;
    const uint16_t dstPort = 50000;
    const std::vector<uint8_t> payload = {'H', 'E', 'L', 'L', 'O'};

    auto frame = buildFrame(srcIp, dstIp, srcPort, dstPort, payload);
    UdpView v{};
    CHECK(parseUdp(frame.data(), static_cast<uint32_t>(frame.size()), v));
    CHECK(v.srcIp      == srcIp);
    CHECK(v.dstIp      == dstIp);
    CHECK(v.srcPort    == srcPort);
    CHECK(v.dstPort    == dstPort);
    CHECK(v.payloadLen == 5u);
    CHECK(v.payload    != nullptr);
    CHECK(std::memcmp(v.payload, payload.data(), 5) == 0);
}

static void test_parseUdp_vlanTagged() {
    // 802.1Q VLAN-tagged frame — the parser must skip the 4-byte tag.
    const uint32_t srcIp   = inet_addr("10.0.0.1");
    const uint32_t dstIp   = inet_addr("10.0.0.2");
    const uint16_t srcPort = 1234;
    const uint16_t dstPort = 5678;
    const std::vector<uint8_t> payload = {0xAB, 0xCD};

    auto frame = buildFrame(srcIp, dstIp, srcPort, dstPort, payload,
                            /*ipProto=*/0x11, /*udpLenOverride=*/0,
                            /*ihl=*/5, /*etherType=*/0x0800,
                            /*vlan=*/true, /*vlanId=*/100);
    UdpView v{};
    CHECK(parseUdp(frame.data(), static_cast<uint32_t>(frame.size()), v));
    CHECK(v.srcIp      == srcIp);
    CHECK(v.dstIp      == dstIp);
    CHECK(v.srcPort    == srcPort);
    CHECK(v.dstPort    == dstPort);
    CHECK(v.payloadLen == 2u);
    CHECK(v.payload != nullptr);
    CHECK(v.payload[0] == 0xAB && v.payload[1] == 0xCD);
}

static void test_parseUdp_ipOptionsIhl6() {
    // IHL=6 (one extra 4-byte option word) — parser uses ihl*4 correctly.
    const uint32_t srcIp   = inet_addr("1.2.3.4");
    const uint32_t dstIp   = inet_addr("5.6.7.8");
    const std::vector<uint8_t> payload = {0x42};

    auto frame = buildFrame(srcIp, dstIp, 111, 222, payload,
                            /*ipProto=*/0x11, 0, /*ihl=*/6);
    UdpView v{};
    CHECK(parseUdp(frame.data(), static_cast<uint32_t>(frame.size()), v));
    CHECK(v.srcPort    == 111);
    CHECK(v.dstPort    == 222);
    CHECK(v.payloadLen == 1u);
    CHECK(v.payload[0] == 0x42);
}

static void test_parseUdp_emptyPayload() {
    // Zero-length UDP payload is legal.
    const std::vector<uint8_t> empty{};
    auto frame = buildFrame(inet_addr("1.1.1.1"), inet_addr("2.2.2.2"),
                            9, 9, empty);
    UdpView v{};
    CHECK(parseUdp(frame.data(), static_cast<uint32_t>(frame.size()), v));
    CHECK(v.payloadLen == 0u);
}

static void test_parseUdp_truncatedPayloadClamped() {
    // The UDP length header claims 20 bytes of payload, but the frame only
    // holds 8.  The parser must clamp payloadLen to the actual bytes present.
    const uint32_t srcIp = inet_addr("10.10.10.10");
    const uint32_t dstIp = inet_addr("10.10.10.20");
    const std::vector<uint8_t> actualPayload(8, 0xFFu);
    // udpLenOverride = 8 (hdr) + 20 (claimed payload)
    auto frame = buildFrame(srcIp, dstIp, 100, 200, actualPayload,
                            0x11, /*udpLenOverride=*/28u);
    UdpView v{};
    CHECK(parseUdp(frame.data(), static_cast<uint32_t>(frame.size()), v));
    CHECK(v.payloadLen == 8u); // clamped to actual
}

static void test_parseUdp_tooShortForEthernetHeader() {
    const std::vector<uint8_t> tiny(13, 0);
    UdpView v{};
    CHECK(!parseUdp(tiny.data(), static_cast<uint32_t>(tiny.size()), v));
}

static void test_parseUdp_tooShortForIpHeader() {
    // Full Ethernet header but only 4 bytes of the IP header.
    std::vector<uint8_t> f(14 + 4, 0);
    // Set EtherType = IPv4 at bytes 12-13.
    f[12] = 0x08; f[13] = 0x00;
    // version+IHL = 0x45.
    f[14] = 0x45;
    UdpView v{};
    CHECK(!parseUdp(f.data(), static_cast<uint32_t>(f.size()), v));
}

static void test_parseUdp_tooShortForUdpHeader() {
    // Ethernet + complete IP + only 4 bytes of UDP header.
    const uint32_t srcIp = inet_addr("1.2.3.4");
    const uint32_t dstIp = inet_addr("5.6.7.8");
    auto frame = buildFrame(srcIp, dstIp, 1, 2, {});
    // Truncate to Ethernet(14) + IP(20) + 4 bytes of UDP (need 8).
    frame.resize(14 + 20 + 4);
    UdpView v{};
    CHECK(!parseUdp(frame.data(), static_cast<uint32_t>(frame.size()), v));
}

static void test_parseUdp_nonIpEthertype() {
    // ARP frame — the parser must reject it (not IPv4).
    auto frame = buildFrame(inet_addr("1.2.3.4"), inet_addr("5.6.7.8"),
                            1, 2, {},
                            0x11, 0, 5, /*etherType=*/0x0806u);
    UdpView v{};
    CHECK(!parseUdp(frame.data(), static_cast<uint32_t>(frame.size()), v));
}

static void test_parseUdp_tcpProtocol() {
    // IPv4 carrying TCP (0x06) — the parser must reject it.
    auto frame = buildFrame(inet_addr("1.2.3.4"), inet_addr("5.6.7.8"),
                            80, 443, {'A'},
                            /*ipProto=*/0x06u);
    UdpView v{};
    CHECK(!parseUdp(frame.data(), static_cast<uint32_t>(frame.size()), v));
}

static void test_parseUdp_ihlTooSmall() {
    // IHL = 4 → header length = 16 bytes < minimum 20; must be rejected.
    auto frame = buildFrame(inet_addr("1.2.3.4"), inet_addr("5.6.7.8"),
                            1, 2, {},
                            0x11, 0, /*ihl=*/4u);
    UdpView v{};
    CHECK(!parseUdp(frame.data(), static_cast<uint32_t>(frame.size()), v));
}

static void test_parseUdp_udpLengthTooSmall() {
    // UDP length field < 8 (the header-only minimum) — must be rejected.
    auto frame = buildFrame(inet_addr("1.2.3.4"), inet_addr("5.6.7.8"),
                            1, 2, {},
                            0x11, /*udpLenOverride=*/7u);
    UdpView v{};
    CHECK(!parseUdp(frame.data(), static_cast<uint32_t>(frame.size()), v));
}

static void test_parseUdp_vlanTooShortForInnerEthertype() {
    // Frame with 0x8100 EtherType but fewer than 4 bytes following it
    // (no room for TCI + inner EtherType).
    std::vector<uint8_t> f;
    f.insert(f.end(), {0,1,2,3,4,5, 6,7,8,9,10,11}); // MACs
    f.push_back(0x81); f.push_back(0x00);              // outer EtherType = 0x8100
    f.push_back(0x00); f.push_back(0x01);              // TCI only (2 bytes) — 4-byte
                                                       // minimum not met
    // Only 2 bytes after offset 14, need 4.
    UdpView v{};
    CHECK(!parseUdp(f.data(), static_cast<uint32_t>(f.size()), v));
}

static void test_parseUdp_payloadPointerValidity() {
    // Verify that payload pointer actually points inside the frame.
    const std::vector<uint8_t> payload = {1, 2, 3};
    auto frame = buildFrame(inet_addr("10.0.0.1"), inet_addr("10.0.0.2"),
                            5000, 6000, payload);
    UdpView v{};
    CHECK(parseUdp(frame.data(), static_cast<uint32_t>(frame.size()), v));
    // Payload must lie within the frame buffer.
    CHECK(v.payload >= frame.data());
    CHECK(v.payload + v.payloadLen <= frame.data() + frame.size());
}

// ── pickNic tests ─────────────────────────────────────────────────────────────

static void test_pickNic_explicit() {
    CHECK(mde::feed::detail::pickNic("enp3s0") == "enp3s0");
}

static void test_pickNic_envVar() {
    ::setenv("MDE_XDP_IFACE", "bond0", 1);
    CHECK(mde::feed::detail::pickNic("") == "bond0");
    ::unsetenv("MDE_XDP_IFACE");
}

static void test_pickNic_default() {
    ::unsetenv("MDE_XDP_IFACE");
    CHECK(mde::feed::detail::pickNic("") == "eth0");
}

static void test_pickNic_explicitOverridesEnv() {
    ::setenv("MDE_XDP_IFACE", "bond0", 1);
    CHECK(mde::feed::detail::pickNic("eth1") == "eth1");
    ::unsetenv("MDE_XDP_IFACE");
}

// ── resolveIpv4 tests ─────────────────────────────────────────────────────────

static void test_resolveIpv4_dottedDecimal() {
    const uint32_t got = resolveIpv4("192.168.100.200");
    CHECK(got == inet_addr("192.168.100.200"));
}

static void test_resolveIpv4_loopback() {
    CHECK(resolveIpv4("127.0.0.1") == inet_addr("127.0.0.1"));
}

static void test_resolveIpv4_broadcast() {
    CHECK(resolveIpv4("255.255.255.255") == inet_addr("255.255.255.255"));
}

static void test_resolveIpv4_invalidOctet() {
    // "256.0.0.0" is not a valid IPv4 dotted-decimal and DNS will not resolve
    // it, so the result must be 0.
    CHECK(resolveIpv4("256.0.0.0") == 0u);
}

static void test_resolveIpv4_emptyString() {
    CHECK(resolveIpv4("") == 0u);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    // parseUdp
    test_parseUdp_basicValid();
    test_parseUdp_vlanTagged();
    test_parseUdp_ipOptionsIhl6();
    test_parseUdp_emptyPayload();
    test_parseUdp_truncatedPayloadClamped();
    test_parseUdp_tooShortForEthernetHeader();
    test_parseUdp_tooShortForIpHeader();
    test_parseUdp_tooShortForUdpHeader();
    test_parseUdp_nonIpEthertype();
    test_parseUdp_tcpProtocol();
    test_parseUdp_ihlTooSmall();
    test_parseUdp_udpLengthTooSmall();
    test_parseUdp_vlanTooShortForInnerEthertype();
    test_parseUdp_payloadPointerValidity();

    // pickNic
    test_pickNic_explicit();
    test_pickNic_envVar();
    test_pickNic_default();
    test_pickNic_explicitOverridesEnv();

    // resolveIpv4
    test_resolveIpv4_dottedDecimal();
    test_resolveIpv4_loopback();
    test_resolveIpv4_broadcast();
    test_resolveIpv4_invalidOctet();
    test_resolveIpv4_emptyString();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
