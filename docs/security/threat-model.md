# Security & Threat Model

This document outlines the security assumptions, trust boundaries, and mitigation strategies across the `onehundo-protocol` surface.

---

## 1. Threat Boundaries

```text
┌────────────────────────────────────────────────────────┐
│                      UNTRUSTED ZONE                    │
│   • Untrusted Pack Files (.onehundo)                   │
│   • Malicious Game Code or Modified Runtimes           │
│   • External Save Files (Plane C)                      │
└────────────────────────────────────────────────────────┘
                           │
       Validator Checks &  │  IPC Protocol Boundaries
       Sandbox Enforcement │
                           ▼
┌────────────────────────────────────────────────────────┐
│                   LOCAL TRUSTED ZONE                   │
│   • onehundo-client Daemon (onehundod)                 │
│   • Local SQLite State Journal                         │
└────────────────────────────────────────────────────────┘
                           │
       TLS 1.3 / OpenAPI   │  Ed25519 Signatures
       Device Auth Tokens  │
                           ▼
┌────────────────────────────────────────────────────────┐
│                   CLOUD PLATFORM ZONE                  │
│   • onehundo-platform Verification Engines             │
│   • Canonical Catalogue and Global Rankings            │
└────────────────────────────────────────────────────────┘
```

---

## 2. Pack Archive Security Constraints

Pack archives (`.onehundo`) are distributed over public networks and may be created by community members. The conformance validator enforces:

1. **Zip-Slip Directory Traversal:**
   - Any entry path containing parent directory markers (`..`), relative prefixes (`./`), or absolute prefixes (`/` or `C:\`) is strictly rejected.
2. **Symbolic Links & Hard Links:**
   - Archives containing Unix symbolic links or directory symlinks are rejected.
3. **Decompression Bombs (Zip Bombs):**
   - The ratio of uncompressed size to compressed size is strictly bounded to a maximum of `100:1`.
   - Single-entry maximum uncompressed size is capped at 64 MiB; total archive uncompressed size is capped at 256 MiB.
4. **File Whitelisting:**
   - Only approved extensions (`.json`, `.png`, `.jpg`, `.webp`, `.wasm`, `.sig`, `.md`) are permitted inside packs. Executables (`.exe`, `.so`, `.dll`, `.bat`, `.sh`) are rejected.

---

## 3. Shared Memory Isolation (Plane A)

1. Memory descriptors explicitly declare the maximum memory buffer length.
2. The daemon maps memory strictly read-only wherever supported by host OS memory protections.
3. Lock-free slot handoffs use bounded ring sizes, preventing unbounded buffer growth.
4. Sequence overflows or state corruption trigger automatic slot invalidation and a `FRAME_GAP` log.
