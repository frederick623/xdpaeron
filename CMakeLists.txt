cmake_minimum_required(VERSION 3.28)
project(xdpaeron CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


find_package(Threads REQUIRED)

set(XDPAERON_HAVE_AERON FALSE)
find_path(AERON_INCLUDE_DIR NAMES Aeron.h aeron/Aeron.h)
find_library(AERON_LIBRARY NAMES aeron_client aeron)
if(AERON_INCLUDE_DIR AND AERON_LIBRARY)
    set(XDPAERON_HAVE_AERON TRUE)
endif()

add_library(xdpaeron_ipc INTERFACE)
add_library(xdpaeron::ipc ALIAS xdpaeron_ipc)
target_include_directories(xdpaeron_ipc
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>
)

if(XDPAERON_HAVE_AERON)
    target_compile_definitions(xdpaeron_ipc INTERFACE TRADEENGINE_HAVE_AERON)
    target_include_directories(xdpaeron_ipc INTERFACE ${AERON_INCLUDE_DIR})
    target_link_libraries(xdpaeron_ipc INTERFACE ${AERON_LIBRARY})
    message(STATUS "xdpaeron: Aeron found — IPC enabled")
else()
    message(STATUS "xdpaeron: Aeron not found — IPC stubs enabled")
endif()

if(UNIX AND NOT APPLE)
    find_path(XDP_INCLUDE_DIR NAMES xdp/xsk.h)
    find_library(XDP_LIBRARY NAMES xdp)
    find_library(BPF_LIBRARY NAMES bpf)

    if(XDPAERON_HAVE_AERON AND XDP_INCLUDE_DIR AND XDP_LIBRARY AND BPF_LIBRARY)
        add_executable(xdp_udp_relayer relayer.cpp)
        target_include_directories(xdp_udp_relayer PRIVATE ${XDP_INCLUDE_DIR})
        target_link_libraries(xdp_udp_relayer
            PRIVATE
                xdpaeron::ipc
                ${XDP_LIBRARY}
                ${BPF_LIBRARY}
                Threads::Threads
        )
        message(STATUS "xdpaeron: xdp_udp_relayer enabled")

        add_executable(aeron_xdp_forwarder forwarder.cpp)
        target_include_directories(aeron_xdp_forwarder PRIVATE ${XDP_INCLUDE_DIR})
        target_link_libraries(aeron_xdp_forwarder
            PRIVATE
                xdpaeron::ipc
                ${XDP_LIBRARY}
                ${BPF_LIBRARY}
                Threads::Threads
        )
        message(STATUS "xdpaeron: aeron_xdp_forwarder enabled")
    else()
        message(STATUS "xdpaeron: xdp_udp_relayer disabled (needs Linux, libxdp, libbpf, Aeron)")
        message(STATUS "xdpaeron: aeron_xdp_forwarder disabled (needs Linux, libxdp, libbpf, Aeron)")
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
        message(STATUS "xdpaeron: shm_udp_relayer enabled")

        add_executable(shm_xdp_forwarder shm_forwarder.cpp)
        target_include_directories(shm_xdp_forwarder PRIVATE ${XDP_INCLUDE_DIR})
        target_link_libraries(shm_xdp_forwarder
            PRIVATE
                ${XDP_LIBRARY}
                ${BPF_LIBRARY}
                Threads::Threads
        )
        message(STATUS "xdpaeron: shm_xdp_forwarder enabled")
    else()
        message(STATUS "xdpaeron: shm_udp_relayer disabled (needs Linux, libxdp, libbpf)")
        message(STATUS "xdpaeron: shm_xdp_forwarder disabled (needs Linux, libxdp, libbpf)")
    endif()

    # ── Unit tests (no libxdp required: stub headers are used) ────────────────
    enable_testing()
    add_executable(xdpaeron_tests tests/test_parse.cpp)
    target_compile_features(xdpaeron_tests PRIVATE cxx_std_20)
    target_include_directories(xdpaeron_tests
        PRIVATE
            # Repo root so that xdpudp.h and aeron.h are found.
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(xdpaeron_tests PRIVATE Threads::Threads)
    add_test(NAME parse_tests COMMAND xdpaeron_tests)
    message(STATUS "xdpaeron: unit tests enabled (target: xdpaeron_tests)")

    # ── Shared-memory IPC unit tests (no libxdp required) ─────────────────────
    add_executable(xdpaeron_shmipc_tests tests/test_shmipc.cpp)
    target_compile_features(xdpaeron_shmipc_tests PRIVATE cxx_std_20)
    target_include_directories(xdpaeron_shmipc_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(xdpaeron_shmipc_tests PRIVATE Threads::Threads)
    add_test(NAME shmipc_tests COMMAND xdpaeron_shmipc_tests)
    message(STATUS "xdpaeron: shmipc tests enabled (target: xdpaeron_shmipc_tests)")

    # ── Benchmarks (requires Google Benchmark; graceful skip if absent) ────────
    find_package(benchmark QUIET)
    if(benchmark_FOUND)
        add_executable(xdpaeron_bench benchmarks/bench_udp_path.cpp)
        target_compile_features(xdpaeron_bench PRIVATE cxx_std_20)
        target_include_directories(xdpaeron_bench
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}
        )
        target_link_libraries(xdpaeron_bench
            PRIVATE
                benchmark::benchmark
                Threads::Threads
        )
        message(STATUS "xdpaeron: benchmarks enabled (target: xdpaeron_bench)")
    else()
        message(STATUS "xdpaeron: benchmarks disabled (install libbenchmark-dev to enable)")
    endif()
endif()
