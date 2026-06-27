cmake_minimum_required(VERSION 3.51)
project(xdpaeron CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../MPMCSimpleQueue
        ${CMAKE_CURRENT_BINARY_DIR}/mpmcqueue
    )
endif()

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

    if(XDPAERON_HAVE_AERON AND XDP_INCLUDE_DIR AND XDP_LIBRARY AND BPF_LIBRARY AND TARGET mpmcqueue)
        add_executable(xdp_udp_relayer relayer.cpp)
        target_include_directories(xdp_udp_relayer PRIVATE ${XDP_INCLUDE_DIR})
        target_link_libraries(xdp_udp_relayer
            PRIVATE
                xdpaeron::ipc
                mpmcqueue
                ${XDP_LIBRARY}
                ${BPF_LIBRARY}
                Threads::Threads
        )
        message(STATUS "xdpaeron: xdp_udp_relayer enabled")
    else()
        message(STATUS "xdpaeron: xdp_udp_relayer disabled (needs Linux, libxdp, libbpf, Aeron)")
    endif()
endif()
