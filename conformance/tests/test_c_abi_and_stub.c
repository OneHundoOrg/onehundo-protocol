/**
 * @file test_c_abi_and_stub.c
 * @brief Conformance tests for C99 ABI guards, Plane B idempotency, Commit-Before-Ack,
 *        and concurrent Plane A slot handoffs (FREE -> WRITING -> READY -> READING -> FREE).
 */

#define _POSIX_C_SOURCE 200809L

#include "onehundo/onehundo.h"
#include "onehundo_internal.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define CONCURRENT_FRAMES 2000U

typedef struct consumer_thread_args {
    onehundo_context_t *ctx;
    uint32_t expected_frames;
    uint32_t consumed_frames;
    uint64_t last_seq;
} consumer_thread_args_t;

static void *arena_consumer_thread(void *arg) {
    consumer_thread_args_t *args = (consumer_thread_args_t *)arg;
    while (args->consumed_frames < args->expected_frames) {
        uint32_t slot_idx = 0;
        onehundo_snapshot_slot_header_t hdr;
        const void *payload = NULL;
        int32_t rc = onehundo_stub_consumer_acquire_ready_slot(args->ctx, &slot_idx, &hdr, &payload);
        if (rc == ONEHUNDO_OK) {
            const uint32_t *words = (const uint32_t *)payload;
            assert(hdr.payload_size_bytes == 64U);
            assert(words[0] == (uint32_t)hdr.sequence_number);
            assert(words[15] == (uint32_t)(hdr.sequence_number ^ 0xA5A5A5A5U));
            args->last_seq = hdr.sequence_number;
            args->consumed_frames++;
            assert(onehundo_stub_consumer_release_slot(args->ctx, slot_idx) == ONEHUNDO_OK);
        } else {
            struct timespec ts = {0, 1000};
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

static void test_abi_guards(void) {
    onehundo_client_config_t cfg;
    onehundo_context_t *ctx = NULL;
    onehundo_diagnostics_t diag;

    onehundo_client_config_init(&cfg);
    assert(cfg.struct_size == sizeof(cfg));
    assert(cfg.api_version == ONEHUNDO_API_VERSION);

    assert(onehundo_init(NULL, &cfg, sizeof(cfg)) == ONEHUNDO_ERR_INVALID_ARGUMENT);
    assert(onehundo_init(&ctx, NULL, sizeof(cfg)) == ONEHUNDO_ERR_INVALID_ARGUMENT);
    assert(onehundo_init(&ctx, &cfg, sizeof(cfg) - 1U) == ONEHUNDO_ERR_SIZE_MISMATCH);

    cfg.canonical_game_id = "sony.bloodborne";
    cfg.producer_id = "bbport_test";
    cfg.api_version = (99U << 16);
    assert(onehundo_init(&ctx, &cfg, sizeof(cfg)) == ONEHUNDO_ERR_UNSUPPORTED_VERSION);

    cfg.api_version = ONEHUNDO_API_VERSION;
    assert(onehundo_init(&ctx, &cfg, sizeof(cfg)) == ONEHUNDO_OK);
    assert(ctx != NULL);

    memset(&diag, 0, sizeof(diag));
    assert(onehundo_get_diagnostics(ctx, &diag, sizeof(diag)) == ONEHUNDO_ERR_SIZE_MISMATCH);
    diag.struct_size = sizeof(diag);
    assert(onehundo_get_diagnostics(ctx, &diag, sizeof(diag) - 1U) == ONEHUNDO_ERR_SIZE_MISMATCH);
    assert(onehundo_get_diagnostics(ctx, &diag, sizeof(diag)) == ONEHUNDO_OK);
    assert(diag.is_connected == 1U);

    onehundo_shutdown(ctx);
}

static void test_plane_b_and_commit_before_ack(void) {
    onehundo_client_config_t cfg;
    onehundo_context_t *ctx = NULL;
    onehundo_captured_event_t ev;
    onehundo_diagnostics_t diag;

    onehundo_client_config_init(&cfg);
    cfg.canonical_game_id = "sony.bloodborne";
    cfg.producer_id = "bbport_pc";
    cfg.session_id = "550e8400-e29b-41d4-a716-446655440000";
    cfg.ingestion_plane = ONEHUNDO_PLANE_SEMANTIC_EVENT;
    cfg.progression_mode = ONEHUNDO_PROGRESSION_MODE_STANDARD;
    cfg.trust_tier = ONEHUNDO_TRUST_TIER_VERIFIED_INTEGRATION;

    assert(onehundo_init(&ctx, &cfg, sizeof(cfg)) == ONEHUNDO_OK);

    assert(onehundo_stat_set(ctx, "blood_echoes", 5000) == ONEHUNDO_OK);
    assert(onehundo_stat_inc(ctx, "bosses_defeated", 1) == ONEHUNDO_OK);
    assert(onehundo_state_observe(ctx, "zone", "cathedral_ward") == ONEHUNDO_OK);
    assert(onehundo_unlock_request(ctx, "ach_gascoigne") == ONEHUNDO_OK);

    memset(&diag, 0, sizeof(diag));
    diag.struct_size = sizeof(diag);
    assert(onehundo_get_diagnostics(ctx, &diag, sizeof(diag)) == ONEHUNDO_OK);
    assert(diag.events_emitted == 4U);
    assert(diag.events_acked == 0U);
    assert(diag.pending_queue_depth == 4U);

    /* Consume and ack one by one */
    assert(onehundo_stub_pop_event(ctx, &ev));
    assert(ev.event_type == ONEHUNDO_EVENT_STAT_SET);
    assert(ev.sequence_number == 1U);
    assert(strcmp(ev.idempotency_key, "bbport_pc:550e8400-e29b-41d4-a716-446655440000:1") == 0);
    assert(ev.int_value == 5000);
    assert(onehundo_stub_ack_event(ctx, ev.sequence_number) == ONEHUNDO_OK);

    assert(onehundo_stub_pop_event(ctx, &ev));
    assert(ev.event_type == ONEHUNDO_EVENT_STAT_INC);
    assert(ev.sequence_number == 2U);
    assert(onehundo_stub_ack_event(ctx, ev.sequence_number) == ONEHUNDO_OK);

    assert(onehundo_stub_pop_event(ctx, &ev));
    assert(ev.event_type == ONEHUNDO_EVENT_STATE_OBSERVE);
    assert(strcmp(ev.string_value, "cathedral_ward") == 0);
    assert(onehundo_stub_ack_event(ctx, ev.sequence_number) == ONEHUNDO_OK);

    assert(onehundo_stub_pop_event(ctx, &ev));
    assert(ev.event_type == ONEHUNDO_EVENT_UNLOCK_REQUEST);
    assert(strcmp(ev.key, "ach_gascoigne") == 0);
    assert(onehundo_stub_ack_event(ctx, ev.sequence_number) == ONEHUNDO_OK);

    assert(!onehundo_stub_pop_event(ctx, &ev));

    assert(onehundo_get_diagnostics(ctx, &diag, sizeof(diag)) == ONEHUNDO_OK);
    assert(diag.events_acked == 4U);
    assert(diag.pending_queue_depth == 0U);

    onehundo_shutdown(ctx);
}

static void test_plane_a_concurrent_slot_handoff(void) {
    onehundo_client_config_t cfg;
    onehundo_context_t *ctx = NULL;
    onehundo_region_descriptor_t region;
    onehundo_diagnostics_t diag;
    pthread_t consumer;
    consumer_thread_args_t args;
    uint32_t frame;

    onehundo_client_config_init(&cfg);
    cfg.canonical_game_id = "shared.recomp.sample";
    cfg.producer_id = "recomp_harness";
    cfg.ingestion_plane = ONEHUNDO_PLANE_MEMORY_ARENA;

    assert(onehundo_init(&ctx, &cfg, sizeof(cfg)) == ONEHUNDO_OK);

    memset(&region, 0, sizeof(region));
    region.struct_size = sizeof(region);
    region.api_version = ONEHUNDO_API_VERSION;
    region.region_id = 0U;
    region.guest_base_address = 0x80000000ULL;
    region.size_bytes = 4096U;
    region.endianness = ONEHUNDO_ENDIAN_BIG;
    region.coherence = ONEHUNDO_COHERENCE_COHERENT;
    region.region_name = "Main RAM";
    assert(onehundo_arena_register_region(ctx, &region, sizeof(region)) == ONEHUNDO_OK);

    args.ctx = ctx;
    args.expected_frames = CONCURRENT_FRAMES;
    args.consumed_frames = 0U;
    args.last_seq = 0U;
    assert(pthread_create(&consumer, NULL, arena_consumer_thread, &args) == 0);

    for (frame = 1U; frame <= CONCURRENT_FRAMES; ) {
        uint32_t slot_idx = 0U;
        void *buf = NULL;
        size_t max_sz = 0U;
        int32_t rc = onehundo_arena_acquire_slot(ctx, &slot_idx, &buf, &max_sz);
        if (rc == ONEHUNDO_OK) {
            uint32_t *words = (uint32_t *)buf;
            onehundo_snapshot_slot_header_t hdr;
            assert(max_sz >= 64U);
            words[0] = frame;
            words[15] = frame ^ 0xA5A5A5A5U;

            memset(&hdr, 0, sizeof(hdr));
            hdr.struct_size = sizeof(hdr);
            hdr.api_version = ONEHUNDO_API_VERSION;
            hdr.sequence_number = frame;
            hdr.timestamp_nanos = (uint64_t)frame * 16666667ULL;
            hdr.lifecycle_signal = ONEHUNDO_SIGNAL_NORMAL_FRAME;
            hdr.payload_size_bytes = 64U;

            assert(onehundo_arena_publish_slot(ctx, slot_idx, &hdr) == ONEHUNDO_OK);
            frame++;
        } else {
            assert(rc == ONEHUNDO_ERR_NO_FREE_SLOT);
            assert(onehundo_emit_lifecycle_signal(ctx, ONEHUNDO_SIGNAL_FRAME_GAP, 1U) == ONEHUNDO_OK);
            {
                struct timespec ts = {0, 1000};
                nanosleep(&ts, NULL);
            }
        }
    }

    assert(pthread_join(consumer, NULL) == 0);
    assert(args.consumed_frames == CONCURRENT_FRAMES);

    memset(&diag, 0, sizeof(diag));
    diag.struct_size = sizeof(diag);
    assert(onehundo_get_diagnostics(ctx, &diag, sizeof(diag)) == ONEHUNDO_OK);
    assert(diag.frame_snapshots_published == CONCURRENT_FRAMES);

    onehundo_shutdown(ctx);
}

int main(void) {
    test_abi_guards();
    test_plane_b_and_commit_before_ack();
    test_plane_a_concurrent_slot_handoff();
    printf("All C ABI, stub, and concurrent Plane A handoff tests PASSED.\n");
    return 0;
}
