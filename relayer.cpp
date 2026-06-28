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
    if (argc < 5 || argc > 6) {
        std::cerr
            << "Usage: relayer <listen_addr> <listen_port> "
               "<aeron_channel> <aeron_stream_id> [xdp_iface]\n"
            << "Example: relayer 239.1.2.3 50000 aeron:ipc 1001 eth0\n";
        return 2;
    }

    try {
        const std::string listenAddr = argv[1];
        const uint16_t listenPort = parsePort(argv[2], "listen port");
        const std::string channel = argv[3];
        const int32_t streamId = parseStreamId(argv[4]);
        const std::string iface = (argc == 6) ? argv[5] : "";

        UdpSource xdpSource(listenAddr, listenPort, iface);
        AeronIpcPublisher publisher(channel, streamId);

        std::cout << "[xdp-relayer] " << listenAddr << ":" << listenPort
                  << " -> " << channel << " stream " << streamId
                  << " (Ctrl-C to stop)\n";

        xdpSource.run([&](const uint8_t* data, uint16_t len) {
            if (!publisher.publish(data, len))
                std::cerr << "[xdp-relayer] Aeron backpressure or disconnected\n";
        });
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[xdp-relayer] fatal: " << e.what() << "\n";
        return 1;
    }
}
