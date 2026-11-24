/**
 * @file transport_win32.c
 * @brief Windows Win32 reference transport (Named Pipe + CreateFileMappingW arena).
 *
 * Implements the OneHundo C99 public API over \\.\pipe\onehundo for control and
 * semantic events, and Win32 named file mappings for Plane A shared memory rings.
 */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "onehundo_internal.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct onehundo_context {
    char canonical_game_id[128];
    char producer_id[128];
    char session_id[64];
    char pipe_path[256];
    wchar_t shm_name_w[128];
    uint32_t ingestion_plane;
    uint32_t progression_mode;
    uint32_t trust_tier;

    HANDLE pipe_handle;
    HANDLE shm_handle;
    onehundo_arena_shm_layout_t *shm_arena;
    uint32_t producer_slot_cursor;

    volatile uint64_t next_seq;
    volatile uint64_t events_emitted;
    volatile uint64_t events_acked;
    volatile uint64_t events_dropped;
    volatile uint64_t frame_snapshots_published;
    volatile uint64_t frame_gaps_recorded;
    volatile uint32_t is_connected;
};

static void win32_safe_strcpy(char *dst, size_t dst_cap, const char *src) {
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

static int32_t win32_send_frame(
    onehundo_context_t *ctx,
    uint16_t msg_type,
    uint64_t seq,
    const void *payload,
    uint32_t payload_len
) {
    onehundo_wire_header_t hdr;
    DWORD written = 0;

    if (!ctx || ctx->pipe_handle == INVALID_HANDLE_VALUE) {
        return ONEHUNDO_ERR_NOT_CONNECTED;
    }

    hdr.magic = ONEHUNDO_WIRE_MAGIC;
    hdr.version = ONEHUNDO_WIRE_VERSION;
    hdr.msg_type = msg_type;
    hdr.flags = 0U;
    hdr.payload_length = payload_len;
    hdr.sequence_number = seq;

    if (!WriteFile(ctx->pipe_handle, &hdr, (DWORD)sizeof(hdr), &written, NULL) ||
        written != sizeof(hdr)) {
        return ONEHUNDO_ERR_TRANSPORT_FAILURE;
    }
    if (payload && payload_len > 0U) {
        if (!WriteFile(ctx->pipe_handle, payload, (DWORD)payload_len, &written, NULL) ||
            written != payload_len) {
            return ONEHUNDO_ERR_TRANSPORT_FAILURE;
        }
    }
    return ONEHUNDO_OK;
}

static void win32_poll_acks(onehundo_context_t *ctx) {
    DWORD avail = 0;
    onehundo_wire_header_t hdr;
    DWORD read_bytes = 0;

    if (!ctx || ctx->pipe_handle == INVALID_HANDLE_VALUE) {
        return;
    }

    while (PeekNamedPipe(ctx->pipe_handle, NULL, 0, NULL, &avail, NULL) &&
           avail >= sizeof(onehundo_wire_header_t)) {
        if (!ReadFile(ctx->pipe_handle, &hdr, (DWORD)sizeof(hdr), &read_bytes, NULL) ||
            read_bytes != sizeof(hdr)) {
            break;
        }
        if (hdr.magic == ONEHUNDO_WIRE_MAGIC) {
            if (hdr.payload_length > 0U && hdr.payload_length <= 4096U) {
                uint8_t discard[4096];
                DWORD discard_read = 0;
                (void)ReadFile(ctx->pipe_handle, discard, (DWORD)hdr.payload_length, &discard_read, NULL);
            }
            if (hdr.msg_type == ONEHUNDO_MSG_EVENT_ACK) {
                InterlockedIncrement64((volatile LONG64 *)&ctx->events_acked);
            }
        }
    }
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
    wchar_t pipe_w[256];
    char hello_json[512];
    int hello_len;
    DWORD mode;
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

    ctx->pipe_handle = INVALID_HANDLE_VALUE;
    ctx->shm_handle = NULL;
    win32_safe_strcpy(ctx->canonical_game_id, sizeof(ctx->canonical_game_id), config->canonical_game_id);
    win32_safe_strcpy(ctx->producer_id, sizeof(ctx->producer_id), config->producer_id);
    if (config->session_id && config->session_id[0] != '\0') {
        win32_safe_strcpy(ctx->session_id, sizeof(ctx->session_id), config->session_id);
    } else {
        win32_safe_strcpy(ctx->session_id, sizeof(ctx->session_id), "00000000-0000-4000-8000-000000000001");
    }

    ctx->ingestion_plane = config->ingestion_plane;
    ctx->progression_mode = config->progression_mode ? config->progression_mode : ONEHUNDO_PROGRESSION_MODE_STANDARD;
    ctx->trust_tier = config->trust_tier ? config->trust_tier : ONEHUNDO_TRUST_TIER_LOCAL;
    ctx->next_seq = 1U;

    if (config->ipc_endpoint_path && config->ipc_endpoint_path[0] != '\0') {
        win32_safe_strcpy(ctx->pipe_path, sizeof(ctx->pipe_path), config->ipc_endpoint_path);
    } else {
        win32_safe_strcpy(ctx->pipe_path, sizeof(ctx->pipe_path), "\\\\.\\pipe\\onehundo");
    }

    MultiByteToWideChar(CP_UTF8, 0, ctx->pipe_path, -1, pipe_w, (int)(sizeof(pipe_w) / sizeof(pipe_w[0])));
    ctx->pipe_handle = CreateFileW(
        pipe_w,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    if (ctx->pipe_handle != INVALID_HANDLE_VALUE) {
        mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
        (void)SetNamedPipeHandleState(ctx->pipe_handle, &mode, NULL, NULL);
        ctx->is_connected = 1U;
        hello_len = _snprintf(
            hello_json,
            sizeof(hello_json),
            "{\"canonical_game_id\":\"%s\",\"producer_id\":\"%s\",\"session_id\":\"%s\",\"plane\":%u}",
            ctx->canonical_game_id,
            ctx->producer_id,
            ctx->session_id,
            (unsigned)ctx->ingestion_plane
        );
        if (hello_len > 0) {
            (void)win32_send_frame(ctx, ONEHUNDO_MSG_HELLO, 0U, hello_json, (uint32_t)hello_len);
        }
    }

    if (ctx->ingestion_plane == ONEHUNDO_PLANE_MEMORY_ARENA) {
        _snwprintf(
            ctx->shm_name_w,
            sizeof(ctx->shm_name_w) / sizeof(ctx->shm_name_w[0]),
            L"Local\\onehundo_arena_%lu",
            (unsigned long)GetCurrentProcessId()
        );
        ctx->shm_handle = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            NULL,
            PAGE_READWRITE,
            0,
            (DWORD)sizeof(onehundo_arena_shm_layout_t),
            ctx->shm_name_w
        );
        if (ctx->shm_handle != NULL) {
            ctx->shm_arena = (onehundo_arena_shm_layout_t *)MapViewOfFile(
                ctx->shm_handle,
                FILE_MAP_ALL_ACCESS,
                0,
                0,
                sizeof(onehundo_arena_shm_layout_t)
            );
            if (ctx->shm_arena) {
                memset(ctx->shm_arena, 0, sizeof(*ctx->shm_arena));
                ctx->shm_arena->magic = ONEHUNDO_WIRE_MAGIC;
                ctx->shm_arena->version = ONEHUNDO_WIRE_VERSION;
                ctx->shm_arena->slot_count = ONEHUNDO_RING_SLOT_COUNT;
                ctx->shm_arena->slot_capacity = ONEHUNDO_RING_SLOT_CAPACITY;
                for (i = 0; i < ONEHUNDO_RING_SLOT_COUNT; ++i) {
                    ctx->shm_arena->slots[i].header.struct_size = (uint32_t)sizeof(onehundo_snapshot_slot_header_t);
                    ctx->shm_arena->slots[i].header.api_version = ONEHUNDO_API_VERSION;
                    ctx->shm_arena->slots[i].header.slot_index = i;
                    oh_atomic_u32_store_rel(&ctx->shm_arena->slots[i].header.slot_state, ONEHUNDO_SLOT_STATE_FREE);
                }
            }
        }
    }

    *out_ctx = ctx;
    return ONEHUNDO_OK;
}

void onehundo_shutdown(onehundo_context_t *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->pipe_handle != INVALID_HANDLE_VALUE) {
        (void)win32_send_frame(ctx, ONEHUNDO_MSG_GOODBYE, 0U, NULL, 0U);
        CloseHandle(ctx->pipe_handle);
        ctx->pipe_handle = INVALID_HANDLE_VALUE;
    }
    if (ctx->shm_arena) {
        UnmapViewOfFile(ctx->shm_arena);
        ctx->shm_arena = NULL;
    }
    if (ctx->shm_handle) {
        CloseHandle(ctx->shm_handle);
        ctx->shm_handle = NULL;
    }
    ctx->is_connected = 0U;
    free(ctx);
}

static int32_t win32_emit_event(
    onehundo_context_t *ctx,
    const char *event_type_str,
    const char *key,
    int64_t int_val,
    const char *str_val
) {
    char payload[768];
    int len;
    uint64_t seq;
    int32_t rc;

    if (!ctx || !key || key[0] == '\0') {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }

    win32_poll_acks(ctx);
    seq = (uint64_t)InterlockedIncrement64((volatile LONG64 *)&ctx->next_seq) - 1ULL;
    InterlockedIncrement64((volatile LONG64 *)&ctx->events_emitted);

    if (str_val) {
        len = _snprintf(
            payload,
            sizeof(payload),
            "{\"event_type\":\"%s\",\"canonical_game_id\":\"%s\",\"producer_id\":\"%s\","
            "\"session_id\":\"%s\",\"sequence_number\":%llu,\"key\":\"%s\",\"string_value\":\"%s\"}",
            event_type_str,
            ctx->canonical_game_id,
            ctx->producer_id,
            ctx->session_id,
            (unsigned long long)seq,
            key,
            str_val
        );
    } else {
        len = _snprintf(
            payload,
            sizeof(payload),
            "{\"event_type\":\"%s\",\"canonical_game_id\":\"%s\",\"producer_id\":\"%s\","
            "\"session_id\":\"%s\",\"sequence_number\":%llu,\"key\":\"%s\",\"int_value\":%lld}",
            event_type_str,
            ctx->canonical_game_id,
            ctx->producer_id,
            ctx->session_id,
            (unsigned long long)seq,
            key,
            (long long)int_val
        );
    }

    if (len < 0 || (size_t)len >= sizeof(payload)) {
        return ONEHUNDO_ERR_BUFFER_TOO_SMALL;
    }
    if (ctx->pipe_handle == INVALID_HANDLE_VALUE) {
        return ONEHUNDO_ERR_NOT_CONNECTED;
    }

    rc = win32_send_frame(ctx, ONEHUNDO_MSG_EVENT_PUBLISH, seq, payload, (uint32_t)len);
    if (rc != ONEHUNDO_OK) {
        InterlockedIncrement64((volatile LONG64 *)&ctx->events_dropped);
    }
    return rc;
}

int32_t onehundo_stat_set(onehundo_context_t *ctx, const char *stat_key, int64_t value) {
    return win32_emit_event(ctx, "STAT_SET", stat_key, value, NULL);
}

int32_t onehundo_stat_inc(onehundo_context_t *ctx, const char *stat_key, int64_t delta) {
    return win32_emit_event(ctx, "STAT_INC", stat_key, delta, NULL);
}

int32_t onehundo_state_observe(onehundo_context_t *ctx, const char *state_key, const char *state_value) {
    if (!state_value) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    return win32_emit_event(ctx, "STATE_OBSERVE", state_key, 0, state_value);
}

int32_t onehundo_unlock_request(onehundo_context_t *ctx, const char *achievement_id) {
    return win32_emit_event(ctx, "UNLOCK_REQUEST", achievement_id, 1, NULL);
}

int32_t onehundo_arena_register_region(
    onehundo_context_t *ctx,
    const onehundo_region_descriptor_t *desc,
    size_t desc_size
) {
    char payload[512];
    int len;
    if (!ctx || !desc) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    if (desc_size != sizeof(onehundo_region_descriptor_t) ||
        desc->struct_size != sizeof(onehundo_region_descriptor_t)) {
        return ONEHUNDO_ERR_SIZE_MISMATCH;
    }
    if (ctx->pipe_handle == INVALID_HANDLE_VALUE) {
        return ONEHUNDO_ERR_NOT_CONNECTED;
    }
    len = _snprintf(
        payload,
        sizeof(payload),
        "{\"region_id\":%u,\"guest_base_address\":\"0x%llx\",\"size_bytes\":%llu,\"endianness\":%u}",
        (unsigned)desc->region_id,
        (unsigned long long)desc->guest_base_address,
        (unsigned long long)desc->size_bytes,
        (unsigned)desc->endianness
    );
    if (len < 0) {
        return ONEHUNDO_ERR_BUFFER_TOO_SMALL;
    }
    return win32_send_frame(ctx, ONEHUNDO_MSG_ARENA_REGISTER, 0U, payload, (uint32_t)len);
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
    if (!ctx->shm_arena) {
        return ONEHUNDO_ERR_UNSUPPORTED_PLANE;
    }
    for (i = 0; i < ctx->shm_arena->slot_count; ++i) {
        uint32_t idx = (ctx->producer_slot_cursor + i) % ctx->shm_arena->slot_count;
        uint32_t expected = ONEHUNDO_SLOT_STATE_FREE;
        if (oh_atomic_u32_cas(&ctx->shm_arena->slots[idx].header.slot_state, &expected, ONEHUNDO_SLOT_STATE_WRITING)) {
            ctx->producer_slot_cursor = (idx + 1U) % ctx->shm_arena->slot_count;
            *out_slot_index = idx;
            *out_buffer_ptr = ctx->shm_arena->slots[idx].payload;
            *out_max_size = ctx->shm_arena->slot_capacity;
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
    if (!ctx || !header) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    if (!ctx->shm_arena) {
        return ONEHUNDO_ERR_UNSUPPORTED_PLANE;
    }
    if (slot_index >= ctx->shm_arena->slot_count) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    if (header->struct_size != sizeof(onehundo_snapshot_slot_header_t)) {
        return ONEHUNDO_ERR_SIZE_MISMATCH;
    }
    slot = &ctx->shm_arena->slots[slot_index];
    if (oh_atomic_u32_load_acq(&slot->header.slot_state) != ONEHUNDO_SLOT_STATE_WRITING) {
        return ONEHUNDO_ERR_INVALID_STATE;
    }
    slot->header.sequence_number = header->sequence_number;
    slot->header.timestamp_nanos = header->timestamp_nanos;
    slot->header.lifecycle_signal = header->lifecycle_signal ? header->lifecycle_signal : ONEHUNDO_SIGNAL_NORMAL_FRAME;
    slot->header.frame_gap_count = header->frame_gap_count;
    slot->header.payload_size_bytes = header->payload_size_bytes;
    slot->header.crc32 = header->crc32;
    oh_atomic_u32_store_rel(&slot->header.slot_state, ONEHUNDO_SLOT_STATE_READY);
    InterlockedIncrement64((volatile LONG64 *)&ctx->frame_snapshots_published);
    return ONEHUNDO_OK;
}

int32_t onehundo_emit_lifecycle_signal(onehundo_context_t *ctx, uint32_t signal, uint32_t frame_gap_count) {
    char payload[128];
    int len;
    if (!ctx) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    if (signal < ONEHUNDO_SIGNAL_NORMAL_FRAME || signal > ONEHUNDO_SIGNAL_FRAME_GAP) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    if (ctx->pipe_handle == INVALID_HANDLE_VALUE) {
        return ONEHUNDO_ERR_NOT_CONNECTED;
    }
    len = _snprintf(payload, sizeof(payload), "{\"signal\":%u,\"frame_gap_count\":%u}", (unsigned)signal, (unsigned)frame_gap_count);
    return win32_send_frame(ctx, ONEHUNDO_MSG_LIFECYCLE_SIGNAL, 0U, payload, (uint32_t)len);
}

int32_t onehundo_get_diagnostics(const onehundo_context_t *ctx, onehundo_diagnostics_t *out_diag, size_t diag_size) {
    if (!ctx || !out_diag) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    if (diag_size != sizeof(onehundo_diagnostics_t) ||
        out_diag->struct_size != sizeof(onehundo_diagnostics_t)) {
        return ONEHUNDO_ERR_SIZE_MISMATCH;
    }
    win32_poll_acks((onehundo_context_t *)ctx);
    out_diag->api_version = ONEHUNDO_API_VERSION;
    out_diag->events_emitted = ctx->events_emitted;
    out_diag->events_acked = ctx->events_acked;
    out_diag->events_dropped = ctx->events_dropped;
    out_diag->frame_snapshots_published = ctx->frame_snapshots_published;
    out_diag->frame_gaps_recorded = ctx->frame_gaps_recorded;
    out_diag->last_sequence_number = (ctx->next_seq > 1U) ? (ctx->next_seq - 1U) : 0U;
    out_diag->is_connected = ctx->is_connected;
    out_diag->pending_queue_depth = (ctx->events_emitted >= ctx->events_acked) ? (uint32_t)(ctx->events_emitted - ctx->events_acked) : 0U;
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

#else
/* Non-Windows translation unit anchor so -pedantic compiles cleanly */
typedef int onehundo_win32_stub_translation_unit_t;
#endif /* _WIN32 */
