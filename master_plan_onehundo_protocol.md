# Master Plan — `onehundo-protocol` (Authoritative Specification, SDK & Conformance)

> **Organization:** OneHundo.org  
> **Repository:** `onehundo-protocol` (Public)  
> **Role:** Authoritative protocol definitions, wire/event/pack schemas, strict C99 public SDK (`onehundo.h`), reference implementations, and conformance suite.  
> **Status:** Phase 0 Kickoff — Ready for autonomous AI agent execution.

---

## 1. Full Architectural Context (Self-Contained Brief)

`onehundo-protocol` powers a **local-first, universal achievement infrastructure** across console emulators, static recompilations, native decompilations/source ports (such as `deadinside28/bloodborne_pc` / `bbport`), and unmodified PC games.

### Core Design Thesis
**Standardise the contracts, not the assumptions.**
- Universal infrastructure standardises integration contracts and tracker state; it **does not** eliminate per-game achievement design, runtime instrumentation, or validation.
- **Strict Separation of Responsibilities:** `onehundo-protocol` defines contracts, schemas, public headers, optional reference examples, and conformance tests. It **MUST NOT** contain `onehundod` production daemon orchestration, SQLite storage, UI code, or `rcheevos` evaluation logic (all of which belong in `onehundo-client`).
- **Provisional Pack Format:** The `.onehundo` pack archive extension is provisional until manifest schema, payload layout, versioning, signing, and compatibility rules are frozen in `versions/v0.1.0`.

### The Three Ingestion Planes
1. **Plane A — Memory Arena:** For runtimes exposing coherent guest-address views (emulators, FPGA bridges, compatible static recompilations). Uses host-described memory regions (never assuming a single flat buffer or host endianness), explicit snapshot buffer ownership (`FREE -> WRITING -> READY -> READING -> FREE`), and strict per-frame sequencing (no silent frame skips; overflow emits an explicit `FRAME_GAP`).
2. **Plane B — Semantic Events:** For native decompilations, source ports (`bbport`), and instrumented PC games. Emits typed gameplay facts (`STAT_SET`, `STAT_INC`, `STATE_OBSERVE`, `UNLOCK_REQUEST`) with monotonic sequence numbers, at-least-once delivery, and idempotency keys (`producer_id + session_id + sequence_number`).
3. **Plane C — External Observation:** Passive readers/importers for unmodified games (platform APIs, local save files, emulator saves, logs). Plane C records are **imported as evidence**, never masqueraded as live gameplay events.

### Four Orthogonal State Dimensions
Every schema, event, and record MUST model these four dimensions independently (never collapsed into one enum):
- **`progression_mode`:** `STANDARD` | `ENCORE` | `SPECTATOR`
- **`trust_tier`:** `LOCAL` | `VERIFIED_INTEGRATION` | `COMPETITIVE`
- **`evidence_class`:** `DIRECT_INTEGRATION_EVENT` | `RUNTIME_OBSERVED` | `PLATFORM_CONFIRMED` | `LOCALLY_OBSERVED` | `INFERRED`
- **`verification_state`:** `PENDING` | `VERIFIED` | `UNVERIFIED` | `REJECTED`

### Canonical Ownership Matrix (Cross-Repo Contract)
- **Owned by `onehundo-protocol`:** Pack format & schemas, public C99 header (`onehundo.h`), IPC message definitions, sync event schema, evidence/trust definitions, conformance fixtures & validator CLI.
- **Owned by `onehundo-client`:** Local daemon (`onehundod`), local achievement state & append-only journal, pack resolution & local evaluation (`rcheevos` + semantic rules), Plane C importers, desktop notifications & optional overlays.
- **Owned by `onehundo-platform`:** User accounts & user IDs, device registration, canonical game catalogue IDs, game releases & regional aliases, achievement catalogue IDs, pack hosting/signing/publication, sync receipt vs. acceptance decisions, and competitive verification policy.

---

## 2. Target Repository Layout

```text
onehundo-protocol/
├── AGENTS.md
├── README.md
├── CONTRIBUTING.md
├── docs/
│   ├── architecture/
│   ├── decisions/
│   ├── security/
│   └── integrations/
├── instructions/
│   ├── client-and-ipc.md
│   ├── packs-and-achievements.md
│   └── server-and-ecosystem.md
├── include/
│   └── onehundo/
│       └── onehundo.h
├── schemas/
│   ├── pack/
│   │   └── manifest.schema.json
│   ├── events/
│   │   └── semantic-event.schema.json
│   ├── memory-arena/
│   │   ├── region-descriptor.schema.json
│   │   └── snapshot-header.schema.json
│   ├── external-observation/
│   │   └── observation-record.schema.json
│   └── sync/
│       └── openapi.yaml
├── reference/
│   ├── c/
│   │   ├── transport_stub.c
│   │   ├── transport_posix.c
│   │   └── transport_win32.c
│   └── examples/
│       └── bbport_semantic_example.c
├── tools/
│   └── onehundo-validate/
├── conformance/
│   ├── fixtures/
│   │   ├── sparse/
│   │   ├── full-snapshot/
│   │   ├── semantic/
│   │   └── observation/
│   └── tests/
└── versions/
    └── v0.1.0/
```

---

## 3. Non-Negotiable Technical Invariants

1. **Strict ISO C99 Public Surface:** `include/onehundo/onehundo.h` MUST compile cleanly under `-std=c99 -Wall -Wextra -Werror -pedantic` with **zero** OS or third-party headers (`<stdint.h>`, `<stddef.h>`, `<stdbool.h>` only).
2. **No Mandatory Transport Implementation:** The protocol specifies required wire behavior, framing, memory ordering, and ownership transitions (`FREE -> WRITING -> READY -> READING -> FREE`), while allowing transport implementations to vary. Reference C transports live in `reference/c/`, not `include/`.
3. **Commit-Before-Ack Wire Contract:** The IPC specification MUST define `EVENT_ACK` such that daemons acknowledge essential events (`STAT_SET`, `STAT_INC`, `UNLOCK_REQUEST`) **only after** durable storage commit.
4. **Sync Receipt vs. Acceptance:** `schemas/sync/openapi.yaml` MUST distinguish HTTP/batch receipt (`receipt_status: RECEIVED | DUPLICATE`) from progression acceptance (`acceptance_status: ACCEPTED | PENDING_REVIEW | REJECTED`).
5. **Zero Copyrighted Assets:** Never commit ROMs, `eboot.bin`, decrypted archives, or proprietary game assets in `conformance/fixtures/`.

---

## 4. Phased Work Breakdown for AI Agents

### Epic P0-1: Repository Scaffold & Governance Docs
- **Deliverables:** Initialize directory tree above; place `AGENTS.md`, `README.md`, `CONTRIBUTING.md`, `instructions/*.md`, and ADR-001 (`.onehundo` provisional format & 3-repo boundary).
- **Acceptance Criteria:** Tree matches specification; all markdown links resolve; ADR documents canonical ownership table.

### Epic P0-2: Public C99 Header (`include/onehundo/onehundo.h`)
- **Deliverables:** Complete versioned C99 ABI with `struct_size` + `api_version` guards, enums for Planes A/B/C, orthogonal state dimensions, lifecycle signals (`NORMAL_FRAME`, `SAVE_STATE_LOAD`, `SOFT_RESET`, `HARD_RESET`, `FRAME_GAP`), memory region descriptors, snapshot slot headers, non-blocking semantic event functions (`onehundo_stat_set`, `onehundo_stat_inc`, `onehundo_state_observe`, `onehundo_unlock_request`), and `onehundo_get_diagnostics()`.
- **Acceptance Criteria:** Compiles with zero warnings under GCC/Clang `-std=c99 -pedantic -Werror` and MSVC `/Za /W4 /WX` on 32-bit and 64-bit targets.

### Epic P0-3: Normative JSON Schemas & OpenAPI Sync Contract
- **Deliverables:**
  - `schemas/pack/manifest.schema.json`: Multi-identity matchers (`rom_hash`, `recomp_game_id`, `build_id`, `platform_id`, `sidecar_uuid`, `asset_archive_hash`), canonical achievement IDs, multiple runtime profiles (`memory`, `semantic`, `observation`), resource/WASM sandbox metadata, and signature block.
  - `schemas/events/semantic-event.schema.json` & `schemas/external-observation/observation-record.schema.json`.
  - `schemas/sync/openapi.yaml`: `/v1/devices/register`, `/v1/packs/resolve`, `/v1/sync/events` (separating durable receipt from acceptance outcome), and `/v1/progression/{canonical_game_id}`.
- **Acceptance Criteria:** All schemas pass JSON Schema Draft 2020-12 meta-validation; OpenAPI passes `spectral` / OpenAPI 3.1 lint with zero errors.

### Epic P0-4: Reference C SDK & `bbport` Semantic Example
- **Deliverables:**
  - `reference/c/transport_stub.c` (in-memory mock sink for headless unit tests).
  - `reference/c/transport_posix.c` (Unix domain socket + POSIX `shm_open` with release/acquire slot transitions) & `reference/c/transport_win32.c` (Named Pipe + `CreateFileMappingW`).
  - `reference/examples/bbport_semantic_example.c` demonstrating non-blocking Plane B instrumentation for a native port (`sony.bloodborne`).
- **Acceptance Criteria:** Reference build compiles and passes ASan/UBSan + ThreadSanitizer tests for concurrent slot handoff (`FREE -> WRITING -> READY -> READING -> FREE`).

### Epic P0-5: Conformance Validator CLI (`tools/onehundo-validate/`) & Fixtures
- **Deliverables:**
  - CLI tool validating pack archives (schema validation, zip-slip `..` path traversal rejection, symlink rejection, zip-bomb ratio limits) and trace fixtures.
  - Synthetic conformance fixtures in `conformance/fixtures/`:
    - `sparse/shared_game_emulated.oh-sparse.jsonl`
    - `full-snapshot/shared_game_recomp.oh-snap.zst`
    - `semantic/bbport_sample.oh-events.jsonl`
    - `observation/steam_local_save.oh-obs.json`
- **Acceptance Criteria:** Validator passes all valid fixtures and rejects all negative security/malformed test cases in CI; tag release `v0.1.0` for consumption by `onehundo-client` and `onehundo-platform`.