#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>


#if defined(TRADEENGINE_HAVE_AERON)
#  if __has_include(<Aeron.h>)
#    include <Aeron.h>
#  elif __has_include(<aeron/Aeron.h>)
#    include <aeron/Aeron.h>
#  else
#    undef TRADEENGINE_HAVE_AERON
#  endif
#endif

namespace mde::feed {
namespace detail {

inline std::atomic<bool>& aeronStop() {
    static std::atomic<bool> s{false};
    return s;
}

inline void aeronOnSignal(int) { aeronStop().store(true, std::memory_order_relaxed); }

inline void aeronInstallSignals() {
    struct sigaction sa{};
    sa.sa_handler = aeronOnSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

} // namespace detail

#if defined(TRADEENGINE_HAVE_AERON)

class AeronIpcPublisher {
public:
    AeronIpcPublisher(std::string channel, int32_t streamId)
        : channel_(std::move(channel)), streamId_(streamId) {
        connect();
    }

    bool publish(const uint8_t* data, uint16_t len) {
        ensurePublication();
        aeron::concurrent::AtomicBuffer buffer(const_cast<uint8_t*>(data), len);
        const std::int64_t result = publication_->offer(buffer, 0, len);
        return result >= 0;
    }

private:
    void connect() {
        aeron::Context ctx;
        aeron_ = aeron::Aeron::connect(ctx);
        if (!aeron_) throw std::runtime_error("aeron connect returned null");
    }

    void ensurePublication() {
        if (publication_) return;
        const std::int64_t regId = aeron_->addPublication(channel_, streamId_);
        for (int i = 0; i < 5000; ++i) {
            publication_ = aeron_->findPublication(regId);
            if (publication_) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("timeout waiting for Aeron publication");
    }

private:
    std::string channel_;
    int32_t streamId_;
    std::shared_ptr<aeron::Aeron> aeron_;
    std::shared_ptr<aeron::Publication> publication_;
};

class AeronIpcSource {
public:
    AeronIpcSource(std::string channel, int32_t streamId, int32_t fragmentLimit = 64)
        : channel_(std::move(channel)),
          streamId_(streamId),
          fragmentLimit_(fragmentLimit > 0 ? fragmentLimit : 64) {}

    template <class OnPacket>
    void run(OnPacket&& onPacket) {
        readLoop(std::forward<OnPacket>(onPacket));
    }

private:
    template <class Sink>
    void readLoop(Sink&& sink) {
        aeron::Context ctx;
        auto a = aeron::Aeron::connect(ctx);
        if (!a) throw std::runtime_error("aeron connect returned null");

        const std::int64_t regId = a->addSubscription(channel_, streamId_);
        std::shared_ptr<aeron::Subscription> sub;
        for (int i = 0; i < 5000; ++i) {
            sub = a->findSubscription(regId);
            if (sub) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!sub) throw std::runtime_error("timeout waiting for Aeron subscription");

        std::cout << "[aeron] subscribed " << channel_ << " stream "
                  << streamId_ << " (Ctrl-C to stop)\n";
        detail::aeronInstallSignals();
        auto& stop = detail::aeronStop();
        stop.store(false, std::memory_order_relaxed);

        auto fragmentHandler = [&](aeron::concurrent::AtomicBuffer& buffer,
                                   aeron::util::index_t offset,
                                   aeron::util::index_t length,
                                   aeron::Header&) {
            if (length <= 0) return;
            const auto n = static_cast<uint16_t>(
                std::min<aeron::util::index_t>(length, 0xFFFF));
            sink(reinterpret_cast<const uint8_t*>(buffer.buffer() + offset), n);
        };

        while (!stop.load(std::memory_order_relaxed)) {
            if (sub->poll(fragmentHandler, fragmentLimit_) == 0)
                std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        std::cout << "[aeron] stopped\n";
    }

private:
    std::string channel_;
    int32_t streamId_;
    int32_t fragmentLimit_;
};

#else

class AeronIpcPublisher {
public:
    AeronIpcPublisher(std::string, int32_t) {}
    bool publish(const uint8_t*, uint16_t) const {
        std::cerr << "[aeron] unavailable: build with Aeron C++ client\n";
        return false;
    }
};

class AeronIpcSource {
public:
    AeronIpcSource(std::string, int32_t, int32_t = 64) {}

    template <class OnPacket>
    void run(OnPacket&&) const {
        std::cerr << "[aeron] unavailable: build with Aeron C++ client\n";
    }
};

#endif

} // namespace mde::feed
