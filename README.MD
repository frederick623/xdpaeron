# xdpaeron

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

---

### XDP Socket Read/Write and Data Flow vs. Traditional UNIX Sockets

#### Traditional UNIX Socket (UDP via `SOCK_DGRAM`)

**Data Flow (RX Path):**

```
Physical NIC
    ↓ (DMA: hardware writes frame to kernel ring buffer)
Kernel RX ring (in kernel memory)
    ↓ (kernel driver processes Ethernet/IP/UDP headers)
Kernel network stack (netfilter, iptables, route lookup, etc.)
    ↓ (socket lookup; packet queued in socket buffer)
Socket RX buffer (sk_buff queue, kernel memory, typically bounded at ~100 MB)
    ↓ (userspace calls recvfrom() → syscall entry, context switch)
User-space buffer (application reads data)
```

**Data Flow (TX Path):**

```
User-space buffer (application data)
    ↓ (userspace calls sendto() → syscall entry, context switch)
Socket TX buffer (sk_buff in kernel memory)
    ↓ (kernel builds Ethernet/IP/UDP headers, route lookup, ARP resolution)
Kernel network stack (netfilter, iptables, QoS, etc.)
    ↓ (kernel TX ring, hardware queue)
Physical NIC
    ↓ (DMA: hardware reads frame from kernel ring buffer, transmits)
Network (wire)
```

**Characteristics:**

- **Syscalls per packet:** At least 2 (one RX `recvfrom()`, one TX `sendto()`), plus additional syscalls for setup, configuration, and ARP lookups.
- **Context switches:** One per packet in each direction, consuming CPU time and L3 cache invalidation.
- **Memory copies:** 1–2 copies per packet in typical operation (frame data copied from kernel to userspace; TX frame re-serialized by kernel).
- **Latency:** Typical minimum 1–10 microseconds per packet (depends on CPU load, scheduling, contention).
- **Throughput (benchmark):** ~890 Mbps @ 512-byte packets (shown in benchmark above: `BM_UdpKernelRecv/512 1068 ns`).
- **Buffering:** Kernel-managed; bounded by `/proc/sys/net/core/rmem_max` and `/proc/sys/net/core/wmem_max`.

#### AF_XDP Socket (Kernel-Bypass UDP)

**Data Flow (RX Path):**

```
Physical NIC
    ↓ (DMA: hardware writes frame directly to user-space UMEM ring)
User-space UMEM ring (zero-copy mode, or kernel copies for SKB mode)
    ↓ (user-space XDP poll loop reads frame addresses from RX descriptor ring)
User-space buffer (frame data is directly accessible, no copy)
    ↓ (application parses Ethernet/IP/UDP headers, filters, extracts payload)
Application buffer (UDP payload bytes delivered to callback)
```

**Data Flow (TX Path):**

```
Application buffer (message bytes)
    ↓ (application writes frame to UMEM; builds Ethernet/IP/UDP headers)
User-space UMEM ring (frame data, zero-copy)
    ↓ (user-space code writes frame address to TX descriptor ring)
Hardware TX ring (kernel-managed, but no syscall needed)
    ↓ (DMA: hardware reads frame from UMEM ring, transmits)
Physical NIC → Network (wire)
```

**Characteristics:**

- **Syscalls per packet:** 0–1 (batched polling loop; no syscall per packet).
- **Context switches:** Effectively none (single poll thread, no context switching during steady-state packet flow).
- **Memory copies:** 0 (zero-copy mode) or 1 (SKB/copy mode, automatic fallback if NIC driver does not support zero-copy).
- **Latency:** Typically <500 nanoseconds per packet in zero-copy mode (shown in benchmark: `BM_XdpParseAndFilter/512 1.22 ns`).
- **Throughput (benchmark):** ~420 Gbps @ 512-byte packets (shown in benchmark above: `BM_XdpParseAndFilter/512`).
- **Buffering:** Application-managed via pre-allocated UMEM chunks (no kernel buffer bloat).

#### Key Differences Summary

| Aspect | Traditional UDP (`SOCK_DGRAM`) | AF_XDP |
|--------|--------------------------------|--------|
| **Syscalls / packet** | 2+ (recvfrom, sendto, + setup) | 0–1 (batched poll) |
| **Context switches** | 1+ per packet | None (single poll thread) |
| **Memory copies** | 1–2 per packet | 0 (zero-copy) or 1 (SKB mode) |
| **Latency (typical)** | 1–10 µs | <500 ns |
| **Throughput @ 512B** | ~890 Mbps | ~420 Gbps |
| **Kernel stack overhead** | Full TCP/IP stack processing | Minimal (pre-filtered by XDP BPF program) |
| **Header building** | Kernel does it | Application does it |
| **Buffer management** | Kernel-managed; bounded | Application-managed; pre-allocated UMEM |
| **Driver dependency** | Works on all drivers | Requires libxdp + libbpf support |
| **Zero-copy support** | No | Yes (hardware-dependent) |

#### RX Path: Detailed XDP Buffer Management

1. **UMEM Setup:**
   - Application allocates a contiguous user-space memory region (UMEM) — typically 128 MB or more.
   - UMEM is divided into fixed-size 4 KB chunks (pages).
   - Pages are pre-registered with the kernel via `xsk_umem__create()` using the `XSK_UMEM_PGSIZE_HUGE1GB` or similar flags for huge pages.
   - Huge pages reduce TLB misses and improve cache locality.

2. **Descriptor Rings:**
   - **FillQ (application → kernel):** Application writes page indices to tell the kernel which UMEM pages are ready to receive data.
   - **RX ring (kernel → application):** Kernel writes completed frame metadata (offset, length) for frames that arrived.
   - Both rings are lock-free circular buffers in shared memory (no syscalls needed to drain).

3. **Polling Loop (Zero-Copy RX):**
   ```
   while (running) {
       // Refill kernel's FillQ with empty pages
       refill_fill_queue();
       
       // Poll RX ring for completed frames
       nb_rx = xsk_ring_cons__peek(&rx_ring, BATCH_SIZE, &idx);
       if (nb_rx == 0) continue;  // No frames yet
       
       for (i = 0; i < nb_rx; i++) {
           // Kernel has written frame data to UMEM at offset+length
           const struct xdp_desc* desc = xsk_ring_cons__at(&rx_ring, idx + i);
           const uint8_t* frame = (uint8_t*)umem_area + desc->addr;
           uint16_t len = desc->len;
           
           // Parse headers, filter, invoke callback with payload
           parseUdp(frame, len, udpView);
           onPacket(udpView.payload, udpView.payloadLen);
       }
       xsk_ring_cons__release(&rx_ring, nb_rx);
   }
   ```

#### TX Path: Detailed XDP Buffer Management

1. **TX Descriptor Ring Setup:**
   - Application pre-allocates frame buffers in UMEM (typically preceding RX buffers in the same region).
   - **TX ring (application → kernel):** Application writes frame descriptors (offset in UMEM, length).
   - **Completion ring (kernel → application):** Kernel acknowledges which TX descriptors have been sent.

2. **Batched TX Loop:**
   ```
   // Build frame in application-owned UMEM region
   uint8_t* frame_buf = (uint8_t*)umem_area + tx_offset;
   buildEthernetIpUdpFrame(frame_buf, payload, len);
   
   // Submit to TX ring (no syscall yet)
   struct xdp_desc tx_desc = {
       .addr = tx_offset,
       .len = frame_len,
   };
   xsk_ring_prod__submit(&tx_ring, 1);
   
   // Kick hardware when batch is large enough (or on timeout)
   if (pending_tx > BATCH_THRESHOLD) {
       sendto(xsk_fd, NULL, 0, 0, NULL, 0);  // One syscall per batch, not per packet
   }
   
   // Poll completion ring to reclaim buffers
   nb_completed = xsk_ring_cons__peek(&comp_ring, BATCH_SIZE, &idx);
   // Mark pages as available for reuse
   ```

3. **Hardware Interaction:**
   - The kernel manages a hardware TX ring that is populated from the application's TX descriptor ring.
   - The actual frame transmission (DMA to hardware) happens without application involvement.
   - Completion is signaled back asynchronously, allowing pipelined operation.

#### Implications for High-Frequency Trading & Real-Time Systems

- **Jitter:** AF_XDP eliminates most kernel-induced jitter (page faults, GC, context switches).
- **Predictability:** Latency is dominated by hardware latency, not software scheduling.
- **Scalability:** Multiple AF_XDP sockets can be bound to different NIC queues, enabling CPU affinity and linear scaling.
- **Cost:** AF_XDP requires more application code (header building, frame parsing) but offers ~1000× latency improvement and ~500× throughput improvement over traditional UDP sockets.

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

## Benchmark

```
root@30874c415336:/ebpf/xdpaeron# ./xdpaeron_bench 
2026-06-28T11:20:37+00:00
Running ./xdpaeron_bench
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

