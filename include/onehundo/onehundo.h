/**
 * @file onehundo.h
 * @brief Authoritative ISO C99 Public API for OneHundo Protocol.
 *
 * OneHundo Universal Achievement Protocol SDK.
 * Standardises integration contracts, memory arena exchange, semantic events,
 * and external observation evidence without binding to specific runtimes.
 *
 * Invariant: Must compile under -std=c99 -pedantic -Wall -Wextra -Werror
 * with zero OS or third-party headers (<stdint.h>, <stddef.h>, <stdbool.h> only).
 *
 * Copyright (c) 2026 OneHundo.org
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */

#ifndef ONEHUNDO_H
#define ONEHUNDO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Versioning & ABI Guards                                                   */
/* ========================================================================= */

#define ONEHUNDO_API_VERSION_MAJOR 0
#define ONEHUNDO_API_VERSION_MINOR 1
#define ONEHUNDO_API_VERSION_PATCH 0

/** Packed 32-bit version representation: (major << 16) | (minor << 8) | patch */
#define ONEHUNDO_API_VERSION \
    (((uint32_t)ONEHUNDO_API_VERSION_MAJOR << 16) | \
     ((uint32_t)ONEHUNDO_API_VERSION_MINOR << 8)  | \
      (uint32_t)ONEHUNDO_API_VERSION_PATCH)

#define ONEHUNDO_WIRE_MAGIC 0x31303048U /* ASCII '100H' in little-endian */

/* ========================================================================= */
/* Status and Error Codes                                                    */
/* ========================================================================= */

typedef enum onehundo_status {
    ONEHUNDO_OK                         =  0,
    ONEHUNDO_ERR_INVALID_ARGUMENT       = -1,
    ONEHUNDO_ERR_UNSUPPORTED_VERSION    = -2,
    ONEHUNDO_ERR_BUFFER_TOO_SMALL       = -3,
    ONEHUNDO_ERR_QUEUE_FULL             = -4,
    ONEHUNDO_ERR_NOT_CONNECTED          = -5,
    ONEHUNDO_ERR_INVALID_STATE          = -6,
    ONEHUNDO_ERR_UNSUPPORTED_PLANE      = -7,
    ONEHUNDO_ERR_NO_FREE_SLOT           = -8,
    ONEHUNDO_ERR_TRANSPORT_FAILURE      = -9,
    ONEHUNDO_ERR_SIZE_MISMATCH          = -10
} onehundo_status_t;

/* ========================================================================= */
/* The Three Ingestion Planes                                                */
/* ========================================================================= */

typedef enum onehundo_ingestion_plane {
    ONEHUNDO_PLANE_UNKNOWN              = 0,
    ONEHUNDO_PLANE_MEMORY_ARENA         = 1, /**< Plane A: Guest-coherent memory view */
    ONEHUNDO_PLANE_SEMANTIC_EVENT       = 2, /**< Plane B: Typed gameplay facts */
    ONEHUNDO_PLANE_EXTERNAL_OBSERVATION = 3  /**< Plane C: Passive evidence observation */
} onehundo_ingestion_plane_t;

/* ========================================================================= */
/* Four Orthogonal State Dimensions                                          */
/* ========================================================================= */

/** Dimension 1: Progression Mode */
typedef enum onehundo_progression_mode {
    ONEHUNDO_PROGRESSION_MODE_STANDARD  = 1, /**< Standard unmodified campaign progression */
    ONEHUNDO_PROGRESSION_MODE_ENCORE    = 2, /**< Modifiers, cheats, NG+, remix modes */
    ONEHUNDO_PROGRESSION_MODE_SPECTATOR = 3  /**< Passive observation / replay evaluation */
} onehundo_progression_mode_t;

/** Dimension 2: Trust Tier */
typedef enum onehundo_trust_tier {
    ONEHUNDO_TRUST_TIER_LOCAL               = 1, /**< Local development or unverified user */
    ONEHUNDO_TRUST_TIER_VERIFIED_INTEGRATION = 2, /**< Authenticated runtime / signed pack */
    ONEHUNDO_TRUST_TIER_COMPETITIVE         = 3  /**< Hardware / anti-cheat / attested run */
} onehundo_trust_tier_t;

/** Dimension 3: Evidence Class */
typedef enum onehundo_evidence_class {
    ONEHUNDO_EVIDENCE_CLASS_DIRECT_INTEGRATION_EVENT = 1, /**< Runtime emitted fact */
    ONEHUNDO_EVIDENCE_CLASS_RUNTIME_OBSERVED         = 2, /**< Memory watcher / breakpoint */
    ONEHUNDO_EVIDENCE_CLASS_PLATFORM_CONFIRMED       = 3, /**< Steam, PSN, Xbox, RetroAchievements */
    ONEHUNDO_EVIDENCE_CLASS_LOCALLY_OBSERVED         = 4, /**< Local save file scan */
    ONEHUNDO_EVIDENCE_CLASS_INFERRED                 = 5  /**< Reconstructed from logs/metadata */
} onehundo_evidence_class_t;

/** Dimension 4: Verification State */
typedef enum onehundo_verification_state {
    ONEHUNDO_VERIFICATION_STATE_PENDING    = 1, /**< Awaiting processing or quorum */
    ONEHUNDO_VERIFICATION_STATE_VERIFIED   = 2, /**< Verified and accepted */
    ONEHUNDO_VERIFICATION_STATE_UNVERIFIED = 3, /**< Retained as unverified local progress */
    ONEHUNDO_VERIFICATION_STATE_REJECTED   = 4  /**< Explicitly rejected by policy/anti-cheat */
} onehundo_verification_state_t;

/* ========================================================================= */
/* Lifecycle Signals (Plane A & Runtime Events)                              */
/* ========================================================================= */

typedef enum onehundo_lifecycle_signal {
    ONEHUNDO_SIGNAL_NORMAL_FRAME    = 1, /**< Standard deterministic frame step */
    ONEHUNDO_SIGNAL_SAVE_STATE_LOAD = 2, /**< Save state restored; evaluation must rewind/reset */
    ONEHUNDO_SIGNAL_SOFT_RESET      = 3, /**< Software/warm reset triggered */
    ONEHUNDO_SIGNAL_HARD_RESET      = 4, /**< Hard power cycle / cold reboot */
    ONEHUNDO_SIGNAL_FRAME_GAP       = 5  /**< Explicit buffer overflow or dropped frame notification */
} onehundo_lifecycle_signal_t;

/* ========================================================================= */
/* Plane A — Memory Arena Structures & Slot Lifecycle                        */
/* ========================================================================= */

/** Endianness specification for guest memory regions */
typedef enum onehundo_endianness {
    ONEHUNDO_ENDIAN_LITTLE = 1,
    ONEHUNDO_ENDIAN_BIG    = 2
} onehundo_endianness_t;

/** Coherence semantics for guest memory regions */
typedef enum onehundo_coherence {
    ONEHUNDO_COHERENCE_COHERENT      = 1, /**< Fully coherent CPU/RAM view */
    ONEHUNDO_COHERENCE_CACHED_DIRTY  = 2, /**< Requires explicit cache invalidate */
    ONEHUNDO_COHERENCE_DMA_ASYNC     = 3  /**< Asynchronous DMA transfer window */
} onehundo_coherence_t;

/** Region flags */
#define ONEHUNDO_REGION_FLAG_READ_ONLY  (1U << 0)
#define ONEHUNDO_REGION_FLAG_BANKED     (1U << 1)
#define ONEHUNDO_REGION_FLAG_VOLATILE   (1U << 2)

/**
 * Descriptor for a single contiguous memory arena region.
 * Guarded by struct_size and api_version.
 */
typedef struct onehundo_region_descriptor {
    uint32_t struct_size;          /**< sizeof(struct onehundo_region_descriptor) */
    uint32_t api_version;          /**< ONEHUNDO_API_VERSION */
    uint32_t region_id;            /**< Host-assigned region index */
    uint32_t flags;                /**< Bitmask of ONEHUNDO_REGION_FLAG_* */
    uint64_t guest_base_address;   /**< Base address in guest memory space */
    uint64_t size_bytes;           /**< Size of the memory region in bytes */
    uint8_t  endianness;           /**< ONEHUNDO_ENDIAN_* */
    uint8_t  coherence;            /**< ONEHUNDO_COHERENCE_* */
    uint8_t  reserved[6];          /**< Explicit padding for 64-bit alignment */
    const char *region_name;       /**< Optional human-readable tag (e.g. "Main RAM") */
} onehundo_region_descriptor_t;

/** Snapshot slot ownership lifecycle states */
typedef enum onehundo_slot_state {
    ONEHUNDO_SLOT_STATE_FREE    = 0, /**< Available for producer allocation */
    ONEHUNDO_SLOT_STATE_WRITING = 1, /**< Producer is writing guest snapshot */
    ONEHUNDO_SLOT_STATE_READY   = 2, /**< Write complete; memory fence released */
    ONEHUNDO_SLOT_STATE_READING = 3  /**< Consumer daemon is reading slot */
} onehundo_slot_state_t;

/**
 * Header guarding a snapshot slot in shared memory.
 * Must be 64-bit aligned.
 */
typedef struct onehundo_snapshot_slot_header {
    uint32_t struct_size;          /**< sizeof(struct onehundo_snapshot_slot_header) */
    uint32_t api_version;          /**< ONEHUNDO_API_VERSION */
    uint32_t slot_index;           /**< Zero-based slot index in the ring */
    uint32_t slot_state;           /**< Atomic onehundo_slot_state_t */
    uint64_t sequence_number;      /**< Monotonic snapshot sequence number */
    uint64_t timestamp_nanos;      /**< Nanosecond host or guest timestamp */
    uint32_t lifecycle_signal;     /**< ONEHUNDO_SIGNAL_* */
    uint32_t frame_gap_count;      /**< Dropped frame count if signal == FRAME_GAP */
    uint64_t payload_size_bytes;   /**< Bytes written in this snapshot */
    uint32_t crc32;                /**< CRC-32 / xxHash checksum of payload */
    uint32_t reserved;             /**< Explicit alignment padding */
} onehundo_snapshot_slot_header_t;

/* ========================================================================= */
/* Plane B — Semantic Event Definitions                                      */
/* ========================================================================= */

typedef enum onehundo_semantic_event_type {
    ONEHUNDO_EVENT_STAT_SET       = 1, /**< Sets absolute stat value */
    ONEHUNDO_EVENT_STAT_INC       = 2, /**< Increments stat by delta */
    ONEHUNDO_EVENT_STATE_OBSERVE  = 3, /**< Emits discrete key-value state fact */
    ONEHUNDO_EVENT_UNLOCK_REQUEST = 4  /**< Direct achievement unlock request */
} onehundo_semantic_event_type_t;

/**
 * Representation of a semantic event.
 */
typedef struct onehundo_semantic_event {
    uint32_t struct_size;          /**< sizeof(struct onehundo_semantic_event) */
    uint32_t api_version;          /**< ONEHUNDO_API_VERSION */
    uint32_t event_type;           /**< ONEHUNDO_EVENT_* */
    uint32_t progression_mode;     /**< ONEHUNDO_PROGRESSION_MODE_* */
    uint32_t trust_tier;           /**< ONEHUNDO_TRUST_TIER_* */
    uint32_t evidence_class;       /**< ONEHUNDO_EVIDENCE_CLASS_* */
    uint64_t sequence_number;      /**< Monotonic session sequence number */
    uint64_t timestamp_nanos;      /**< Monotonic timestamp in nanoseconds */
    const char *producer_id;       /**< Unique identifier of the producer */
    const char *session_id;        /**< UUID of the current gameplay session */
    const char *idempotency_key;   /**< producer_id + session_id + seq_num */
    const char *key;               /**< Stat key, state key, or achievement ID */
    int64_t  int_value;            /**< Integer value for STAT_SET / STAT_INC */
    double   float_value;          /**< Floating point value (if applicable) */
    const char *string_value;      /**< String value for STATE_OBSERVE */
} onehundo_semantic_event_t;

/* ========================================================================= */
/* Client Configuration & Context                                            */
/* ========================================================================= */

/**
 * Initialization configuration for the onehundo client SDK.
 */
typedef struct onehundo_client_config {
    uint32_t struct_size;          /**< sizeof(struct onehundo_client_config) */
    uint32_t api_version;          /**< ONEHUNDO_API_VERSION */
    const char *canonical_game_id; /**< Canonical ID e.g. "sony.bloodborne" */
    const char *producer_id;       /**< Integration name/version e.g. "bbport_pc" */
    const char *session_id;        /**< Session UUID or NULL to auto-generate */
    uint32_t ingestion_plane;      /**< ONEHUNDO_PLANE_* */
    uint32_t progression_mode;     /**< ONEHUNDO_PROGRESSION_MODE_* */
    uint32_t trust_tier;           /**< ONEHUNDO_TRUST_TIER_* */
    uint32_t event_queue_capacity; /**< Maximum pending event ring size */
    void *transport_user_data;     /**< User pointer for custom transports */
    const char *ipc_endpoint_path; /**< Custom socket / pipe path or NULL */
} onehundo_client_config_t;

/**
 * Runtime telemetry and diagnostics.
 */
typedef struct onehundo_diagnostics {
    uint32_t struct_size;               /**< sizeof(struct onehundo_diagnostics) */
    uint32_t api_version;               /**< ONEHUNDO_API_VERSION */
    uint64_t events_emitted;            /**< Total events queued by runtime */
    uint64_t events_acked;              /**< Total events acknowledged by daemon */
    uint64_t events_dropped;            /**< Events dropped due to ring overflow */
    uint64_t frame_snapshots_published; /**< Plane A snapshots published */
    uint64_t frame_gaps_recorded;       /**< Plane A frame gap count */
    uint64_t last_sequence_number;      /**< Last assigned sequence number */
    uint32_t is_connected;              /**< 1 if IPC connected, 0 otherwise */
    uint32_t pending_queue_depth;       /**< Current queue depth */
} onehundo_diagnostics_t;

/** Opaque handle to the client integration context */
typedef struct onehundo_context onehundo_context_t;

/* ========================================================================= */
/* Public API Functions                                                      */
/* ========================================================================= */

/**
 * Initialize a client config struct with default values.
 *
 * @param config Pointer to config structure to initialize.
 */
void onehundo_client_config_init(onehundo_client_config_t *config);

/**
 * Initialize a OneHundo client context.
 *
 * @param out_ctx Pointer to receive the allocated context handle.
 * @param config Client configuration options.
 * @param config_size Size of configuration struct (for forward ABI compatibility).
 * @return ONEHUNDO_OK on success, or negative error code.
 */
int32_t onehundo_init(
    onehundo_context_t **out_ctx,
    const onehundo_client_config_t *config,
    size_t config_size
);

/**
 * Shut down the client context and release associated resources.
 *
 * @param ctx Context handle to destroy.
 */
void onehundo_shutdown(onehundo_context_t *ctx);

/* ------------------------------------------------------------------------- */
/* Plane B Non-Blocking Semantic Events                                      */
/* ------------------------------------------------------------------------- */

/**
 * Set an absolute integer stat value (non-blocking).
 *
 * @param ctx Client context handle.
 * @param stat_key Unique stat name defined in pack manifest.
 * @param value Absolute integer value.
 * @return ONEHUNDO_OK on success, or negative error code.
 */
int32_t onehundo_stat_set(
    onehundo_context_t *ctx,
    const char *stat_key,
    int64_t value
);

/**
 * Increment a stat value by a delta (non-blocking).
 *
 * @param ctx Client context handle.
 * @param stat_key Unique stat name defined in pack manifest.
 * @param delta Amount to increment (may be negative).
 * @return ONEHUNDO_OK on success, or negative error code.
 */
int32_t onehundo_stat_inc(
    onehundo_context_t *ctx,
    const char *stat_key,
    int64_t delta
);

/**
 * Record a discrete state observation (non-blocking).
 *
 * @param ctx Client context handle.
 * @param state_key Unique state variable key.
 * @param state_value State string representation.
 * @return ONEHUNDO_OK on success, or negative error code.
 */
int32_t onehundo_state_observe(
    onehundo_context_t *ctx,
    const char *state_key,
    const char *state_value
);

/**
 * Request an explicit achievement unlock (non-blocking).
 *
 * @param ctx Client context handle.
 * @param achievement_id Target achievement ID defined in pack.
 * @return ONEHUNDO_OK on success, or negative error code.
 */
int32_t onehundo_unlock_request(
    onehundo_context_t *ctx,
    const char *achievement_id
);

/* ------------------------------------------------------------------------- */
/* Plane A Memory Arena Interface                                            */
/* ------------------------------------------------------------------------- */

/**
 * Register a memory arena region descriptor with the daemon.
 *
 * @param ctx Client context handle.
 * @param desc Region descriptor.
 * @param desc_size Size of struct onehundo_region_descriptor.
 * @return ONEHUNDO_OK on success, or negative error code.
 */
int32_t onehundo_arena_register_region(
    onehundo_context_t *ctx,
    const onehundo_region_descriptor_t *desc,
    size_t desc_size
);

/**
 * Acquire a free snapshot slot buffer for writing (non-blocking).
 *
 * @param ctx Client context handle.
 * @param out_slot_index Pointer to receive slot index.
 * @param out_buffer_ptr Pointer to receive writable memory destination.
 * @param out_max_size Pointer to receive buffer capacity.
 * @return ONEHUNDO_OK if acquired, ONEHUNDO_ERR_NO_FREE_SLOT if ring full.
 */
int32_t onehundo_arena_acquire_slot(
    onehundo_context_t *ctx,
    uint32_t *out_slot_index,
    void **out_buffer_ptr,
    size_t *out_max_size
);

/**
 * Publish a populated snapshot slot, transitioning it to READY (non-blocking).
 *
 * @param ctx Client context handle.
 * @param slot_index Slot index obtained from onehundo_arena_acquire_slot.
 * @param header Snapshot slot header with sequence number and signals.
 * @return ONEHUNDO_OK on success, or negative error code.
 */
int32_t onehundo_arena_publish_slot(
    onehundo_context_t *ctx,
    uint32_t slot_index,
    const onehundo_snapshot_slot_header_t *header
);

/**
 * Emit an explicit lifecycle signal (e.g. frame reset, save load, or frame gap).
 *
 * @param ctx Client context handle.
 * @param signal ONEHUNDO_SIGNAL_*
 * @param frame_gap_count Number of skipped frames if signal == FRAME_GAP, else 0.
 * @return ONEHUNDO_OK on success, or negative error code.
 */
int32_t onehundo_emit_lifecycle_signal(
    onehundo_context_t *ctx,
    uint32_t signal,
    uint32_t frame_gap_count
);

/* ------------------------------------------------------------------------- */
/* Diagnostics & Utilities                                                   */
/* ------------------------------------------------------------------------- */

/**
 * Query current diagnostics and queue health.
 *
 * @param ctx Client context handle.
 * @param out_diag Output buffer for diagnostics.
 * @param diag_size Size of struct onehundo_diagnostics.
 * @return ONEHUNDO_OK on success, or negative error code.
 */
int32_t onehundo_get_diagnostics(
    const onehundo_context_t *ctx,
    onehundo_diagnostics_t *out_diag,
    size_t diag_size
);

/**
 * Return a human-readable description of a status error code.
 *
 * @param error_code Status code.
 * @return Static string error message.
 */
const char *onehundo_strerror(int32_t error_code);

/**
 * Return the protocol library version string (e.g. "0.1.0").
 */
const char *onehundo_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* ONEHUNDO_H */
