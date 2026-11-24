# Agent Instructions for `onehundo-protocol`

> **Repository:** `onehundo-protocol` (Public)  
> **Role:** Authoritative protocol definitions, wire/event/pack schemas, strict C99 public SDK (`onehundo.h`), reference implementations, and conformance suite.  
> **Target Audience:** Autonomous AI agents and human contributors working within the OneHundo ecosystem.

---

## 1. Primary Mandate and Scope

`onehundo-protocol` is the canonical single source of truth for all contracts across the OneHundo achievement infrastructure.

### Strict Separation of Responsibilities

| Responsibility | Repository | Notes |
| :--- | :--- | :--- |
| **Protocol Definitions & Wire Schemas** | `onehundo-protocol` *(this repo)* | JSON schemas, OpenAPI specifications, C99 public API, reference transports. |
| **Local Daemon (`onehundod`) & Runtime** | `onehundo-client` | Daemon orchestration, IPC listener, SQLite journal, `rcheevos` evaluation, UI overlay, notifications. |
| **Platform Services & Web Backend** | `onehundo-platform` | User accounts, device registration, canonical game IDs, pack registry, cloud synchronization, community leaderboards. |

**Invariants for Agents:**
- **DO NOT** implement daemon service loops, SQLite storage, UI code, or `rcheevos` logic in this repository.
- **DO NOT** commit proprietary game dumps, ROMs, or copyrighted executable binaries into test fixtures.
- **ALWAYS** preserve backwards compatibility when updating schemas or incrementing protocol versions.

---

## 2. Non-Negotiable Technical Invariants

1. **Strict ISO C99 Public API (`include/onehundo/onehundo.h`):**
   - Must compile with `-std=c99 -pedantic -Wall -Wextra -Werror` and MSVC `/Za /W4 /WX`.
   - Allowed headers: `<stdint.h>`, `<stddef.h>`, `<stdbool.h>` **ONLY**. Zero platform/OS headers (`<windows.h>`, `<unistd.h>`, `<sys/...>`) in `include/`.
   - Forward binary compatibility via explicit `struct_size` and `api_version` guards on all public descriptor structs.
2. **Decoupled Transport Semantics:**
   - The protocol specifies wire serialization, atomics, and buffer ownership handoffs (`FREE -> WRITING -> READY -> READING -> FREE`).
   - OS-specific transport mechanisms (POSIX shared memory, Windows named pipes) are isolated to `reference/c/`.
3. **Commit-Before-Ack Contract:**
   - The IPC wire contract dictates that daemons acknowledge critical events (`STAT_SET`, `STAT_INC`, `UNLOCK_REQUEST`) **only after** state is durably flushed to disk.
4. **Receipt vs. Acceptance Distinction:**
   - In sync interactions, network/batch receipt (`receipt_status`: `RECEIVED` | `DUPLICATE`) must remain orthogonal to verification outcome (`acceptance_status`: `ACCEPTED` | `PENDING_REVIEW` | `REJECTED`).
5. **Four Orthogonal State Dimensions:**
   Never collapse or conflate these four dimensions:
   - `progression_mode`: `STANDARD` | `ENCORE` | `SPECTATOR`
   - `trust_tier`: `LOCAL` | `VERIFIED_INTEGRATION` | `COMPETITIVE`
   - `evidence_class`: `DIRECT_INTEGRATION_EVENT` | `RUNTIME_OBSERVED` | `PLATFORM_CONFIRMED` | `LOCALLY_OBSERVED` | `INFERRED`
   - `verification_state`: `PENDING` | `VERIFIED` | `UNVERIFIED` | `REJECTED`

---

## 3. Directory Layout Guidelines

- `include/onehundo/`: Header files distributed to integration consumers. Public surface must remain minimal and strictly C99.
- `schemas/`: Normative JSON Schema Draft 2020-12 and OpenAPI 3.1 definitions.
- `reference/c/`: Reference implementations of transports and game integration examples.
- `tools/onehundo-validate/`: Conformance and validation CLI tool.
- `conformance/fixtures/`: Synthetic, non-proprietary test fixtures exercising valid and invalid edge cases.
- `instructions/`: Deep-dive architectural instructions for client, pack, and server authors.
- `docs/decisions/`: Architecture Decision Records (ADRs).
