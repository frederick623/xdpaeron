cmake_minimum_required(VERSION 3.28)
project(xdpio CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


find_package(Threads REQUIRED)

set(XDPIO_HAVE_AERON FALSE)
find_path(AERON_INCLUDE_DIR NAMES Aeron.h aeron/Aeron.h)
find_library(AERON_LIBRARY NAMES aeron_client aeron)
if(AERON_INCLUDE_DIR AND AERON_LIBRARY)
    set(XDPIO_HAVE_AERON TRUE)
endif()

add_library(xdpio_ipc INTERFACE)
add_library(xdpio::ipc ALIAS xdpio_ipc)
target_include_directories(xdpio_ipc
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>
)

if(XDPIO_HAVE_AERON)
    target_compile_definitions(xdpio_ipc INTERFACE TRADEENGINE_HAVE_AERON)
    target_include_directories(xdpio_ipc INTERFACE ${AERON_INCLUDE_DIR})
    target_link_libraries(xdpio_ipc INTERFACE ${AERON_LIBRARY})
    message(STATUS "xdpio: Aeron found — IPC enabled")
else()
    message(STATUS "xdpio: Aeron not found — IPC stubs enabled")
endif()

if(UNIX AND NOT APPLE)
    find_path(XDP_INCLUDE_DIR NAMES xdp/xsk.h)
    find_library(XDP_LIBRARY NAMES xdp)
    find_library(BPF_LIBRARY NAMES bpf)

    if(XDPIO_HAVE_AERON AND XDP_INCLUDE_DIR AND XDP_LIBRARY AND BPF_LIBRARY)
        add_executable(xdp_udp_relayer relayer.cpp)
        target_include_directories(xdp_udp_relayer PRIVATE ${XDP_INCLUDE_DIR})
        target_link_libraries(xdp_udp_relayer
            PRIVATE
                xdpio::ipc
                ${XDP_LIBRARY}
                ${BPF_LIBRARY}
                Threads::Threads
        )
        message(STATUS "xdpio: xdp_udp_relayer enabled")

        add_executable(aeron_xdp_forwarder forwarder.cpp)
        target_include_directories(aeron_xdp_forwarder PRIVATE ${XDP_INCLUDE_DIR})
        target_link_libraries(aeron_xdp_forwarder
            PRIVATE
                xdpio::ipc
                ${XDP_LIBRARY}
                ${BPF_LIBRARY}
                Threads::Threads
        )
        message(STATUS "xdpio: aeron_xdp_forwarder enabled")
    else()
        message(STATUS "xdpio: xdp_udp_relayer disabled (needs Linux, libxdp, libbpf, Aeron)")
        message(STATUS "xdpio: aeron_xdp_forwarder disabled (needs Linux, libxdp, libbpf, Aeron)")
    endif()

    # ── Shared-memory IPC counterparts (no Aeron required; only need libxdp) ──
    if(XDP_INCLUDE_DIR AND XDP_LIBRARY AND BPF_LIBRARY)
        add_executable(shm_udp_relayer shm_relayer.cpp)
        target_include_directories(shm_udp_relayer PRIVATE ${XDP_INCLUDE_DIR})
        target_link_libraries(shm_udp_relayer
            PRIVATE
                ${XDP_LIBRARY}
                ${BPF_LIBRARY}
                Threads::Threads
        )
        message(STATUS "xdpio: shm_udp_relayer enabled")

        add_executable(shm_xdp_forwarder shm_forwarder.cpp)
        target_include_directories(shm_xdp_forwarder PRIVATE ${XDP_INCLUDE_DIR})
        target_link_libraries(shm_xdp_forwarder
            PRIVATE
                ${XDP_LIBRARY}
                ${BPF_LIBRARY}
                Threads::Threads
        )
        message(STATUS "xdpio: shm_xdp_forwarder enabled")
    else()
        message(STATUS "xdpio: shm_udp_relayer disabled (needs Linux, libxdp, libbpf)")
        message(STATUS "xdpio: shm_xdp_forwarder disabled (needs Linux, libxdp, libbpf)")
    endif()

    # ── Unit tests (no libxdp required: stub headers are used) ────────────────
    enable_testing()
    add_executable(xdpio_tests tests/test_parse.cpp)
    target_compile_features(xdpio_tests PRIVATE cxx_std_20)
    target_include_directories(xdpio_tests
        PRIVATE
            # Repo root so that xdpudp.h and aeron.h are found.
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(xdpio_tests PRIVATE Threads::Threads)
    add_test(NAME parse_tests COMMAND xdpio_tests)
    message(STATUS "xdpio: unit tests enabled (target: xdpio_tests)")

    # ── Shared-memory IPC unit tests (no libxdp required) ─────────────────────
    add_executable(xdpio_shmipc_tests tests/test_shmipc.cpp)
    target_compile_features(xdpio_shmipc_tests PRIVATE cxx_std_20)
    target_include_directories(xdpio_shmipc_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(xdpio_shmipc_tests PRIVATE Threads::Threads)
    add_test(NAME shmipc_tests COMMAND xdpio_shmipc_tests)
    message(STATUS "xdpio: shmipc tests enabled (target: xdpio_shmipc_tests)")

    # ── Benchmarks (requires Google Benchmark; graceful skip if absent) ────────
    find_package(benchmark QUIET)
    if(benchmark_FOUND)
        add_executable(xdpio_bench benchmarks/bench_udp_path.cpp)
        target_compile_features(xdpio_bench PRIVATE cxx_std_20)
        target_include_directories(xdpio_bench
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}
        )
        target_link_libraries(xdpio_bench
            PRIVATE
                benchmark::benchmark
                Threads::Threads
        )
        message(STATUS "xdpio: benchmarks enabled (target: xdpio_bench)")

        add_executable(xdpio_bench_shm benchmarks/bench_shm_path.cpp)
        target_compile_features(xdpio_bench_shm PRIVATE cxx_std_20)
        target_include_directories(xdpio_bench_shm
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}
        )
        target_link_libraries(xdpio_bench_shm
            PRIVATE
                benchmark::benchmark
                Threads::Threads
        )
        message(STATUS "xdpio: shm benchmarks enabled (target: xdpio_bench_shm)")
    else()
        message(STATUS "xdpio: benchmarks disabled (install libbenchmark-dev to enable)")
    endif()
endif()
