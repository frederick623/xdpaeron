# xdpaeron

A high-performance UDP-to-Aeron relayer built on **AF_XDP** (Linux kernel-bypass networking). It receives raw UDP datagrams directly from a NIC queue — bypassing the kernel TCP/IP stack entirely — and publishes them as messages on an [Aeron](https://github.com/real-logic/aeron) IPC channel with minimal latency.

---

## Overview

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

The main entry point (`relayer.cpp`) wires these two components together as a standalone `xdp_udp_relayer` executable. Both components are also available as reusable C++ classes for use in larger applications.

---

## Repository Structure

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | CMake build configuration (C++20). Detects optional dependencies (Aeron, libxdp, libbpf) and enables features accordingly. |
| `relayer.cpp` | `main()` for the `xdp_udp_relayer` executable. Parses CLI arguments and wires `UdpSource` → `AeronIpcPublisher`. |
| `xdpudp.h` | `UdpSource` and `UdpConnector` — AF_XDP receive loop, frame parsing (Ethernet/VLAN/IPv4/UDP), and IP/port filtering. Linux-only. |
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

## Running the Relayer

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

> **Privileges required:** `CAP_NET_RAW`, `CAP_BPF`, `CAP_SYS_ADMIN` (or simply run with `sudo`).

**Recommended — pin the NIC to a single RX queue** so all traffic lands on queue 0:

```bash
sudo ethtool -L eth0 combined 1
```

Press `Ctrl-C` or send `SIGTERM` to stop gracefully.

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

