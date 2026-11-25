# Master Plan — `onehundo-protocol` (Authoritative Specification, SDK & Conformance)

> **Organization:** `OneHundoOrg` (`onehundo.org`)  
> **Repository:** [`onehundo-protocol`](https://github.com/OneHundoOrg/onehundo-protocol) (Public, dual `MIT OR Apache-2.0`)  
> **Role:** Authoritative protocol definitions, wire/event/pack schemas, strict C99 public SDK (`onehundo.h`), reference implementations, and conformance suite.  
> **Status:** Post-Phase 1.5 Baseline Complete — `v0.1.0` contracts frozen and verified for Phase 2 Plane A cross-runtime proof in `onehundo-client`.

---

## 1. Full Architectural Context (Self-Contained Brief)

`onehundo-protocol` powers a **local-first, universal achievement infrastructure** across console emulators, static recompilations, native decompilations/source ports (such as `bbport`), and unmodified PC games.

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
- **Owned by `onehundo-protocol` ([`OneHundoOrg/onehundo-protocol`](https://github.com/OneHundoOrg/onehundo-protocol), Public, `MIT OR Apache-2.0`):** Pack format & schemas, public C99 header (`onehundo.h`), IPC message definitions, sync event schema, evidence/trust definitions, conformance fixtures & validator CLI.
- **Owned by `onehundo-client` ([`OneHundoOrg/onehundo-client`](https://github.com/OneHundoOrg/onehundo-client), Public, `MIT OR Apache-2.0`):** Local daemon (`onehundod`), local achievement state & append-only journal, pack resolution & local evaluation (`rcheevos` + semantic rules), Plane C importers, desktop notifications & `Game Health` / overlays.
- **Owned by `onehundo-platform` ([`OneHundoOrg/onehundo-platform`](https://github.com/OneHundoOrg/onehundo-platform), Private, Proprietary):** User accounts & user IDs, device registration, Open Achievement Protocol (OAP) canonical game catalogue IDs, game releases & regional aliases, achievement catalogue IDs, pack hosting/signing/publication, sync receipt vs. acceptance decisions, and competitive verification policy.

---

## 2. Repository Layout

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

## 4. Phased Delivery Status

### [x] Phase 0 (Epics P0-1 through P0-5) — Completed
- **P0-1 Repository Scaffold & Governance Docs:** Tree, `AGENTS.md`, `README.md`, `CONTRIBUTING.md`, `instructions/*.md`, and `docs/decisions/0001-provisional-pack-format-and-three-repo-boundary.md`.
- **P0-2 Public C99 Header (`include/onehundo/onehundo.h`):** Complete versioned C99 ABI (`struct_size` + `api_version` guards, Planes A/B/C, four orthogonal dimensions, lifecycle signals, non-blocking semantic event APIs, `onehundo_get_diagnostics()`).
- **P0-3 Normative JSON Schemas & OpenAPI Sync Contract:** Draft 2020-12 schemas (`manifest`, `semantic-event`, `region-descriptor`, `snapshot-header`, `observation-record`, `dimensions`) and OpenAPI 3.1 `schemas/sync/openapi.yaml`.
- **P0-4 Reference C SDK & `bbport` Semantic Example:** `transport_stub.c`, `transport_posix.c`, `transport_win32.c`, and `bbport_semantic_example.c`.
- **P0-5 Conformance Validator CLI (`tools/onehundo-validate/`) & Fixtures:** Archive security validation (zip-slip `..`, symlink rejection, 100:1 compression ratio) and synthetic fixtures.

### [x] Phase 1 & Phase 1.5 — Completed
- **POSIX/Win32 IPC Transport Hardening:** `$ONEHUNDO_SOCKET` / `$XDG_RUNTIME_DIR/onehundo.sock` discovery before `/tmp/onehundo.sock` fallback, buffered `EVENT_ACK` handling, and `HELLO` `progression_mode`/`trust_tier` wiring in `transport_posix.c` / `transport_win32.c` + `conformance/tests/test_posix_ipc.c`.
- **TSan & Clippy Cleanliness:** Atomic `slot_state` reads in `reference/c/transport_stub.c` (TSan-clean) and Rust 1.100 Clippy-clean `tools/onehundo-validate/`.
- **Licensing & Attribution:** Updated dual `MIT OR Apache-2.0` copyright and Cargo repository metadata to `OneHundoOrg` (`https://github.com/OneHundoOrg/onehundo-protocol`).

### [ ] Phase 2 Guardrails (Upstream Stability During `onehundo-client` Plane A Proof)
- Maintain `v0.1.0` ABI and schema stability while `onehundo-client` wires Plane A shared-memory ring evaluation (`rcheevos`) and cross-runtime portability harnesses.
- Any protocol change discovered during Phase 2 must land here first, pass CMake/CTest + TSan + `cargo test` + `cargo clippy` + `run_conformance.py`, and be propagated cleanly to `onehundo-client` and `onehundo-platform`.
