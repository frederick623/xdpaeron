# xdpio

A high-performance UDP↔Aeron bridge built on **AF_XDP** (Linux kernel-bypass networking). It can operate in two directions:

- **RX path** — receives raw UDP datagrams directly from a NIC queue via AF_XDP and publishes them on an [Aeron](https://github.com/real-logic/aeron) IPC channel.
- **TX path** — subscribes to an Aeron channel and transmits each message as a raw UDP datagram via the AF_XDP TX ring (the reverse path).

Both paths bypass the kernel TCP/IP stack entirely, achieving near-wire-rate latency.

---

## Overview

### RX path (UDP → Aeron)

```
NIC (hardware queue)
       │  AF_XDP (zero-copy / copy mode)
       ▼
  UdpSource / UdpConnector          ← xdpudp.h
  (filter by IP + UDP port)
       │  UDP payload bytes
       ▼
  AeronIpcPublisher                 ← aeron.h
  (Aeron channel + stream ID)
       │
       ▼
  Aeron subscribers (any process)
```

### TX path (Aeron → UDP) — reverse direction

```
  Aeron publishers (any process)
       │
       ▼
  AeronIpcSource                    ← aeron.h
  (Aeron channel + stream ID)
       │  message bytes
       ▼
  UdpSink                           ← xdpudp.h
  (build raw Ethernet/IPv4/UDP frame,
   transmit via AF_XDP TX ring)
       │  AF_XDP TX (zero-copy / copy mode)
       ▼
  NIC (hardware queue) → network
```

The main entry points wire these components together as standalone executables:

| Executable | Path |
|------------|------|
| `xdp_udp_relayer` (`relayer.cpp`) | UDP → Aeron |
| `aeron_xdp_forwarder` (`forwarder.cpp`) | Aeron → UDP |

Both components are also available as reusable C++ classes for use in larger applications.

---

## Repository Structure

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | CMake build configuration (C++20). Detects optional dependencies (Aeron, libxdp, libbpf) and enables features accordingly. |
| `relayer.cpp` | `main()` for the `xdp_udp_relayer` executable. Wires `UdpSource` → `AeronIpcPublisher` (RX path). |
| `forwarder.cpp` | `main()` for the `aeron_xdp_forwarder` executable. Wires `AeronIpcSource` → `UdpSink` (TX path). |
| `xdpudp.h` | `UdpSource`, `UdpConnector`, and `UdpSink` — AF_XDP RX/TX loops, frame parsing and building (Ethernet/VLAN/IPv4/UDP), IP/port filtering. Linux-only. |
| `aeron.h` | `AeronIpcPublisher` and `AeronIpcSource` — thin wrappers around the Aeron C++ client. Falls back to no-op stubs when the Aeron library is not present. |
| `LICENSE` | Project license. |

The code uses the global namespace and is part of a larger market-data / trading-engine ecosystem.

---

## Key Technologies

### AF_XDP (eXpress Data Path)
AF_XDP is a Linux socket type that maps NIC RX queue descriptors directly into user-space memory (UMEM), bypassing the kernel network stack. This eliminates per-packet syscall overhead and achieves near-wire-rate packet processing. **libxdp** (`xdp/xsk.h`) and **libbpf** are the userspace libraries used to create and manage these sockets.

- Supports zero-copy mode (requires NIC driver support) and falls back to copy/SKB mode automatically.
- The XDP program itself is libxdp's built-in default — no custom BPF object file needs to be shipped.
- Multicast groups are joined via a normal `SOCK_DGRAM` socket (for IGMP), while AF_XDP intercepts the actual frames before the kernel UDP stack processes them.

### Aeron
[Aeron](https://github.com/real-logic/aeron) is a high-performance message-transport library designed for low-latency IPC and network messaging. This project uses the **Aeron C++ client** to publish messages to a channel/stream. Common channels include `aeron:ipc` (shared-memory IPC on the same host).

### CMake (C++20)
The build system uses CMake with C++20. Feature availability is detected at configure time:

- If the **Aeron C++ client** is found: real publisher/subscriber classes are compiled; otherwise stubs are used.
- If **libxdp**, **libbpf**, and **Aeron** are all present on Linux: the `xdp_udp_relayer` executable is built.

---

## Building

### Prerequisites (Ubuntu 24.04)

```bash
sudo apt install libxdp-dev libbpf-dev
# Install the Aeron C++ client (build from source or use a package)
```

### Configure and build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

If dependencies are missing, CMake will print status messages indicating which features were disabled.

---

## Running the Relayer (UDP → Aeron)

```
Usage: xdp_udp_relayer <listen_addr> <listen_port> <aeron_channel> <aeron_stream_id> [xdp_iface]
```

**Example — receive a multicast feed and publish over Aeron IPC:**

```bash
sudo ./build/xdp_udp_relayer 239.1.2.3 50000 aeron:ipc 1001 eth0
```

| Argument | Description |
|----------|-------------|
| `listen_addr` | Destination IP to filter on (unicast or multicast). Use `0.0.0.0` for any. |
| `listen_port` | UDP destination port to filter on. |
| `aeron_channel` | Aeron channel URI (e.g. `aeron:ipc`, `aeron:udp?endpoint=localhost:20121`). |
| `aeron_stream_id` | Aeron stream ID (positive 32-bit integer). |
| `xdp_iface` | (Optional) NIC interface name. Defaults to `eth0`. |

---

## Running the Forwarder (Aeron → UDP)

```
Usage: aeron_xdp_forwarder <aeron_channel> <aeron_stream_id> <dest_addr> <dest_port> [src_port] [xdp_iface]
```

**Example — consume from Aeron IPC and send UDP to a remote host:**

```bash
sudo ./build/aeron_xdp_forwarder aeron:ipc 1001 192.168.1.10 9000 9001 eth0
```

| Argument | Description |
|----------|-------------|
| `aeron_channel` | Aeron channel URI to subscribe to. |
| `aeron_stream_id` | Aeron stream ID (positive 32-bit integer). |
| `dest_addr` | Destination IP address (dotted-decimal). |
| `dest_port` | Destination UDP port (host byte order). |
| `src_port` | (Optional) Source UDP port. Defaults to `dest_port`. |
| `xdp_iface` | (Optional) NIC interface name. Defaults to `eth0`. |

The forwarder looks up the destination MAC address in the kernel ARP table and falls back to a broadcast MAC if no ARP entry is found.

---

## UDP Multicast Support

The **relayer** (UDP → Aeron) fully supports receiving **UDP multicast packets** via AF_XDP. When you specify a multicast group address in `listen_addr`, the relayer automatically:

1. **Sends an IGMP join** — registers the multicast group with the network stack so the switch/router forwards traffic to the interface.
2. **Intercepts frames at the NIC** — the XDP program captures incoming Ethernet frames *before* the kernel UDP stack processes them, avoiding kernel overhead.
3. **Filters by multicast IP and UDP port** — only matching datagrams are forwarded to Aeron.

### Example — receive a multicast feed

```bash
# Listen on multicast group 239.1.2.3, port 50000, and publish to Aeron IPC stream 1001
sudo ./build/xdp_udp_relayer 239.1.2.3 50000 aeron:ipc 1001 eth0
```

### Using multicast in C++

```cpp
// Bind to multicast group and process each datagram
UdpSource src("239.1.2.3", 50000, "eth0");
src.run([](const uint8_t* data, uint16_t len) {
    // Called for each multicast UDP datagram received
    std::cout << "Received " << len << " bytes\n";
});
```

Multicast groups use addresses in the `224.0.0.0/4` range. See [IANA Multicast Address Allocations](https://www.iana.org/assignments/multicast-addresses/multicast-addresses.xhtml) for standard group assignments.

---

## Class Reference

### `UdpSource` (`xdpudp.h`)

Binds AF_XDP to a NIC queue and delivers UDP payloads matching a destination IP/port filter.

```cpp
UdpSource src("239.1.2.3", 50000, "eth0");
src.run([](const uint8_t* data, uint16_t len) {
    // called on the consumer thread for each matching datagram
});
```

### `UdpConnector` (`xdpudp.h`)

Like `UdpSource` but filters by *source* IP/port (emulates a "connected" UDP socket).

```cpp
UdpConnector conn("192.168.1.10", 9000, /*localPort=*/0);
conn.run([](const uint8_t* data, uint16_t len) { ... });
```

### `AeronIpcPublisher` (`aeron.h`)

Connects to an Aeron media driver and publishes byte buffers on a channel/stream.

```cpp
AeronIpcPublisher pub("aeron:ipc", 1001);
pub.publish(data, len);   // returns false on backpressure
```

### `AeronIpcSource` (`aeron.h`)

Subscribes to an Aeron channel/stream and delivers messages via callback.

```cpp
AeronIpcSource src("aeron:ipc", 1001);
src.run([](const uint8_t* data, uint16_t len) { ... });
```

### `UdpSink` (`xdpudp.h`)

Transmits UDP datagrams via the AF_XDP TX ring (kernel-bypass egress). Resolves the destination IP, reads the local interface MAC/IP via `ioctl`, looks up the destination MAC in the kernel ARP table (falls back to broadcast on ARP miss), and builds complete Ethernet/IPv4/UDP frames. Typically used as the target of an `AeronIpcSource` callback.

```cpp
UdpSink sink("192.168.1.10", /*dstPort=*/9000, /*srcPort=*/9001, "eth0");

AeronIpcSource src("aeron:ipc", 1001);
src.run([&](const uint8_t* data, uint16_t len) {
    sink.send(data, len);   // returns false if TX ring is full
});
```

> **Privileges required for both executables:** `CAP_NET_RAW`, `CAP_BPF`, `CAP_SYS_ADMIN` (or simply run with `sudo`).

**Recommended — pin the NIC to a single queue** so all traffic lands on queue 0:

```bash
sudo ethtool -L eth0 combined 1
```

Press `Ctrl-C` or send `SIGTERM` to stop gracefully.

---

## Threading Model

Both `UdpSource`/`UdpConnector` and `AeronIpcSource` deliver payloads directly via callback:

- The **reader thread** runs the AF_XDP or Aeron poll loop and invokes the user-supplied `onPacket` callback directly.
- This approach minimizes latency by eliminating intermediate queue buffering.

High-performance applications can offload heavy consumer logic to a separate thread if needed.

---

## Benchmarks

Benchmarks require [Google Benchmark](https://github.com/google/benchmark). Install it with:

```bash
sudo apt install libbenchmark-dev
```

### UDP path benchmark (`benchmarks/bench_udp_path.cpp`)

Compares AF_XDP user-space parsing against ordinary kernel UDP receive.

**Build and run:**

```bash
cmake -S . -B build && cmake --build build --target xdpio_bench
./build/xdpio_bench [--benchmark_format=json] [--benchmark_filter=...]
```

**Sample output:**

```
root@30874c415336:/ebpf/xdpio# ./build/xdpio_bench
2026-06-28T11:20:37+00:00
Running ./build/xdpio_bench
Run on (2 X 48 MHz CPU s)
Load Average: 0.00, 0.02, 0.00
***WARNING*** Library was built as DEBUG. Timings may be affected.
------------------------------------------------------------------------------------
Benchmark                          Time             CPU   Iterations UserCounters...
------------------------------------------------------------------------------------
BM_XdpParseAndFilter/64         1.20 ns         1.20 ns    525166575 bytes_per_second=82.3026Gi/s xdp-parse+filter
BM_XdpParseAndFilter/512        1.22 ns         1.22 ns    575616281 bytes_per_second=424.536Gi/s xdp-parse+filter
BM_XdpParseAndFilter/1400       1.21 ns         1.21 ns    579316939 bytes_per_second=1.0844Ti/s xdp-parse+filter
BM_UdpKernelRecv/64              819 ns          477 ns      1456544 bytes_per_second=127.848Mi/s kernel-udp-recv
BM_UdpKernelRecv/512            1068 ns          548 ns      1283269 bytes_per_second=891.097Mi/s kernel-udp-recv
BM_UdpKernelRecv/1400           1296 ns          624 ns      1113067 bytes_per_second=2.08808Gi/s kernel-udp-recv
```

**Benchmark descriptions:**

| Benchmark | What it measures |
|-----------|-----------------|
| `BM_XdpParseAndFilter` | Pure userspace cost of `parseUdp()` + the filter closure on a pre-built in-memory frame. **No syscall involved.** Models the hot path after AF_XDP delivers a frame pointer from the UMEM ring. |
| `BM_UdpKernelRecv` | One `recvfrom()` call on a loopback `SOCK_DGRAM` socket. A background sender thread pre-fills the socket receive buffer so the benchmark measures steady-state kernel delivery cost, not idle wakeup latency. |

Both benchmarks sweep payload sizes of **64, 512, and 1400 bytes**. The AF_XDP parse+filter time is essentially payload-independent (~1.2 ns) because the parser only touches the fixed-size headers; the kernel path grows with payload size due to copy costs. The gap (~400–500×) represents the syscall + kernel networking stack overhead that AF_XDP eliminates.

---

### Shared-memory path benchmark (`benchmarks/bench_shm_path.cpp`)

Benchmarks the shared-memory IPC receive path and the full UDP → shared-memory → consumer pipeline.

**Build and run:**

```bash
cmake -S . -B build && cmake --build build --target xdpio_bench_shm
./build/xdpio_bench_shm [--benchmark_format=json] [--benchmark_filter=...]
```

**Benchmark descriptions:**

| Benchmark | What it measures |
|-----------|-----------------|
| `BM_ShmIpcPublish` | Isolated `ShmIpcPublisher::publish()` throughput. A background drain thread keeps the ring continuously empty so the publisher never encounters back-pressure; isolates the write-side cost (memcpy + atomic store). |
| `BM_ShmIpcRecv` | Isolated shared-memory consumer throughput. A background producer keeps the ring saturated; each iteration busy-polls one slot, copies the payload, and releases the slot. Includes cross-CPU cache-line transfer latency since producer and consumer run on separate threads. |
| `BM_UdpKernelPlusShmRecv` | Full pipeline: a background sender saturates a loopback UDP socket; a background relayer thread calls `recvfrom()` then `publish()` (mirroring `shm_udp_relayer`'s hot path); each benchmark iteration drains one shm slot. Measures end-to-end throughput through the entire kernel UDP + shared-memory stack. |

All three benchmarks sweep payload sizes of **64, 512, and 1400 bytes**.

---

## AF_XDP Internals

This section documents the internal mechanics of the RX and TX paths implemented in `xdpudp.h`.

### UMEM and socket setup

Both `UdpSource`/`UdpConnector` (RX) and `UdpSink` (TX) begin by allocating a **UMEM**: a contiguous slab of anonymous pages mapped with `mmap` that is shared between userspace and the kernel.

| Parameter | Value | Meaning |
|-----------|-------|---------|
| `FRAME_SIZE` | 4096 bytes | One UMEM slot = one Ethernet frame |
| `NUM_FRAMES` | 4096 | Total slots → 16 MB ring buffer |
| `FILL_DESCS` | 2048 | Fill ring capacity (how many free slots the kernel can be offered at once) |
| `XDP_USE_NEED_WAKEUP` | bind flag | Avoids spinning: kernel signals when a wakeup kick is needed |
| `XDP_FLAGS_UPDATE_IF_NOEXIST` | XDP flag | Attaches libxdp's built-in redirect program; no custom BPF `.o` to ship |

Mode selection is automatic: libxdp tries **zero-copy** (requires NIC driver support) first, falls back to **driver copy** mode, then to **SKB/copy** mode. On virtual NICs (e.g. Colima's virtio) only SKB mode is available; functionally correct but without the true zero-copy latency benefit of real hardware.

Before the RX loop starts, all `FILL_DESCS` UMEM frame addresses are written into the **fill ring** so the kernel immediately has a pool of receive buffers to write into.

For the TX-only `UdpSink`, `rx_size = 0` and `XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD` suppresses the XDP BPF program load (not needed for TX).

### RX poll loop

```
while not stopped:
    poll(fd, POLLIN, 200 ms)            ← kick kernel + wait for frames
    rcvd = peek RX ring (batch ≤ 64)
    if rcvd == 0: continue

    reserve rcvd slots in fill ring     ← pre-allocate recycle slots

    for each RX descriptor:
        frame = UMEM[desc.addr]         ← zero-copy pointer into shared memory
        if parseUdp(frame, len, view) and filter(view):
            sink(view.payload, view.payloadLen)
        fill_ring[i] = orig_addr        ← return slot to kernel

    submit fill ring additions
    release RX consumer ring
```

The `poll()` call with a 200 ms timeout serves two purposes: it delivers the wakeup signal the kernel needs (when `XDP_USE_NEED_WAKEUP` is set on the fill ring) and it blocks instead of busy-spinning when the queue is idle. Fill ring slots must be reserved *before* iterating over the batch; if `reserve` stalls, a `recvfrom(..., MSG_DONTWAIT)` kick is issued and the reserve is retried.

### Frame parsing — `parseUdp()`

`parseUdp()` is a zero-allocation, bounds-checked parser over the raw frame bytes in UMEM.

| Layer | Checks performed |
|-------|-----------------|
| **Ethernet** (14 B) | `len >= 14`; reads `ether_type` |
| **802.1Q VLAN** (optional +4 B) | If EtherType = `0x8100`: `len >= offset + 4`, skip TCI, read inner EtherType |
| **IPv4** (≥ 20 B) | EtherType = `0x0800`; `version == 4`; `protocol == 0x11` (UDP); `ihl >= 5`; `len >= offset + ihl*4` |
| **UDP** (8 B) | `len >= udp_offset + 8`; `udp.len >= 8` |
| **Payload** | `payloadLen = udp.len - 8`; clamped to actual frame bytes if the UDP length field overstates (truncation-safe) |

The function fills a `UdpView` struct: `srcIp`/`dstIp` in network byte order, `srcPort`/`dstPort` in host byte order, `payload` as a pointer directly into the UMEM frame, and `payloadLen`. All returned pointers are into the original frame buffer — no copies are made.

### TX path — `UdpSink`

`UdpSink` resolves all frame-header fields once at construction time:

1. `resolveIpv4()` — resolves the destination hostname (dotted-decimal or DNS) to a network-order `uint32_t`.
2. `getMacAddr()` via `ioctl(SIOCGIFHWADDR)` — reads the local NIC MAC address.
3. `getIfaceIpv4()` via `ioctl(SIOCGIFADDR)` — reads the local NIC IP.
4. `lookupArpMac()` — scans `/proc/net/arp` for a complete (`ATF_COM`) entry matching the destination IP. Falls back to broadcast MAC (`FF:FF:FF:FF:FF:FF`) with a warning if no ARP entry exists.

Per `send()` call:

1. `drainComp()` — peeks the **completion ring** (up to 64 entries/batch) and returns completed TX buffer addresses to the `freeFrames_` list.
2. `buildTxFrame()` — assembles the full Ethernet/IPv4/UDP frame into a `std::vector`. The IPv4 checksum is computed by `ipv4Checksum()` (one's complement over the 20-byte header, with the checksum field pre-zeroed). The UDP checksum is left as 0 (optional in IPv4).
3. `memcpy` — copies the assembled frame into the chosen UMEM slot.
4. Post to TX ring via `xsk_ring_prod__reserve` + `xsk_ring_prod__submit`.
5. Kick the kernel with `sendto(fd, nullptr, 0, MSG_DONTWAIT)` if `xsk_ring_prod__needs_wakeup` is set.

### Signal handling

Both `xdpReceiveLoop` (RX) and `AeronIpcSource::readLoop` (Aeron) install `SIGINT`/`SIGTERM` handlers that atomically set a `std::atomic<bool>` stop flag. Each poll loop checks this flag on every iteration. On stop, resources are released in order: `xsk_socket__delete` → `xsk_umem__delete` → `munmap`.

---

## Test Suite

### Running the tests

Both test binaries require no external libraries beyond `libc`/`libpthread` — libxdp and libbpf are not needed.

**Frame-parsing and TX helper tests (`tests/test_parse.cpp`):**

```bash
cmake -B build
cmake --build build --target xdpio_tests
ctest --test-dir build -R parse_tests
# or run the binary directly for per-test output:
./build/xdpio_tests
```

**Shared-memory IPC tests (`tests/test_shmipc.cpp`):**

```bash
cmake -B build
cmake --build build --target xdpio_shmipc_tests
ctest --test-dir build -R shmipc_tests
# or run the binary directly for per-test output:
./build/xdpio_shmipc_tests
```

**Run all tests at once:**

```bash
ctest --test-dir build
```

### Test runner

Both test files use a minimal hand-rolled runner: a `CHECK(cond)` macro that increments global `g_pass`/`g_fail` counters and prints `FAIL file:line expression` to stderr on failure. The `main()` function runs every test function in sequence and exits with code 0 if all pass, 1 otherwise.

### Frame builder helper

`buildFrame()` constructs synthetic Ethernet/IPv4/UDP byte vectors that match the exact binary layout expected by `parseUdp()`. It supports:

- Custom src/dst MAC, IP, port, and payload bytes
- Optional 802.1Q VLAN tagging (inserts a 4-byte tag before the inner EtherType)
- Variable IHL to inject IP options padding
- `udpLenOverride` to write a different value into the UDP length field (for truncation/underflow tests)
- Custom outer EtherType (e.g. `0x0806` for ARP rejection tests)

### `parseUdp` test cases

**Valid frames:**

| Test | What it verifies |
|------|-----------------|
| `basicValid` | Normal IPv4/UDP frame; all `UdpView` fields (`srcIp`, `dstIp`, `srcPort`, `dstPort`, `payloadLen`, payload bytes) correctly extracted |
| `vlanTagged` | 802.1Q frame: parser skips the 4-byte VLAN tag and reads the inner EtherType correctly |
| `ipOptionsIhl6` | IHL=6 (one extra 4-byte option word): parser uses `ihl*4` to find the UDP header |
| `emptyPayload` | Zero-length UDP payload is legal; `payloadLen == 0` |
| `truncatedPayloadClamped` | UDP length header claims more bytes than the frame holds; `payloadLen` is clamped to actual bytes present |

**Rejection cases:**

| Test | What it verifies |
|------|-----------------|
| `tooShortForEthernetHeader` | 13-byte buffer → `false` |
| `tooShortForIpHeader` | Ethernet header only + 4 bytes of IP → `false` |
| `tooShortForUdpHeader` | Ethernet + full IP + only 4 bytes of UDP (8 required) → `false` |
| `nonIpEthertype` | ARP frame (`0x0806`) → `false` |
| `tcpProtocol` | IPv4 carrying TCP (`0x06`) → `false` |
| `ihlTooSmall` | IHL=4 (16 bytes) < minimum 20 → `false` |
| `udpLengthTooSmall` | UDP length field = 7 < 8-byte header minimum → `false` |
| `vlanTooShortForInnerEthertype` | `0x8100` EtherType but fewer than 4 bytes follow → `false` |
| `payloadPointerValidity` | Returned `payload` pointer must lie within the original frame buffer |

### TX helper test cases

| Test | What it verifies |
|------|-----------------|
| `buildTxFrame_roundtrip` | Frame assembled by `buildTxFrame()` parses back through `parseUdp()` with all fields intact |
| `buildTxFrame_emptyPayload` | Zero-length payload survives the round-trip |
| `buildTxFrame_frameSize` | Output size == `14 + 20 + 8 + payloadLen` exactly |
| `buildTxFrame_ipChecksum` | Re-running `ipv4Checksum` over the 20-byte header (with the stored checksum included) returns `0x0000` — the one's-complement verification identity |
| `getMacAddr_loopback` | `ioctl(SIOCGIFHWADDR, "lo")` succeeds and returns `00:00:00:00:00:00` |
| `getIfaceIpv4_loopback` | `ioctl(SIOCGIFADDR, "lo")` returns `127.0.0.1` |

### Helper function test cases

| Test | What it verifies |
|------|-----------------|
| `pickNic_explicit` | Non-empty NIC name is returned unchanged |
| `pickNic_default` | Empty string → `"eth0"` |
| `resolveIpv4_dottedDecimal` | `inet_pton` path; result equals `inet_addr()` |
| `resolveIpv4_loopback` | `"127.0.0.1"` resolves correctly |
| `resolveIpv4_broadcast` | `"255.255.255.255"` resolves correctly |
| `resolveIpv4_invalidOctet` | `"256.0.0.0"` → `0` (invalid, not resolvable) |
| `resolveIpv4_emptyString` | `""` → `0` |

---

## Shared-Memory IPC Test Suite (`tests/test_shmipc.cpp`)

Tests for `ShmIpcPublisher` / `ShmIpcSource` in `shmipc.h`. The test binary links only `libpthread`; no Aeron, libxdp, or libbpf is required.

### `shmName` test cases

| Test | What it verifies |
|------|-----------------|
| `shmName_plain` | Plain channel name produces `/channel_streamId` |
| `shmName_leading_slash` | Leading `/` in the channel name is preserved and not doubled |
| `shmName_colon_replaced` | Colons (illegal in POSIX shm names) are replaced with `_` |
| `shmName_internal_slash_replaced` | Internal `/` characters are replaced with `_` |
| `shmName_empty_channel` | Empty channel string produces a name starting with `/` |

### Publish / receive round-trip test cases

| Test | What it verifies |
|------|-----------------|
| `publish_receive_single` | A single published message is received intact by a `ShmIpcSource` reader thread |
| `publish_receive_multiple` | Five distinct messages are received in order with correct byte content |

### `publish()` edge-case test cases

| Test | What it verifies |
|------|-----------------|
| `publish_zero_length_rejected` | `publish(data, 0)` returns `false` |
| `publish_max_payload_accepted` | `publish()` with exactly `SHM_MAX_PAYLOAD` bytes succeeds and is received at full length |
| `publish_oversized_rejected` | `publish()` with `SHM_MAX_PAYLOAD + 1` bytes returns `false` |

### Ring wrap-around test case

| Test | What it verifies |
|------|-----------------|
| `ring_wraparound` | Publishing `SHM_CAPACITY + 16` messages exercises the modular wrap of `write_pos`/`read_pos`; all messages are received |

