/**
 * @file transport_stub.c
 * @brief In-memory headless reference implementation of the OneHundo C99 SDK.
 *
 * Provides a lock-free SPSC semantic event sink and a multi-slot memory arena
 * implementing the FREE -> WRITING -> READY -> READING -> FREE ownership protocol
 * with acquire/release memory ordering.
 */

#include "onehundo_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct onehundo_context {
    char canonical_game_id[128];
    char producer_id[128];
    char session_id[64];
    uint32_t ingestion_plane;
    uint32_t progression_mode;
    uint32_t trust_tier;
    uint32_t event_queue_capacity;

    /* Atomic diagnostics & sequence counters */
    volatile uint64_t next_seq;
    volatile uint64_t events_emitted;
    volatile uint64_t events_acked;
    volatile uint64_t events_dropped;
    volatile uint64_t frame_snapshots_published;
    volatile uint64_t frame_gaps_recorded;
    volatile uint32_t is_connected;

    /* Lock-free SPSC event ring for headless inspection */
    uint32_t event_ring_cap;
    volatile uint32_t event_write_idx;
    volatile uint32_t event_read_idx;
    onehundo_captured_event_t events[ONEHUNDO_STUB_EVENT_CAP];

    /* Registered Plane A memory regions */
    uint32_t region_count;
    onehundo_region_descriptor_t regions[ONEHUNDO_MAX_REGIONS];
    char region_names[ONEHUNDO_MAX_REGIONS][64];

    /* Plane A shared memory arena simulation */
    onehundo_arena_shm_layout_t arena;
    uint32_t producer_slot_cursor;
    uint32_t consumer_slot_cursor;
};

static void oh_safe_strcpy(char *dst, size_t dst_cap, const char *src) {
    size_t len;
    if (!dst || dst_cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    len = strlen(src);
    if (len >= dst_cap) {
        len = dst_cap - 1U;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

void onehundo_client_config_init(onehundo_client_config_t *config) {
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->struct_size = (uint32_t)sizeof(onehundo_client_config_t);
    config->api_version = ONEHUNDO_API_VERSION;
    config->ingestion_plane = ONEHUNDO_PLANE_SEMANTIC_EVENT;
    config->progression_mode = ONEHUNDO_PROGRESSION_MODE_STANDARD;
    config->trust_tier = ONEHUNDO_TRUST_TIER_LOCAL;
    config->event_queue_capacity = ONEHUNDO_STUB_EVENT_CAP;
}

int32_t onehundo_init(
    onehundo_context_t **out_ctx,
    const onehundo_client_config_t *config,
    size_t config_size
) {
    onehundo_context_t *ctx;
    uint32_t i;

    if (!out_ctx || !config) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    if (config_size != sizeof(onehundo_client_config_t) ||
        config->struct_size != sizeof(onehundo_client_config_t)) {
        return ONEHUNDO_ERR_SIZE_MISMATCH;
    }
    if ((config->api_version >> 16) != ONEHUNDO_API_VERSION_MAJOR) {
        return ONEHUNDO_ERR_UNSUPPORTED_VERSION;
    }
    if (!config->canonical_game_id || config->canonical_game_id[0] == '\0' ||
        !config->producer_id || config->producer_id[0] == '\0') {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }

    ctx = (onehundo_context_t *)calloc(1, sizeof(onehundo_context_t));
    if (!ctx) {
        return ONEHUNDO_ERR_BUFFER_TOO_SMALL;
    }

    oh_safe_strcpy(ctx->canonical_game_id, sizeof(ctx->canonical_game_id), config->canonical_game_id);
    oh_safe_strcpy(ctx->producer_id, sizeof(ctx->producer_id), config->producer_id);
    if (config->session_id && config->session_id[0] != '\0') {
        oh_safe_strcpy(ctx->session_id, sizeof(ctx->session_id), config->session_id);
    } else {
        oh_safe_strcpy(ctx->session_id, sizeof(ctx->session_id), "00000000-0000-4000-8000-000000000001");
    }

    ctx->ingestion_plane = config->ingestion_plane;
    ctx->progression_mode = config->progression_mode ? config->progression_mode : ONEHUNDO_PROGRESSION_MODE_STANDARD;
    ctx->trust_tier = config->trust_tier ? config->trust_tier : ONEHUNDO_TRUST_TIER_LOCAL;
    ctx->event_queue_capacity = ONEHUNDO_STUB_EVENT_CAP;
    ctx->event_ring_cap = ONEHUNDO_STUB_EVENT_CAP;
    ctx->next_seq = 1U;
    ctx->is_connected = 1U;

    ctx->arena.magic = ONEHUNDO_WIRE_MAGIC;
    ctx->arena.version = ONEHUNDO_WIRE_VERSION;
    ctx->arena.slot_count = ONEHUNDO_RING_SLOT_COUNT;
    ctx->arena.slot_capacity = ONEHUNDO_RING_SLOT_CAPACITY;
    for (i = 0; i < ONEHUNDO_RING_SLOT_COUNT; ++i) {
        ctx->arena.slots[i].header.struct_size = (uint32_t)sizeof(onehundo_snapshot_slot_header_t);
        ctx->arena.slots[i].header.api_version = ONEHUNDO_API_VERSION;
        ctx->arena.slots[i].header.slot_index = i;
        oh_atomic_u32_store_rel(&ctx->arena.slots[i].header.slot_state, ONEHUNDO_SLOT_STATE_FREE);
    }

    *out_ctx = ctx;
    return ONEHUNDO_OK;
}

void onehundo_shutdown(onehundo_context_t *ctx) {
    if (!ctx) {
        return;
    }
    oh_atomic_u32_store_rel(&ctx->is_connected, 0U);
    free(ctx);
}

static int32_t oh_enqueue_semantic_event(
    onehundo_context_t *ctx,
    uint32_t event_type,
    const char *key,
    int64_t int_val,
    double float_val,
    const char *str_val
) {
    uint32_t w;
    uint32_t r;
    uint32_t next_w;
    uint64_t seq;
    onehundo_captured_event_t *slot;

    if (!ctx || !key || key[0] == '\0') {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }

    w = oh_atomic_u32_load_acq(&ctx->event_write_idx);
    r = oh_atomic_u32_load_acq(&ctx->event_read_idx);
    next_w = (w + 1U) % ctx->event_ring_cap;

    if (next_w == r) {
        oh_atomic_u64_fetch_add(&ctx->events_dropped, 1U);
        return ONEHUNDO_ERR_QUEUE_FULL;
    }

    seq = oh_atomic_u64_fetch_add(&ctx->next_seq, 1U);
    slot = &ctx->events[w];
    memset(slot, 0, sizeof(*slot));
    slot->event_type = event_type;
    slot->progression_mode = ctx->progression_mode;
    slot->trust_tier = ctx->trust_tier;
    slot->evidence_class = ONEHUNDO_EVIDENCE_CLASS_DIRECT_INTEGRATION_EVENT;
    slot->sequence_number = seq;
    slot->timestamp_nanos = seq * 1000000ULL;
    oh_safe_strcpy(slot->producer_id, sizeof(slot->producer_id), ctx->producer_id);
    oh_safe_strcpy(slot->session_id, sizeof(slot->session_id), ctx->session_id);
    snprintf(
        slot->idempotency_key,
        sizeof(slot->idempotency_key),
        "%s:%s:%llu",
        ctx->producer_id,
        ctx->session_id,
        (unsigned long long)seq
    );
    oh_safe_strcpy(slot->key, sizeof(slot->key), key);
    slot->int_value = int_val;
    slot->float_value = float_val;
    oh_safe_strcpy(slot->string_value, sizeof(slot->string_value), str_val);
    slot->acked = false;

    oh_atomic_u32_store_rel(&ctx->event_write_idx, next_w);
    oh_atomic_u64_fetch_add(&ctx->events_emitted, 1U);
    return ONEHUNDO_OK;
}

int32_t onehundo_stat_set(onehundo_context_t *ctx, const char *stat_key, int64_t value) {
    return oh_enqueue_semantic_event(ctx, ONEHUNDO_EVENT_STAT_SET, stat_key, value, 0.0, NULL);
}

int32_t onehundo_stat_inc(onehundo_context_t *ctx, const char *stat_key, int64_t delta) {
    return oh_enqueue_semantic_event(ctx, ONEHUNDO_EVENT_STAT_INC, stat_key, delta, 0.0, NULL);
}

int32_t onehundo_state_observe(onehundo_context_t *ctx, const char *state_key, const char *state_value) {
    if (!state_value) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    return oh_enqueue_semantic_event(ctx, ONEHUNDO_EVENT_STATE_OBSERVE, state_key, 0, 0.0, state_value);
}

int32_t onehundo_unlock_request(onehundo_context_t *ctx, const char *achievement_id) {
    return oh_enqueue_semantic_event(ctx, ONEHUNDO_EVENT_UNLOCK_REQUEST, achievement_id, 1, 0.0, NULL);
}

int32_t onehundo_arena_register_region(
    onehundo_context_t *ctx,
    const onehundo_region_descriptor_t *desc,
    size_t desc_size
) {
    uint32_t idx;
    if (!ctx || !desc) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    if (desc_size != sizeof(onehundo_region_descriptor_t) ||
        desc->struct_size != sizeof(onehundo_region_descriptor_t)) {
        return ONEHUNDO_ERR_SIZE_MISMATCH;
    }
    if ((desc->api_version >> 16) != ONEHUNDO_API_VERSION_MAJOR) {
        return ONEHUNDO_ERR_UNSUPPORTED_VERSION;
    }
    if (desc->size_bytes == 0) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    if (ctx->region_count >= ONEHUNDO_MAX_REGIONS) {
        return ONEHUNDO_ERR_QUEUE_FULL;
    }

    idx = ctx->region_count++;
    ctx->regions[idx] = *desc;
    oh_safe_strcpy(ctx->region_names[idx], sizeof(ctx->region_names[idx]), desc->region_name);
    ctx->regions[idx].region_name = ctx->region_names[idx];
    return ONEHUNDO_OK;
}

int32_t onehundo_arena_acquire_slot(
    onehundo_context_t *ctx,
    uint32_t *out_slot_index,
    void **out_buffer_ptr,
    size_t *out_max_size
) {
    uint32_t i;
    if (!ctx || !out_slot_index || !out_buffer_ptr || !out_max_size) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }

    for (i = 0; i < ctx->arena.slot_count; ++i) {
        uint32_t idx = (ctx->producer_slot_cursor + i) % ctx->arena.slot_count;
        uint32_t expected = ONEHUNDO_SLOT_STATE_FREE;
        if (oh_atomic_u32_cas(&ctx->arena.slots[idx].header.slot_state, &expected, ONEHUNDO_SLOT_STATE_WRITING)) {
            ctx->producer_slot_cursor = (idx + 1U) % ctx->arena.slot_count;
            *out_slot_index = idx;
            *out_buffer_ptr = ctx->arena.slots[idx].payload;
            *out_max_size = ctx->arena.slot_capacity;
            return ONEHUNDO_OK;
        }
    }

    return ONEHUNDO_ERR_NO_FREE_SLOT;
}

int32_t onehundo_arena_publish_slot(
    onehundo_context_t *ctx,
    uint32_t slot_index,
    const onehundo_snapshot_slot_header_t *header
) {
    onehundo_arena_slot_t *slot;
    uint32_t state;

    if (!ctx || !header) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    if (slot_index >= ctx->arena.slot_count) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    if (header->struct_size != sizeof(onehundo_snapshot_slot_header_t)) {
        return ONEHUNDO_ERR_SIZE_MISMATCH;
    }
    if (header->payload_size_bytes > ctx->arena.slot_capacity) {
        return ONEHUNDO_ERR_BUFFER_TOO_SMALL;
    }

    slot = &ctx->arena.slots[slot_index];
    state = oh_atomic_u32_load_acq(&slot->header.slot_state);
    if (state != ONEHUNDO_SLOT_STATE_WRITING) {
        return ONEHUNDO_ERR_INVALID_STATE;
    }

    slot->header.sequence_number = header->sequence_number ? header->sequence_number : oh_atomic_u64_fetch_add(&ctx->next_seq, 1U);
    slot->header.timestamp_nanos = header->timestamp_nanos;
    slot->header.lifecycle_signal = header->lifecycle_signal ? header->lifecycle_signal : ONEHUNDO_SIGNAL_NORMAL_FRAME;
    slot->header.frame_gap_count = header->frame_gap_count;
    slot->header.payload_size_bytes = header->payload_size_bytes;
    slot->header.crc32 = header->crc32;

    if (slot->header.lifecycle_signal == ONEHUNDO_SIGNAL_FRAME_GAP) {
        uint64_t gaps = header->frame_gap_count ? header->frame_gap_count : 1U;
        oh_atomic_u64_fetch_add(&ctx->frame_gaps_recorded, gaps);
    }

    oh_atomic_u32_store_rel(&slot->header.slot_state, ONEHUNDO_SLOT_STATE_READY);
    oh_atomic_u64_fetch_add(&ctx->frame_snapshots_published, 1U);
    return ONEHUNDO_OK;
}

int32_t onehundo_emit_lifecycle_signal(
    onehundo_context_t *ctx,
    uint32_t signal,
    uint32_t frame_gap_count
) {
    if (!ctx) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    if (signal < ONEHUNDO_SIGNAL_NORMAL_FRAME || signal > ONEHUNDO_SIGNAL_FRAME_GAP) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    if (signal == ONEHUNDO_SIGNAL_FRAME_GAP) {
        uint64_t gaps = frame_gap_count ? frame_gap_count : 1U;
        oh_atomic_u64_fetch_add(&ctx->frame_gaps_recorded, gaps);
    }
    return ONEHUNDO_OK;
}

int32_t onehundo_get_diagnostics(
    const onehundo_context_t *ctx,
    onehundo_diagnostics_t *out_diag,
    size_t diag_size
) {
    uint64_t emitted;
    uint64_t acked;
    uint64_t seq;

    if (!ctx || !out_diag) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    if (diag_size != sizeof(onehundo_diagnostics_t) ||
        out_diag->struct_size != sizeof(onehundo_diagnostics_t)) {
        return ONEHUNDO_ERR_SIZE_MISMATCH;
    }

    emitted = oh_atomic_u64_load_acq(&ctx->events_emitted);
    acked = oh_atomic_u64_load_acq(&ctx->events_acked);
    seq = oh_atomic_u64_load_acq(&ctx->next_seq);

    out_diag->api_version = ONEHUNDO_API_VERSION;
    out_diag->events_emitted = emitted;
    out_diag->events_acked = acked;
    out_diag->events_dropped = oh_atomic_u64_load_acq(&ctx->events_dropped);
    out_diag->frame_snapshots_published = oh_atomic_u64_load_acq(&ctx->frame_snapshots_published);
    out_diag->frame_gaps_recorded = oh_atomic_u64_load_acq(&ctx->frame_gaps_recorded);
    out_diag->last_sequence_number = (seq > 1U) ? (seq - 1U) : 0U;
    out_diag->is_connected = oh_atomic_u32_load_acq(&ctx->is_connected);
    out_diag->pending_queue_depth = (emitted >= acked) ? (uint32_t)(emitted - acked) : 0U;

    return ONEHUNDO_OK;
}

const char *onehundo_strerror(int32_t error_code) {
    switch (error_code) {
        case ONEHUNDO_OK:                       return "Success";
        case ONEHUNDO_ERR_INVALID_ARGUMENT:     return "Invalid argument";
        case ONEHUNDO_ERR_UNSUPPORTED_VERSION:  return "Unsupported API version";
        case ONEHUNDO_ERR_BUFFER_TOO_SMALL:     return "Buffer too small";
        case ONEHUNDO_ERR_QUEUE_FULL:           return "Event queue full";
        case ONEHUNDO_ERR_NOT_CONNECTED:        return "Daemon not connected";
        case ONEHUNDO_ERR_INVALID_STATE:        return "Invalid state transition";
        case ONEHUNDO_ERR_UNSUPPORTED_PLANE:    return "Unsupported ingestion plane";
        case ONEHUNDO_ERR_NO_FREE_SLOT:         return "No free arena snapshot slot";
        case ONEHUNDO_ERR_TRANSPORT_FAILURE:    return "IPC transport failure";
        case ONEHUNDO_ERR_SIZE_MISMATCH:        return "Struct size ABI mismatch";
        default:                                return "Unknown error";
    }
}

const char *onehundo_version_string(void) {
    return "0.1.0";
}

/* ========================================================================= */
/* Stub Inspection & Mock Consumer Implementation                            */
/* ========================================================================= */

bool onehundo_stub_pop_event(
    onehundo_context_t *ctx,
    onehundo_captured_event_t *out_event
) {
    uint32_t r;
    uint32_t w;

    if (!ctx || !out_event) {
        return false;
    }

    r = oh_atomic_u32_load_acq(&ctx->event_read_idx);
    w = oh_atomic_u32_load_acq(&ctx->event_write_idx);
    if (r == w) {
        return false;
    }

    *out_event = ctx->events[r];
    oh_atomic_u32_store_rel(&ctx->event_read_idx, (r + 1U) % ctx->event_ring_cap);
    return true;
}

int32_t onehundo_stub_ack_event(
    onehundo_context_t *ctx,
    uint64_t sequence_number
) {
    if (!ctx || sequence_number == 0) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    oh_atomic_u64_fetch_add(&ctx->events_acked, 1U);
    return ONEHUNDO_OK;
}

int32_t onehundo_stub_consumer_acquire_ready_slot(
    onehundo_context_t *ctx,
    uint32_t *out_slot_index,
    onehundo_snapshot_slot_header_t *out_header,
    const void **out_payload_ptr
) {
    uint32_t i;
    if (!ctx || !out_slot_index || !out_header || !out_payload_ptr) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }

    for (i = 0; i < ctx->arena.slot_count; ++i) {
        uint32_t idx = (ctx->consumer_slot_cursor + i) % ctx->arena.slot_count;
        uint32_t expected = ONEHUNDO_SLOT_STATE_READY;
        if (oh_atomic_u32_cas(&ctx->arena.slots[idx].header.slot_state, &expected, ONEHUNDO_SLOT_STATE_READING)) {
            const onehundo_snapshot_slot_header_t *hdr = &ctx->arena.slots[idx].header;
            ctx->consumer_slot_cursor = (idx + 1U) % ctx->arena.slot_count;
            *out_slot_index = idx;
            /*
             * Copy non-atomic header fields individually so we never perform a
             * non-atomic read of hdr->slot_state while the producer polls CAS.
             */
            out_header->struct_size = hdr->struct_size;
            out_header->api_version = hdr->api_version;
            out_header->slot_index = hdr->slot_index;
            out_header->slot_state = ONEHUNDO_SLOT_STATE_READING;
            out_header->sequence_number = hdr->sequence_number;
            out_header->timestamp_nanos = hdr->timestamp_nanos;
            out_header->lifecycle_signal = hdr->lifecycle_signal;
            out_header->frame_gap_count = hdr->frame_gap_count;
            out_header->payload_size_bytes = hdr->payload_size_bytes;
            out_header->crc32 = hdr->crc32;
            out_header->reserved = hdr->reserved;
            *out_payload_ptr = ctx->arena.slots[idx].payload;
            return ONEHUNDO_OK;
        }
    }

    return ONEHUNDO_ERR_NO_FREE_SLOT;
}

int32_t onehundo_stub_consumer_release_slot(
    onehundo_context_t *ctx,
    uint32_t slot_index
) {
    uint32_t state;
    if (!ctx || slot_index >= ctx->arena.slot_count) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    state = oh_atomic_u32_load_acq(&ctx->arena.slots[slot_index].header.slot_state);
    if (state != ONEHUNDO_SLOT_STATE_READING) {
        return ONEHUNDO_ERR_INVALID_STATE;
    }
    oh_atomic_u32_store_rel(&ctx->arena.slots[slot_index].header.slot_state, ONEHUNDO_SLOT_STATE_FREE);
    return ONEHUNDO_OK;
}
