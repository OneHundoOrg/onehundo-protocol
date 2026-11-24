# ADR-001: Provisional Pack Format & Canonical Three-Repository Boundary

- **Status:** Accepted
- **Date:** 2026-11-23
- **Deciders:** OneHundo Protocol Architecture Group

---

## Context

The OneHundo ecosystem establishes a universal achievement network for emulators, static recompilations, source ports, and unmodified games. To avoid monolithic coupling and ensure clean integration across platforms, we must clearly define:
1. The structural boundaries and responsibilities across the three repositories (`onehundo-protocol`, `onehundo-client`, `onehundo-platform`).
2. The format and lifecycle of achievement pack bundles (`.onehundo`).
3. Core synchronization invariants (commit-before-ack and receipt vs. acceptance).

---

## Decision

### 1. Canonical Three-Repository Boundary

The system responsibilities are partitioned as follows:

| Responsibility | Repository | Scope / Notes |
| :--- | :--- | :--- |
| **Protocol Specification & SDK** | `onehundo-protocol` | Public C99 SDK (`onehundo.h`), wire framing definitions, JSON schemas, OpenAPI contracts, reference transports, conformance validator. |
| **Local Daemon & Evaluation** | `onehundo-client` | Background daemon (`onehundod`), append-only SQLite state journal, memory scanning & `rcheevos` engine evaluation, Plane C file importers, desktop overlay and notification manager. |
| **Platform Services & Web** | `onehundo-platform` | User accounts, device registration, canonical game catalogue, pack hosting, cryptographic signing, competitive verification rules, cloud telemetry. |

**Invariants:**
- `onehundo-protocol` contains zero database access code, UI components, or daemon process management.
- `onehundo-client` depends on `onehundo-protocol` contracts and is the sole component writing local progression journals.
- `onehundo-platform` accepts event synchronizations validated against protocol schemas.

### 2. Provisional Pack Format (`.onehundo`)

Achievement definition packs are packaged as standard ZIP archives with the `.onehundo` extension.
Until the release of `v0.1.0` is finalized, the format is designated **provisional**.

Structure:
```text
game-pack.onehundo
├── manifest.json            # Required: validates against manifest.schema.json
├── achievements/            # Definition files (logic, triggers)
│   └── default.json
├── assets/                  # Icons, banners, audio badges
│   └── icons/
└── signatures/              # Optional signatures
    └── pack.sig
```

Security constraints enforced by the validator:
- Path traversal rejection: Paths containing `..` or leading `/` are invalid.
- Symbolic links rejection: No symlinks permitted.
- Compression ratio limit: Maximum uncompressed to compressed ratio is 100:1.

### 3. Commit-Before-Ack Wire Contract

When a game runtime communicates via Plane B IPC (`STAT_SET`, `STAT_INC`, `UNLOCK_REQUEST`), the client daemon MUST NOT emit an `EVENT_ACK` until the event is committed to durable storage (e.g., SQLite WAL flushed). This prevents state divergence in case of crashes.

### 4. Receipt vs. Acceptance in Synchronization

Cloud synchronization endpoints MUST separate:
- **Receipt Status (`receipt_status`):** `RECEIVED` or `DUPLICATE`. Confirms network payload integrity and durable ingestion.
- **Acceptance Status (`acceptance_status`):** `ACCEPTED`, `PENDING_REVIEW`, or `REJECTED`. Reflects anti-cheat policy, trust tier validation, and game verification.

---

## Consequences

- Game runtime integrations remain lightweight and unburdened by database or networking overhead.
- All three repositories develop against stable, schema-validated contracts.
- Security boundaries against untrusted or malicious pack files are enforced at the protocol validator level.
