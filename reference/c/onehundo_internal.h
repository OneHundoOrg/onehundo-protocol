/**
 * @file onehundo_internal.h
 * @brief Internal wire framing, atomic handoff, and test-sink definitions for reference transports.
 *
 * NOT part of the public ABI (must remain in reference/c/, never in include/).
 */

#ifndef ONEHUNDO_INTERNAL_H
#define ONEHUNDO_INTERNAL_H

#include "onehundo/onehundo.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Wire Framing (24-byte 64-bit aligned header)                              */
/* ========================================================================= */

#define ONEHUNDO_WIRE_VERSION 1U

typedef enum onehundo_wire_msg_type {
    ONEHUNDO_MSG_HELLO            = 0x0001U,
    ONEHUNDO_MSG_HELLO_ACK        = 0x0002U,
    ONEHUNDO_MSG_ARENA_REGISTER   = 0x0003U,
    ONEHUNDO_MSG_ARENA_ACK        = 0x0004U,
    ONEHUNDO_MSG_EVENT_PUBLISH    = 0x0010U,
    ONEHUNDO_MSG_EVENT_ACK        = 0x0011U,
    ONEHUNDO_MSG_LIFECYCLE_SIGNAL = 0x0020U,
    ONEHUNDO_MSG_GOODBYE          = 0x00FFU
} onehundo_wire_msg_type_t;

typedef struct onehundo_wire_header {
    uint32_t magic;           /**< ONEHUNDO_WIRE_MAGIC (0x31303048) */
    uint16_t version;         /**< ONEHUNDO_WIRE_VERSION (1) */
    uint16_t msg_type;        /**< onehundo_wire_msg_type_t */
    uint32_t flags;           /**< Reserved flags (0) */
    uint32_t payload_length;  /**< Payload byte count following header */
    uint64_t sequence_number; /**< Monotonic sequence number */
} onehundo_wire_header_t;

/* ========================================================================= */
/* Portable Release/Acquire Atomic Helpers                                   */
/* ========================================================================= */

#if defined(_MSC_VER)
#include <intrin.h>
static __inline uint32_t oh_atomic_u32_load_acq(const volatile uint32_t *ptr) {
    uint32_t v = *ptr;
    _ReadWriteBarrier();
    return v;
}
static __inline void oh_atomic_u32_store_rel(volatile uint32_t *ptr, uint32_t val) {
    _ReadWriteBarrier();
    *ptr = val;
}
static __inline bool oh_atomic_u32_cas(volatile uint32_t *ptr, uint32_t *expected, uint32_t desired) {
    long prev = _InterlockedCompareExchange((volatile long *)ptr, (long)desired, (long)*expected);
    if ((uint32_t)prev == *expected) {
        return true;
    }
    *expected = (uint32_t)prev;
    return false;
}
#elif defined(__GNUC__) || defined(__clang__)
static inline uint32_t oh_atomic_u32_load_acq(const volatile uint32_t *ptr) {
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}
static inline void oh_atomic_u32_store_rel(volatile uint32_t *ptr, uint32_t val) {
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}
static inline bool oh_atomic_u32_cas(volatile uint32_t *ptr, uint32_t *expected, uint32_t desired) {
    return __atomic_compare_exchange_n(
        ptr, expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE
    );
}
static inline uint64_t oh_atomic_u64_load_acq(const volatile uint64_t *ptr) {
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}
static inline void oh_atomic_u64_store_rel(volatile uint64_t *ptr, uint64_t val) {
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}
static inline uint64_t oh_atomic_u64_fetch_add(volatile uint64_t *ptr, uint64_t delta) {
    return __atomic_fetch_add(ptr, delta, __ATOMIC_ACQ_REL);
}
#else
#error "Unsupported compiler for atomic primitives in reference implementation"
#endif

/* ========================================================================= */
/* Arena Ring Configuration                                                  */
/* ========================================================================= */

#define ONEHUNDO_RING_SLOT_COUNT    4U
#define ONEHUNDO_RING_SLOT_CAPACITY (64U * 1024U)
#define ONEHUNDO_STUB_EVENT_CAP     1024U
#define ONEHUNDO_MAX_REGIONS        32U

typedef struct onehundo_arena_slot {
    onehundo_snapshot_slot_header_t header;
    uint8_t payload[ONEHUNDO_RING_SLOT_CAPACITY];
} onehundo_arena_slot_t;

typedef struct onehundo_arena_shm_layout {
    uint32_t magic;
    uint32_t version;
    uint32_t slot_count;
    uint32_t slot_capacity;
    onehundo_arena_slot_t slots[ONEHUNDO_RING_SLOT_COUNT];
} onehundo_arena_shm_layout_t;

/* ========================================================================= */
/* Stub Inspection & Mock Consumer API (for Headless / TSan Unit Tests)      */
/* ========================================================================= */

typedef struct onehundo_captured_event {
    uint32_t event_type;
    uint32_t progression_mode;
    uint32_t trust_tier;
    uint32_t evidence_class;
    uint64_t sequence_number;
    uint64_t timestamp_nanos;
    char producer_id[128];
    char session_id[64];
    char idempotency_key[256];
    char key[128];
    int64_t int_value;
    double float_value;
    char string_value[256];
    bool acked;
} onehundo_captured_event_t;

/**
 * Inspect the next unconsumed semantic event from the stub sink without blocking.
 * Returns true if an event was copied out, false if queue empty.
 */
bool onehundo_stub_pop_event(
    onehundo_context_t *ctx,
    onehundo_captured_event_t *out_event
);

/**
 * Simulate daemon EVENT_ACK after durable storage commit (Commit-Before-Ack).
 */
int32_t onehundo_stub_ack_event(
    onehundo_context_t *ctx,
    uint64_t sequence_number
);

/**
 * Consumer side of Plane A slot handoff: finds a READY slot, transitions
 * READY -> READING via CAS, and returns its index and header.
 */
int32_t onehundo_stub_consumer_acquire_ready_slot(
    onehundo_context_t *ctx,
    uint32_t *out_slot_index,
    onehundo_snapshot_slot_header_t *out_header,
    const void **out_payload_ptr
);

/**
 * Consumer side completion: transitions READING -> FREE with release store.
 */
int32_t onehundo_stub_consumer_release_slot(
    onehundo_context_t *ctx,
    uint32_t slot_index
);

#ifdef __cplusplus
}
#endif

#endif /* ONEHUNDO_INTERNAL_H */
