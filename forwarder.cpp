#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <string>

#include "aeron.h"
#include "xdpudp.h"

namespace {

uint16_t parsePort(const char* s, const char* argName) {
    const long v = std::strtol(s, nullptr, 10);
    if (v <= 0 || v > std::numeric_limits<uint16_t>::max())
        throw std::runtime_error(std::string("invalid ") + argName + ": " + s);
    return static_cast<uint16_t>(v);
}

int32_t parseStreamId(const char* s) {
    const long v = std::strtol(s, nullptr, 10);
    if (v <= 0 || v > std::numeric_limits<int32_t>::max())
        throw std::runtime_error(std::string("invalid stream id: ") + s);
    return static_cast<int32_t>(v);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5 || argc > 7) {
        std::cerr
            << "Usage: aeron_xdp_forwarder <aeron_channel> <aeron_stream_id> "
               "<dest_addr> <dest_port> [src_port] [xdp_iface]\n"
            << "Example: aeron_xdp_forwarder aeron:ipc 1001 "
               "192.168.1.10 9000 9001 eth0\n";
        return 2;
    }

    try {
        const std::string channel   = argv[1];
        const int32_t  streamId     = parseStreamId(argv[2]);
        const std::string destAddr  = argv[3];
        const uint16_t destPort     = parsePort(argv[4], "dest port");
        const uint16_t srcPort      = (argc >= 6) ? parsePort(argv[5], "src port") : 0;
        const std::string iface     = (argc == 7) ? argv[6] : "";

        UdpSink sink(destAddr, destPort, srcPort, iface);
        AeronIpcSource source(channel, streamId);

        std::cout << "[aeron-forwarder] " << channel << " stream " << streamId
                  << " -> " << destAddr << ":" << destPort
                  << " (Ctrl-C to stop)\n";

        source.run([&](const uint8_t* data, uint16_t len) {
            if (!sink.send(data, len))
                std::cerr << "[aeron-forwarder] send failed\n";
        });
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[aeron-forwarder] fatal: " << e.what() << "\n";
        return 1;
    }
}
