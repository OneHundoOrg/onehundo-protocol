# Agent Instructions for `onehundo-protocol`

> **Repository:** [`onehundo-protocol`](https://github.com/OneHundoOrg/onehundo-protocol) (Public, dual `MIT OR Apache-2.0`)  
> **Organization:** `OneHundoOrg` (`onehundo.org`)  
> **Role:** Authoritative protocol definitions, wire/event/pack schemas, strict C99 public SDK (`onehundo.h`), reference implementations, and conformance suite.  
> **Current Phase:** Post-Phase 1.5 baseline verified (`v0.1.0` contracts frozen for Phase 2 entry in `onehundo-client`).  
> **Target Audience:** Autonomous AI agents and human contributors working within the OneHundo ecosystem.

---

## 1. Primary Mandate and Scope

`onehundo-protocol` is the canonical single source of truth for all contracts across the OneHundo achievement infrastructure.

### Strict Separation of Responsibilities

| Responsibility | Repository | Notes |
| :--- | :--- | :--- |
| **Protocol Definitions & Wire Schemas** | [`onehundo-protocol`](https://github.com/OneHundoOrg/onehundo-protocol) *(this repo, Public, `MIT OR Apache-2.0`)* | JSON schemas, OpenAPI specifications, C99 public API, reference transports, conformance validator. |
| **Local Daemon (`onehundod`) & Runtime** | [`onehundo-client`](https://github.com/OneHundoOrg/onehundo-client) *(Public, `MIT OR Apache-2.0`)* | Daemon orchestration, IPC/SHM server, SQLite WAL journal, `rcheevos` evaluation, `Game Health`, notifications. |
| **Platform Services & Web Backend** | [`onehundo-platform`](https://github.com/OneHundoOrg/onehundo-platform) *(Private, Proprietary)* | User accounts, device registration, Open Achievement Protocol (OAP) catalog, pack registry, `/v1/*` cloud sync, web UI. |

**Invariants for Agents:**
- **DO NOT** implement daemon service loops, SQLite storage, UI code, or `rcheevos` logic in this repository.
- **DO NOT** commit proprietary game dumps, ROMs, or copyrighted executable binaries into test fixtures.
- **ALWAYS** preserve backwards compatibility when updating schemas or incrementing protocol versions.
- **Phase 2 Guardrail:** As `onehundo-client` enters Phase 2 (Plane A shared-memory ring + `rcheevos` cross-runtime proof), keep `include/onehundo/onehundo.h` and `schemas/` stable; any required additive contract update must be validated here first before propagating to `onehundo-client` and `onehundo-platform`.

---

## 2. Non-Negotiable Technical Invariants

1. **Strict ISO C99 Public API (`include/onehundo/onehundo.h`):**
   - Must compile with `-std=c99 -pedantic -Wall -Wextra -Werror` and MSVC `/Za /W4 /WX`.
   - Allowed headers: `<stdint.h>`, `<stddef.h>`, `<stdbool.h>` **ONLY**. Zero platform/OS headers (`<windows.h>`, `<unistd.h>`, `<sys/...>`) in `include/`.
   - Forward binary compatibility via explicit `struct_size` and `api_version` guards on all public descriptor structs.
2. **Decoupled Transport Semantics:**
   - The protocol specifies wire serialization, atomics, and buffer ownership handoffs (`FREE -> WRITING -> READY -> READING -> FREE`).
   - OS-specific transport mechanisms (POSIX Unix domain socket + shared memory with `$ONEHUNDO_SOCKET` -> `$XDG_RUNTIME_DIR/onehundo.sock` -> `/tmp/onehundo.sock` discovery, buffered `EVENT_ACK`s, `HELLO` `progression_mode`/`trust_tier`, and Windows named pipes) are isolated to `reference/c/`.
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
- `reference/c/`: Reference implementations of transports (`transport_stub.c`, `transport_posix.c`, `transport_win32.c`) and game integration examples.
- `tools/onehundo-validate/`: Conformance and validation CLI tool (`cargo test`, `cargo clippy`).
- `conformance/fixtures/`: Synthetic, non-proprietary test fixtures exercising valid and invalid edge cases.
- `instructions/`: Deep-dive architectural instructions for client, pack, and server authors.
- `docs/decisions/`: Architecture Decision Records (ADRs).

---

## 4. Verification Commands

- `gcc -x c -std=c99 -Wall -Wextra -Werror -pedantic include/onehundo/onehundo.h -fsyntax-only`
- `g++ -x c++ -Wall -Wextra -Werror -pedantic include/onehundo/onehundo.h -fsyntax-only`
- `cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure` (verify TSan/ASan/UBSan cleanliness on `transport_stub.c` and POSIX IPC tests)
- `cargo test --manifest-path tools/onehundo-validate/Cargo.toml`
- `cargo clippy --manifest-path tools/onehundo-validate/Cargo.toml --all-targets -- -D warnings`
- `python3 conformance/tests/run_conformance.py`
