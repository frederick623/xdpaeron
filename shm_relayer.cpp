#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <string>

#include "shmipc.h"
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
            << "Usage: shm_udp_relayer <shm_channel> <shm_stream_id> "
               "<listen_addr> <listen_port> [xdp_iface]\n"
            << "Example: shm_udp_relayer myapp 1001 239.1.2.3 50000 eth0\n";
        return 2;
    }

    try {
        const std::string channel   = argv[1];
        const int32_t    streamId   = parseStreamId(argv[2]);
        const std::string listenAddr = argv[3];
        const uint16_t   listenPort  = parsePort(argv[4], "listen port");
        const std::string iface      = (argc == 6) ? argv[5] : "";

        UdpSource         xdpSource(listenAddr, listenPort, iface);
        ShmIpcPublisher   publisher(channel, streamId);

        std::cout << "[shm-relayer] " << listenAddr << ":" << listenPort
                  << " -> shm:" << detail::shmName(channel, streamId)
                  << " (Ctrl-C to stop)\n";

        xdpSource.run([&](const uint8_t* data, uint16_t len) {
            if (!publisher.publish(data, len))
                std::cerr << "[shm-relayer] ring full; dropping frame\n";
        });
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[shm-relayer] fatal: " << e.what() << "\n";
        return 1;
    }
}
