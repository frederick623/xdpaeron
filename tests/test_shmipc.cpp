// Unit tests for shmipc.h — ShmIpcPublisher / ShmIpcSource.
//
// Tests exercise:
//   shmName       — POSIX shm name derivation
//   ShmIpcPublisher — creates ring, publish() round-trip, back-pressure
//   ShmIpcSource  — receives messages via run() (driven from a thread)
//
// Build (handled by CMakeLists.txt):
//   cmake -B build && cmake --build build --target xdpio_shmipc_tests
//   ctest --test-dir build -R shmipc

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "shmipc.h"

// ── Minimal test runner ───────────────────────────────────────────────────────
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) {                                                            \
            ++g_pass;                                                          \
        } else {                                                               \
            ++g_fail;                                                          \
            std::fprintf(stderr, "FAIL  %s:%d  %s\n",                         \
                         __FILE__, __LINE__, #cond);                           \
        }                                                                      \
    } while (0)

// ── shmName tests ─────────────────────────────────────────────────────────────

static void test_shmName_plain() {
    CHECK(detail::shmName("myapp", 1) == "/myapp_1");
}

static void test_shmName_leading_slash() {
    CHECK(detail::shmName("/myapp", 42) == "/myapp_42");
}

static void test_shmName_colon_replaced() {
    // Colons are illegal in POSIX shm names — they must be replaced with '_'.
    const std::string name = detail::shmName("aeron:ipc", 1001);
    CHECK(name == "/aeron_ipc_1001");
    for (char c : name) CHECK(c != ':');
}

static void test_shmName_internal_slash_replaced() {
    const std::string name = detail::shmName("a/b", 5);
    // Leading '/' is kept; the internal '/' should be replaced.
    CHECK(name == "/a_b_5");
}

static void test_shmName_empty_channel() {
    const std::string name = detail::shmName("", 7);
    CHECK(!name.empty() && name[0] == '/');
    CHECK(name == "/_7");
}

// ── Publish / receive round-trip ──────────────────────────────────────────────

// Shared-memory name used by all publish/receive tests (avoids collision with
// any live segments left from a prior run — ShmIpcPublisher unlinks stale ones
// in its constructor).
static const char kTestChannel[] = "xdpio_test";
static constexpr int32_t kStreamId = 9887;

static void test_publish_receive_single() {
    ShmIpcPublisher pub(kTestChannel, kStreamId);

    const uint8_t msg[] = {'H', 'e', 'l', 'l', 'o'};

    // Reset stop flag before starting the reader thread.
    detail::shmStop().store(false, std::memory_order_relaxed);

    std::vector<std::vector<uint8_t>> received;
    std::atomic<bool>                 done{false};

    ShmIpcSource src(kTestChannel, kStreamId);
    std::thread reader([&]() {
        src.run([&](const uint8_t* d, uint16_t n) {
            received.emplace_back(d, d + n);
            // Stop after the first message.
            detail::shmStop().store(true, std::memory_order_relaxed);
        });
        done.store(true, std::memory_order_relaxed);
    });

    // Give the reader thread time to open the segment and enter its loop.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    pub.publish(msg, sizeof msg);

    // Wait for the reader to finish (at most 2 s).
    for (int i = 0; i < 2000 && !done.load(std::memory_order_relaxed); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    reader.join();

    CHECK(received.size() == 1u);
    if (!received.empty()) {
        CHECK(received[0].size() == sizeof msg);
        CHECK(std::memcmp(received[0].data(), msg, sizeof msg) == 0);
    }
}

static void test_publish_receive_multiple() {
    ShmIpcPublisher pub(kTestChannel, kStreamId);

    constexpr int kCount = 5;
    const uint8_t msgs[kCount][4] = {
        {0x01, 0x02, 0x03, 0x04},
        {0xAA, 0xBB, 0xCC, 0xDD},
        {0x11, 0x22, 0x33, 0x44},
        {0xDE, 0xAD, 0xBE, 0xEF},
        {0xFF, 0x00, 0xFF, 0x00},
    };

    detail::shmStop().store(false, std::memory_order_relaxed);

    std::vector<std::vector<uint8_t>> received;
    std::atomic<bool>                 done{false};
    ShmIpcSource src(kTestChannel, kStreamId);
    std::thread reader([&]() {
        src.run([&](const uint8_t* d, uint16_t n) {
            received.emplace_back(d, d + n);
            if (static_cast<int>(received.size()) >= kCount)
                detail::shmStop().store(true, std::memory_order_relaxed);
        });
        done.store(true, std::memory_order_relaxed);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    for (int i = 0; i < kCount; ++i)
        pub.publish(msgs[i], 4);

    for (int i = 0; i < 2000 && !done.load(std::memory_order_relaxed); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    reader.join();

    CHECK(static_cast<int>(received.size()) == kCount);
    for (int i = 0; i < std::min(kCount, (int)received.size()); ++i) {
        CHECK(received[i].size() == 4u);
        CHECK(std::memcmp(received[i].data(), msgs[i], 4) == 0);
    }
}

// ── publish() edge cases ──────────────────────────────────────────────────────

static void test_publish_zero_length_rejected() {
    ShmIpcPublisher pub(kTestChannel, kStreamId);
    const uint8_t   data[] = {0x42};
    CHECK(pub.publish(data, 0) == false);
}

static void test_publish_max_payload_accepted() {
    ShmIpcPublisher pub(kTestChannel, kStreamId);
    std::vector<uint8_t> big(detail::SHM_MAX_PAYLOAD, 0xAB);

    detail::shmStop().store(false, std::memory_order_relaxed);

    std::atomic<uint16_t> recvLen{0};
    std::atomic<bool>     done{false};
    ShmIpcSource src(kTestChannel, kStreamId);
    std::thread reader([&]() {
        src.run([&](const uint8_t* /*d*/, uint16_t n) {
            recvLen.store(n, std::memory_order_relaxed);
            detail::shmStop().store(true, std::memory_order_relaxed);
        });
        done.store(true, std::memory_order_relaxed);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(pub.publish(big.data(), static_cast<uint16_t>(big.size())) == true);

    for (int i = 0; i < 2000 && !done.load(std::memory_order_relaxed); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    reader.join();

    CHECK(recvLen.load() == detail::SHM_MAX_PAYLOAD);
}

static void test_publish_oversized_rejected() {
    ShmIpcPublisher pub(kTestChannel, kStreamId);
    std::vector<uint8_t> huge(detail::SHM_MAX_PAYLOAD + 1u, 0xFF);
    CHECK(pub.publish(huge.data(),
                      static_cast<uint16_t>(detail::SHM_MAX_PAYLOAD + 1u)) == false);
}

// ── Ring wrap-around ──────────────────────────────────────────────────────────
// Publish and receive more messages than a single pass through the ring to
// verify correct modular wrap of write_pos / read_pos.

static void test_ring_wraparound() {
    // Use a dedicated stream ID so there is no segment collision.
    constexpr int32_t kWrapStream = 9888;
    constexpr int     kTotal      = static_cast<int>(detail::SHM_CAPACITY) + 16;

    ShmIpcPublisher pub(kTestChannel, kWrapStream);

    detail::shmStop().store(false, std::memory_order_relaxed);

    std::atomic<int> recvCount{0};
    std::atomic<bool> done{false};
    ShmIpcSource src(kTestChannel, kWrapStream);
    std::thread reader([&]() {
        src.run([&](const uint8_t* /*d*/, uint16_t /*n*/) {
            if (recvCount.fetch_add(1, std::memory_order_relaxed) + 1 >= kTotal)
                detail::shmStop().store(true, std::memory_order_relaxed);
        });
        done.store(true, std::memory_order_relaxed);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const uint8_t data[4] = {0xCA, 0xFE, 0xBA, 0xBE};
    int published = 0;
    while (published < kTotal) {
        if (pub.publish(data, 4))
            ++published;
        else
            std::this_thread::yield(); // ring full — wait for consumer
    }

    for (int i = 0; i < 5000 && !done.load(std::memory_order_relaxed); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    reader.join();

    CHECK(recvCount.load() == kTotal);
}

// ── main ──────────────────────────────────────────────────────────────────────
int main() {
    // shmName
    test_shmName_plain();
    test_shmName_leading_slash();
    test_shmName_colon_replaced();
    test_shmName_internal_slash_replaced();
    test_shmName_empty_channel();

    // publish / receive
    test_publish_receive_single();
    test_publish_receive_multiple();

    // edge cases
    test_publish_zero_length_rejected();
    test_publish_max_payload_accepted();
    test_publish_oversized_rejected();

    // wrap-around
    test_ring_wraparound();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
