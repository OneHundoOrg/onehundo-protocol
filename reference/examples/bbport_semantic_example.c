/**
 * @file bbport_semantic_example.c
 * @brief Reference Plane B semantic integration example for a native source port (sony.bloodborne / bbport).
 *
 * Demonstrates non-blocking gameplay event emission, state observations,
 * direct unlock requests, and runtime diagnostics inspection.
 */

#include "onehundo/onehundo.h"

#include <stdio.h>
#include <string.h>

static void check_rc(const char *op, int32_t rc) {
    if (rc != ONEHUNDO_OK) {
        fprintf(stderr, "[bbport] %s failed: %s (%d)\n", op, onehundo_strerror(rc), rc);
    }
}

int main(void) {
    onehundo_client_config_t cfg;
    onehundo_context_t *ctx = NULL;
    onehundo_diagnostics_t diag;
    int32_t rc;

    onehundo_client_config_init(&cfg);
    cfg.canonical_game_id = "sony.bloodborne";
    cfg.producer_id = "bbport_pc_x64";
    cfg.session_id = "550e8400-e29b-41d4-a716-446655440000";
    cfg.ingestion_plane = ONEHUNDO_PLANE_SEMANTIC_EVENT;
    cfg.progression_mode = ONEHUNDO_PROGRESSION_MODE_STANDARD;
    cfg.trust_tier = ONEHUNDO_TRUST_TIER_VERIFIED_INTEGRATION;

    rc = onehundo_init(&ctx, &cfg, sizeof(cfg));
    if (rc != ONEHUNDO_OK || !ctx) {
        fprintf(stderr, "Failed to initialize OneHundo SDK: %s\n", onehundo_strerror(rc));
        return 1;
    }

    printf("OneHundo SDK v%s initialized for %s\n", onehundo_version_string(), cfg.canonical_game_id);

    /* Emit non-blocking gameplay telemetry from main loop */
    check_rc("state_observe(zone)", onehundo_state_observe(ctx, "current_zone", "central_yharnam"));
    check_rc("stat_set(blood_echoes)", onehundo_stat_set(ctx, "blood_echoes_held", 4250));
    check_rc("stat_inc(enemies_slain)", onehundo_stat_inc(ctx, "enemies_slain", 12));
    check_rc("state_observe(boss)", onehundo_state_observe(ctx, "boss_encounter", "cleric_beast"));
    check_rc("stat_inc(bosses_defeated)", onehundo_stat_inc(ctx, "bosses_defeated", 1));
    check_rc("unlock_request(cleric_beast)", onehundo_unlock_request(ctx, "ach_cleric_beast"));

    memset(&diag, 0, sizeof(diag));
    diag.struct_size = (uint32_t)sizeof(diag);
    if (onehundo_get_diagnostics(ctx, &diag, sizeof(diag)) == ONEHUNDO_OK) {
        printf(
            "Diagnostics: emitted=%llu acked=%llu dropped=%llu last_seq=%llu pending=%u\n",
            (unsigned long long)diag.events_emitted,
            (unsigned long long)diag.events_acked,
            (unsigned long long)diag.events_dropped,
            (unsigned long long)diag.last_sequence_number,
            (unsigned)diag.pending_queue_depth
        );
    }

    onehundo_shutdown(ctx);
    return 0;
}
