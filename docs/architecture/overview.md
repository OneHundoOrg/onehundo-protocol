# Architecture Overview

`onehundo-protocol` specifies the contracts and schemas connecting game execution runtimes, local client daemons, and cloud verification platforms.

---

## Ingestion Planes

### Plane A: Memory Arena
- **Target Runtimes:** Console emulators (e.g. SNES, PS1, PS2, RPCS3) and static recompilations.
- **Characteristics:**
  - Described memory regions with base guest address, length, host pointer, endianness, and coherence flags.
  - Zero-copy shared memory slots managed by an atomic four-state lifecycle: `FREE` -> `WRITING` -> `READY` -> `READING` -> `FREE`.
  - Frame lifecycle signals: `NORMAL_FRAME`, `SAVE_STATE_LOAD`, `SOFT_RESET`, `HARD_RESET`, `FRAME_GAP`.

### Plane B: Semantic Events
- **Target Runtimes:** Native source ports, game decompilations (`bbport`), and instrumented PC games.
- **Characteristics:**
  - High-level typed gameplay facts emitted directly from game code: `STAT_SET`, `STAT_INC`, `STATE_OBSERVE`, `UNLOCK_REQUEST`.
  - Strict sequence numbers per session.
  - Non-blocking client SDK ensuring zero frame stutters in game loops.
  - Commit-before-ack wire durability contract.

### Plane C: External Observation
- **Target Runtimes:** Unmodified games with no source access.
- **Characteristics:**
  - Filesystem save watchers, platform API syncs, memory scanners.
  - Ingested strictly as external evidence with provenance tracking (`PLATFORM_CONFIRMED`, `LOCALLY_OBSERVED`, `INFERRED`).

---

## State Dimensions

All progression records in the protocol maintain four orthogonal dimensions:
1. **Progression Mode:** `STANDARD` (regular play), `ENCORE` (modifier/cheat/NG+), `SPECTATOR` (observation).
2. **Trust Tier:** `LOCAL` (unverified dev/casual), `VERIFIED_INTEGRATION` (signed build/official hash), `COMPETITIVE` (hardware/anti-cheat attestations).
3. **Evidence Class:** `DIRECT_INTEGRATION_EVENT`, `RUNTIME_OBSERVED`, `PLATFORM_CONFIRMED`, `LOCALLY_OBSERVED`, `INFERRED`.
4. **Verification State:** `PENDING`, `VERIFIED`, `UNVERIFIED`, `REJECTED`.
