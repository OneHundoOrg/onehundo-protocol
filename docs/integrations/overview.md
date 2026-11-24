# Runtime Integration Overview

This guide explains how developers of emulators, recompilations, source ports, and tools integrate with `onehundo-protocol`.

---

## Choosing Your Ingestion Plane

| Scenario | Plane | Primary Mechanisms |
| :--- | :--- | :--- |
| Console emulators (e.g. Ares, DuckStation, PCSX2, RPCS3) | **Plane A** | `onehundo_arena_*` APIs, shared memory ring buffer, frame lifecycle signals. |
| Static recompilations (e.g. N64/PS1/PS2 static recomp) | **Plane A or B** | Plane A if coherent memory layout exposed; Plane B if events instrumented directly. |
| Native source ports & decompilations (e.g. `bbport`) | **Plane B** | `onehundo_stat_set()`, `onehundo_stat_inc()`, `onehundo_unlock_request()`. |
| Unmodified PC games & external save scanners | **Plane C** | File save scanners emitting observation records to `onehundod`. |

---

## Plane B Quickstart (Native Source Ports / Decompilations)

Include `<onehundo/onehundo.h>` in your game project. Initialize the client at game startup:

```c
#include <onehundo/onehundo.h>

static onehundo_context_t *g_oh_ctx = NULL;

void game_init(void) {
    onehundo_client_config_t config;
    onehundo_client_config_init(&config);
    config.canonical_game_id = "sony.bloodborne";
    config.producer_id = "bbport_pc_x64";
    config.ingestion_plane = ONEHUNDO_PLANE_SEMANTIC_EVENT;

    onehundo_init(&g_oh_ctx, &config, sizeof(config));
}

void on_boss_killed(const char *boss_id) {
    if (g_oh_ctx) {
        onehundo_stat_inc(g_oh_ctx, "bosses_defeated", 1);
        onehundo_state_observe(g_oh_ctx, "last_defeated_boss", boss_id);
    }
}

void game_shutdown(void) {
    if (g_oh_ctx) {
        onehundo_shutdown(g_oh_ctx);
        g_oh_ctx = NULL;
    }
}
```

Calls to `onehundo_*` are non-blocking and safe to call on rendering or gameplay threads.
