#pragma once
// Minimal stub for <xdp/xsk.h> used only by the unit-test build.
// It provides just enough type and constant definitions so that xdpudp.h
// compiles without libxdp installed.  None of the stub functions are called
// during testing — only the pure parsing helpers are exercised.

#include <stddef.h>
#include <stdint.h>

// Pull in the kernel's if_xdp.h which already defines:
//   XDP_USE_NEED_WAKEUP, struct xdp_desc, and related constants.
#include <linux/if_xdp.h>

// ── Ring / UMEM geometry constants ───────────────────────────────────────────
#ifndef XSK_UMEM__DEFAULT_FRAME_SIZE
#define XSK_UMEM__DEFAULT_FRAME_SIZE      4096u
#endif
#ifndef XSK_RING_PROD__DEFAULT_NUM_DESCS
#define XSK_RING_PROD__DEFAULT_NUM_DESCS  2048u
#endif
#ifndef XSK_RING_CONS__DEFAULT_NUM_DESCS
#define XSK_RING_CONS__DEFAULT_NUM_DESCS  2048u
#endif
#ifndef XSK_UMEM__DEFAULT_FRAME_HEADROOM
#define XSK_UMEM__DEFAULT_FRAME_HEADROOM  0u
#endif

// ── Opaque handle types ───────────────────────────────────────────────────────
typedef struct { int _unused; } xsk_umem;
typedef struct { int _unused; } xsk_ring_prod;
typedef struct { int _unused; } xsk_ring_cons;
typedef struct { int _unused; } xsk_socket;

// ── Configuration structs ─────────────────────────────────────────────────────
struct xsk_umem_config {
    uint32_t fill_size;
    uint32_t comp_size;
    uint32_t frame_size;
    uint32_t frame_headroom;
    uint32_t flags;
};

struct xsk_socket_config {
    uint32_t rx_size;
    uint32_t tx_size;
    uint32_t libbpf_flags;
    uint32_t xdp_flags;
    uint16_t bind_flags;
};

// ── Stub inline functions — compile but never execute in tests ─────────────────
static inline int xsk_umem__create(xsk_umem** u, void* area, size_t size,
                                   xsk_ring_prod* fill, xsk_ring_cons* comp,
                                   const struct xsk_umem_config* cfg) {
    (void)u; (void)area; (void)size; (void)fill; (void)comp; (void)cfg;
    return -38; // -ENOSYS
}
static inline int xsk_umem__delete(xsk_umem* u) { (void)u; return 0; }

static inline int xsk_socket__create(xsk_socket** xsk, const char* ifname,
                                     uint32_t queue_id, xsk_umem* umem,
                                     xsk_ring_cons* rx, xsk_ring_prod* tx,
                                     const struct xsk_socket_config* cfg) {
    (void)xsk; (void)ifname; (void)queue_id; (void)umem;
    (void)rx; (void)tx; (void)cfg;
    return -38;
}
static inline void xsk_socket__delete(xsk_socket* xsk) { (void)xsk; }
static inline int  xsk_socket__fd(xsk_socket* xsk) { (void)xsk; return -1; }

static inline uint32_t xsk_ring_prod__reserve(xsk_ring_prod* r, uint32_t nb,
                                               uint32_t* idx) {
    (void)r; (void)nb; (void)idx; return 0u;
}
static inline uint64_t* xsk_ring_prod__fill_addr(xsk_ring_prod* r, uint32_t idx) {
    (void)r; (void)idx; return (uint64_t*)0;
}
static inline void xsk_ring_prod__submit(xsk_ring_prod* r, uint32_t nb) {
    (void)r; (void)nb;
}
static inline int xsk_ring_prod__needs_wakeup(const xsk_ring_prod* r) {
    (void)r; return 0;
}

static inline uint32_t xsk_ring_cons__peek(xsk_ring_cons* r, uint32_t nb,
                                            uint32_t* idx) {
    (void)r; (void)nb; (void)idx; return 0u;
}
static inline const struct xdp_desc* xsk_ring_cons__rx_desc(
        const xsk_ring_cons* r, uint32_t idx) {
    (void)r; (void)idx; return (const struct xdp_desc*)0;
}
static inline void xsk_ring_cons__release(xsk_ring_cons* r, uint32_t nb) {
    (void)r; (void)nb;
}

static inline uint64_t xsk_umem__extract_addr(uint64_t addr)       { return addr; }
static inline uint64_t xsk_umem__add_offset_to_addr(uint64_t addr) { return addr; }
static inline void*    xsk_umem__get_data(void* area, uint64_t addr) {
    return (uint8_t*)area + addr;
}
