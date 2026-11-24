# onehundo-protocol

[![Specification Version](https://img.shields.io/badge/spec-v0.1.0--provisional-blue.svg)](versions/v0.1.0/)
[![License: MIT OR Apache-2.0](https://img.shields.io/badge/License-MIT%20OR%20Apache--2.0-blue.svg)](LICENSE-MIT)
[![Standards: ISO C99](https://img.shields.io/badge/standard-ISO%20C99-brightgreen.svg)](include/onehundo/onehundo.h)

**Authoritative protocol specification, wire/pack schemas, public C99 SDK, and conformance suite for the OneHundo universal achievement ecosystem.**

---

## Overview

`onehundo-protocol` establishes a vendor-neutral, local-first achievement protocol designed to span emulators, static recompilations, native game source ports (e.g. `bbport`), and external observation of unmodified PC titles.

### Core Design Principles

1. **Standardise contracts, not assumptions:** We do not mandate how a game is written or emulated; we standardize how runtime state and semantic events are handed off, validated, and synchronized.
2. **Strict separation of concerns:**
   - **`onehundo-protocol`** *(this repository)*: Authoritative schemas, ABI headers (`onehundo.h`), reference transports, and conformance tests.
   - **`onehundo-client`**: Local daemon (`onehundod`), append-only SQLite journal, `rcheevos` evaluation, and UI overlays.
   - **`onehundo-platform`**: Global registry, identity, pack signing, catalog, and cloud sync.
3. **Commit-before-Ack:** The local client daemon commits critical game events to durable non-volatile storage before issuing an acknowledgement to the game runtime.
4. **Receipt vs. Acceptance:** Transport ingestion success is strictly separated from progression validation decisions.

---

## The Three Ingestion Planes

```text
┌────────────────────────────────────────────────────────────────────────┐
│                        ONEHUNDO INGESTION PLANES                       │
├──────────────────────────┬─────────────────────────┬───────────────────┤
│ Plane A: Memory Arena    │ Plane B: Semantic Event │ Plane C: External │
│ (Emulators / Static Rec) │ (Native Ports / Source) │ (Save Importers)  │
├──────────────────────────┼─────────────────────────┼───────────────────┤
│ • Described guest memory │ • Typed gameplay facts  │ • Platform API    │
│ • Lock-free ring/slots   │ • Monotonic sequence    │ • Disk save scans │
│ • Explicit buffer state  │ • Idempotency key       │ • Evidence-only   │
│ • Frame lifecycle tags   │ • Commit-before-ack     │ • Non-live replay │
└──────────────────────────┴─────────────────────────┴───────────────────┘
```

- **Plane A — Memory Arena:** Designed for console emulators and static recompilations exposing contiguous or banked guest address spaces. Host applications describe memory regions (base, length, endianness, coherence) and exchange snapshot slot buffers following a strict state cycle (`FREE -> WRITING -> READY -> READING -> FREE`).
- **Plane B — Semantic Events:** Designed for native decompilations, source ports (such as `bbport`), and instrumented PC games. Runtimes emit structured facts (`STAT_SET`, `STAT_INC`, `STATE_OBSERVE`, `UNLOCK_REQUEST`) tagged with sequence numbers and idempotency keys.
- **Plane C — External Observation:** Designed for unmodified games. Imports filesystem save states, platform achievements, and external monitors. Stored strictly as **evidence**, never masquerading as live deterministic events.

---

## Four Orthogonal State Dimensions

Every event, state entry, and achievement record models these four orthogonal dimensions independently:

```text
┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│ Progression Mode │  │    Trust Tier    │  │  Evidence Class  │  │Verification State│
├──────────────────┤  ├──────────────────┤  ├──────────────────┤  ├──────────────────┤
│ STANDARD         │  │ LOCAL            │  │ DIRECT_INTEGR... │  │ PENDING          │
│ ENCORE           │  │ VERIFIED_INTEG.. │  │ RUNTIME_OBSERVED │  │ VERIFIED         │
│ SPECTATOR        │  │ COMPETITIVE      │  │ PLATFORM_CONF... │  │ UNVERIFIED       │
│                  │  │                  │  │ LOCALLY_OBSERVED │  │ REJECTED         │
│                  │  │                  │  │ INFERRED         │  │                  │
└──────────────────┘  └──────────────────┘  └──────────────────┘  └──────────────────┘
```

---

## Repository Structure

```text
onehundo-protocol/
├── AGENTS.md                  # Autonomous agent workflow rules and contracts
├── README.md                  # This file
├── CONTRIBUTING.md            # Guidelines for issues, PRs, and schemas
├── docs/                      # Architectural specifications and ADRs
│   ├── architecture/          # In-depth architectural write-ups
│   ├── decisions/             # Architecture Decision Records (ADRs)
│   ├── security/              # Threat models and sandboxing guidelines
│   └── integrations/          # Runtime integration guides (Planes A, B, C)
├── instructions/              # Cross-repo contract instructions
│   ├── client-and-ipc.md      # Daemon wire protocol & IPC specifications
│   ├── packs-and-achievements.md # Pack archive structure & manifest schemas
│   └── server-and-ecosystem.md   # Cloud sync, device registration & APIs
├── include/
│   └── onehundo/
│       └── onehundo.h         # Strict ISO C99 public header
├── schemas/                   # Normative JSON Schema Draft 2020-12 & OpenAPI
│   ├── pack/                  # Manifest schema for .onehundo bundles
│   ├── events/                # Semantic event schemas
│   ├── memory-arena/          # Memory region descriptors & snapshot headers
│   ├── external-observation/  # Observation record schemas
│   └── sync/                  # OpenAPI 3.1 cloud synchronization contract
├── reference/                 # Reference implementations and examples
│   ├── c/                     # Reference transports (Stub, POSIX, Win32)
│   └── examples/              # Integration examples (e.g. bbport)
├── tools/
│   └── onehundo-validate/     # Pack and fixture validation tool
├── conformance/
│   ├── fixtures/              # Synthetic test fixtures (no copyrighted assets)
│   └── tests/                 # End-to-end conformance test suite
└── versions/
    └── v0.1.0/                # Tagged protocol artifacts for v0.1.0
```

---

## Getting Started

### Building Reference SDK & Validator

Requirements:
- C99-compliant compiler (`gcc` or `clang`)
- Rust toolchain (`cargo` >= 1.70)
- CMake >= 3.16

```bash
# Build reference SDK and run test suite
mkdir build && cd build
cmake ..
cmake --build .
ctest --output-on-failure

# Build and run conformance validator
cd ../tools/onehundo-validate
cargo build --release
cargo test
```

### Validating Pack Archives

```bash
cargo run --manifest-path tools/onehundo-validate/Cargo.toml -- \
  pack path/to/sample.onehundo
```

---

## License

Licensed under either of:
- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE))
- MIT license ([LICENSE-MIT](LICENSE-MIT))

at your option.
