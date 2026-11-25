/**
 * @file transport_posix.c
 * @brief POSIX reference transport (Unix domain socket + POSIX shm_open arena).
 *
 * Implements the OneHundo C99 public API over AF_UNIX stream sockets for control/
 * semantic events and POSIX shared memory (shm_open + mmap) for Plane A arena slots.
 * Enforces release/acquire slot ownership transitions and Commit-Before-Ack tracking.
 */

#define _POSIX_C_SOURCE 200809L

#include "onehundo_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

struct onehundo_context {
    char canonical_game_id[128];
    char producer_id[128];
    char session_id[64];
    char socket_path[108];
    char shm_name[64];
    uint32_t ingestion_plane;
    uint32_t progression_mode;
    uint32_t trust_tier;

    int sock_fd;
    int shm_fd;
    onehundo_arena_shm_layout_t *shm_arena;
    bool owns_shm;
    uint32_t producer_slot_cursor;

    volatile uint64_t next_seq;
    volatile uint64_t events_emitted;
    volatile uint64_t events_acked;
    volatile uint64_t events_dropped;
    volatile uint64_t frame_snapshots_published;
    volatile uint64_t frame_gaps_recorded;
    volatile uint32_t is_connected;
    uint8_t rx_buf[8192];
    size_t rx_len;
};

static void posix_safe_strcpy(char *dst, size_t dst_cap, const char *src) {
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

static void posix_resolve_socket_path(
    char *dst,
    size_t dst_cap,
    const char *configured_path
) {
    const char *env_sock;
    const char *xdg_runtime;
    char candidate[108];
    int n;
    struct stat st;

    if (configured_path && configured_path[0] != '\0') {
        posix_safe_strcpy(dst, dst_cap, configured_path);
        return;
    }

    env_sock = getenv("ONEHUNDO_SOCKET");
    if (env_sock && env_sock[0] != '\0') {
        posix_safe_strcpy(dst, dst_cap, env_sock);
        return;
    }

    xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime && xdg_runtime[0] != '\0') {
        size_t xlen = strlen(xdg_runtime);
        const char *sep = (xdg_runtime[xlen - 1U] == '/') ? "" : "/";
        n = snprintf(candidate, sizeof(candidate), "%s%sonehundo.sock", xdg_runtime, sep);
        if (n > 0 && (size_t)n < sizeof(candidate)) {
            if (stat(candidate, &st) == 0 && S_ISSOCK(st.st_mode)) {
                posix_safe_strcpy(dst, dst_cap, candidate);
                return;
            }
        }
    }

    posix_safe_strcpy(dst, dst_cap, "/tmp/onehundo.sock");
}

static void posix_disconnect(onehundo_context_t *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->sock_fd >= 0) {
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
    }
    ctx->rx_len = 0U;
    oh_atomic_u32_store_rel(&ctx->is_connected, 0U);
}

static int32_t posix_write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
#else
        ssize_t n = write(fd, p + sent, len - sent);
#endif
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return ONEHUNDO_ERR_QUEUE_FULL;
            }
            return ONEHUNDO_ERR_TRANSPORT_FAILURE;
        }
        sent += (size_t)n;
    }
    return ONEHUNDO_OK;
}

static int32_t posix_send_frame(
    onehundo_context_t *ctx,
    uint16_t msg_type,
    uint64_t seq,
    const void *payload,
    uint32_t payload_len
) {
    onehundo_wire_header_t hdr;
    int32_t rc;

    if (!ctx || ctx->sock_fd < 0) {
        return ONEHUNDO_ERR_NOT_CONNECTED;
    }

    hdr.magic = ONEHUNDO_WIRE_MAGIC;
    hdr.version = ONEHUNDO_WIRE_VERSION;
    hdr.msg_type = msg_type;
    hdr.flags = 0U;
    hdr.payload_length = payload_len;
    hdr.sequence_number = seq;

    rc = posix_write_all(ctx->sock_fd, &hdr, sizeof(hdr));
    if (rc == ONEHUNDO_OK && payload && payload_len > 0U) {
        rc = posix_write_all(ctx->sock_fd, payload, payload_len);
    }
    if (rc == ONEHUNDO_ERR_TRANSPORT_FAILURE) {
        posix_disconnect(ctx);
    }
    return rc;
}

static void posix_poll_acks(onehundo_context_t *ctx) {
    if (!ctx || ctx->sock_fd < 0) {
        return;
    }

    for (;;) {
        size_t space = sizeof(ctx->rx_buf) - ctx->rx_len;
        ssize_t n = 0;

        if (space > 0U) {
            n = recv(ctx->sock_fd, ctx->rx_buf + ctx->rx_len, space, MSG_DONTWAIT);
            if (n == 0) {
                posix_disconnect(ctx);
                break;
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    posix_disconnect(ctx);
                    break;
                }
                n = 0;
            } else {
                ctx->rx_len += (size_t)n;
            }
        }

        while (ctx->rx_len >= sizeof(onehundo_wire_header_t)) {
            onehundo_wire_header_t hdr;
            size_t frame_len;
            memcpy(&hdr, ctx->rx_buf, sizeof(hdr));
            if (hdr.magic != ONEHUNDO_WIRE_MAGIC || hdr.payload_length > 4096U) {
                posix_disconnect(ctx);
                return;
            }
            frame_len = sizeof(hdr) + (size_t)hdr.payload_length;
            if (ctx->rx_len < frame_len) {
                break;
            }
            if (hdr.msg_type == ONEHUNDO_MSG_EVENT_ACK) {
                oh_atomic_u64_fetch_add(&ctx->events_acked, 1U);
            }
            if (ctx->rx_len > frame_len) {
                memmove(ctx->rx_buf, ctx->rx_buf + frame_len, ctx->rx_len - frame_len);
            }
            ctx->rx_len -= frame_len;
        }

        if (n <= 0) {
            break;
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
    struct sockaddr_un addr;
    char hello_json[768];
    int hello_len;
    int flags;
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

    ctx->sock_fd = -1;
    ctx->shm_fd = -1;
    posix_safe_strcpy(ctx->canonical_game_id, sizeof(ctx->canonical_game_id), config->canonical_game_id);
    posix_safe_strcpy(ctx->producer_id, sizeof(ctx->producer_id), config->producer_id);
    if (config->session_id && config->session_id[0] != '\0') {
        posix_safe_strcpy(ctx->session_id, sizeof(ctx->session_id), config->session_id);
    } else {
        posix_safe_strcpy(ctx->session_id, sizeof(ctx->session_id), "00000000-0000-4000-8000-000000000001");
    }

    ctx->ingestion_plane = config->ingestion_plane;
    ctx->progression_mode = config->progression_mode ? config->progression_mode : ONEHUNDO_PROGRESSION_MODE_STANDARD;
    ctx->trust_tier = config->trust_tier ? config->trust_tier : ONEHUNDO_TRUST_TIER_LOCAL;
    ctx->next_seq = 1U;

    posix_resolve_socket_path(ctx->socket_path, sizeof(ctx->socket_path), config->ipc_endpoint_path);

    /* Connect Unix domain socket if daemon is listening; stay resilient if offline */
    ctx->sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx->sock_fd >= 0) {
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        posix_safe_strcpy(addr.sun_path, sizeof(addr.sun_path), ctx->socket_path);
        if (connect(ctx->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
#ifdef SO_NOSIGPIPE
            int set = 1;
            (void)setsockopt(ctx->sock_fd, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));
#endif
            flags = fcntl(ctx->sock_fd, F_GETFL, 0);
            if (flags >= 0) {
                (void)fcntl(ctx->sock_fd, F_SETFL, flags | O_NONBLOCK);
            }
            oh_atomic_u32_store_rel(&ctx->is_connected, 1U);
            hello_len = snprintf(
                hello_json,
                sizeof(hello_json),
                "{\"canonical_game_id\":\"%s\",\"producer_id\":\"%s\",\"session_id\":\"%s\",\"plane\":%u,\"progression_mode\":%u,\"trust_tier\":%u}",
                ctx->canonical_game_id,
                ctx->producer_id,
                ctx->session_id,
                (unsigned)ctx->ingestion_plane,
                (unsigned)ctx->progression_mode,
                (unsigned)ctx->trust_tier
            );
            if (hello_len > 0 && (size_t)hello_len < sizeof(hello_json)) {
                (void)posix_send_frame(ctx, ONEHUNDO_MSG_HELLO, 0U, hello_json, (uint32_t)hello_len);
            }
        } else {
            close(ctx->sock_fd);
            ctx->sock_fd = -1;
        }
    }

    /* Initialize POSIX shared memory ring for Plane A */
    if (ctx->ingestion_plane == ONEHUNDO_PLANE_MEMORY_ARENA) {
        snprintf(ctx->shm_name, sizeof(ctx->shm_name), "/onehundo_arena_%ld", (long)getpid());
        ctx->shm_fd = shm_open(ctx->shm_name, O_CREAT | O_RDWR, 0600);
        if (ctx->shm_fd >= 0) {
            if (ftruncate(ctx->shm_fd, (off_t)sizeof(onehundo_arena_shm_layout_t)) == 0) {
                void *mapped = mmap(
                    NULL,
                    sizeof(onehundo_arena_shm_layout_t),
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    ctx->shm_fd,
                    0
                );
                if (mapped != MAP_FAILED) {
                    ctx->shm_arena = (onehundo_arena_shm_layout_t *)mapped;
                    ctx->owns_shm = true;
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
    }

    *out_ctx = ctx;
    return ONEHUNDO_OK;
}

void onehundo_shutdown(onehundo_context_t *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->sock_fd >= 0) {
        (void)posix_send_frame(ctx, ONEHUNDO_MSG_GOODBYE, 0U, NULL, 0U);
        posix_disconnect(ctx);
    }
    if (ctx->shm_arena) {
        munmap(ctx->shm_arena, sizeof(onehundo_arena_shm_layout_t));
        ctx->shm_arena = NULL;
    }
    if (ctx->shm_fd >= 0) {
        close(ctx->shm_fd);
        ctx->shm_fd = -1;
    }
    if (ctx->owns_shm && ctx->shm_name[0] != '\0') {
        shm_unlink(ctx->shm_name);
    }
    oh_atomic_u32_store_rel(&ctx->is_connected, 0U);
    free(ctx);
}

static int32_t posix_emit_event(
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

    posix_poll_acks(ctx);
    seq = oh_atomic_u64_fetch_add(&ctx->next_seq, 1U);
    oh_atomic_u64_fetch_add(&ctx->events_emitted, 1U);

    if (str_val) {
        len = snprintf(
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
        len = snprintf(
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

    if (ctx->sock_fd < 0) {
        oh_atomic_u64_fetch_add(&ctx->events_dropped, 1U);
        return ONEHUNDO_ERR_NOT_CONNECTED;
    }

    rc = posix_send_frame(ctx, ONEHUNDO_MSG_EVENT_PUBLISH, seq, payload, (uint32_t)len);
    if (rc != ONEHUNDO_OK) {
        oh_atomic_u64_fetch_add(&ctx->events_dropped, 1U);
    }
    return rc;
}

int32_t onehundo_stat_set(onehundo_context_t *ctx, const char *stat_key, int64_t value) {
    return posix_emit_event(ctx, "STAT_SET", stat_key, value, NULL);
}

int32_t onehundo_stat_inc(onehundo_context_t *ctx, const char *stat_key, int64_t delta) {
    return posix_emit_event(ctx, "STAT_INC", stat_key, delta, NULL);
}

int32_t onehundo_state_observe(onehundo_context_t *ctx, const char *state_key, const char *state_value) {
    if (!state_value) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    return posix_emit_event(ctx, "STATE_OBSERVE", state_key, 0, state_value);
}

int32_t onehundo_unlock_request(onehundo_context_t *ctx, const char *achievement_id) {
    return posix_emit_event(ctx, "UNLOCK_REQUEST", achievement_id, 1, NULL);
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
    if (ctx->sock_fd < 0) {
        return ONEHUNDO_ERR_NOT_CONNECTED;
    }

    len = snprintf(
        payload,
        sizeof(payload),
        "{\"shm_name\":\"%s\",\"region_id\":%u,\"guest_base_address\":\"0x%llx\",\"size_bytes\":%llu,\"endianness\":%u}",
        ctx->shm_name,
        (unsigned)desc->region_id,
        (unsigned long long)desc->guest_base_address,
        (unsigned long long)desc->size_bytes,
        (unsigned)desc->endianness
    );
    if (len < 0) {
        return ONEHUNDO_ERR_BUFFER_TOO_SMALL;
    }

    return posix_send_frame(ctx, ONEHUNDO_MSG_ARENA_REGISTER, 0U, payload, (uint32_t)len);
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

    slot->header.sequence_number = header->sequence_number ? header->sequence_number : oh_atomic_u64_fetch_add(&ctx->next_seq, 1U);
    slot->header.timestamp_nanos = header->timestamp_nanos;
    slot->header.lifecycle_signal = header->lifecycle_signal ? header->lifecycle_signal : ONEHUNDO_SIGNAL_NORMAL_FRAME;
    slot->header.frame_gap_count = header->frame_gap_count;
    slot->header.payload_size_bytes = header->payload_size_bytes;
    slot->header.crc32 = header->crc32;

    if (slot->header.lifecycle_signal == ONEHUNDO_SIGNAL_FRAME_GAP) {
        oh_atomic_u64_fetch_add(&ctx->frame_gaps_recorded, header->frame_gap_count ? header->frame_gap_count : 1U);
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
    char payload[128];
    int len;

    if (!ctx) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    if (signal < ONEHUNDO_SIGNAL_NORMAL_FRAME || signal > ONEHUNDO_SIGNAL_FRAME_GAP) {
        return ONEHUNDO_ERR_INVALID_ARGUMENT;
    }
    if (signal == ONEHUNDO_SIGNAL_FRAME_GAP) {
        oh_atomic_u64_fetch_add(&ctx->frame_gaps_recorded, frame_gap_count ? frame_gap_count : 1U);
    }
    if (ctx->sock_fd < 0) {
        return ONEHUNDO_ERR_NOT_CONNECTED;
    }

    len = snprintf(payload, sizeof(payload), "{\"signal\":%u,\"frame_gap_count\":%u}", (unsigned)signal, (unsigned)frame_gap_count);
    return posix_send_frame(ctx, ONEHUNDO_MSG_LIFECYCLE_SIGNAL, 0U, payload, (uint32_t)len);
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

    posix_poll_acks((onehundo_context_t *)ctx);

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
