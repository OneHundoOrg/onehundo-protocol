# Contributing to onehundo-protocol

Thank you for contributing to `onehundo-protocol`! As the authoritative specification repository for the OneHundo ecosystem, changes here impact downstream clients, emulators, source ports, and cloud services.

---

## 1. Governance and Invariants

All contributions must strictly respect our architectural invariants:
- **Strict ISO C99 Public Header:** `include/onehundo/onehundo.h` must not include OS-specific headers. Only `<stdint.h>`, `<stddef.h>`, and `<stdbool.h>` are permitted.
- **Cross-Repo Boundary:** Do not implement SQLite persistence, UI components, daemon services, or `rcheevos` rule evaluation logic here.
- **Zero Copyrighted Material:** No game ROMs, proprietary binaries, copyrighted artwork, or decrypted game archives may be committed to this repository. All fixtures must be synthetic or procedurally generated.

---

## 2. Development Standards

### C Code (`include/` and `reference/`)
- Public header: Must adhere to ISO C99.
- Clean compilation under `-std=c99 -Wall -Wextra -Werror -pedantic`.
- Explicit struct padding and alignment where structs are intended for binary wire serialization or shared memory.
- Clear state transitions using atomic handoffs (`FREE -> WRITING -> READY -> READING -> FREE`).

### JSON Schemas & OpenAPI
- Schemas must validate against **JSON Schema Draft 2020-12**.
- OpenAPI specs must adhere to **OpenAPI 3.1.0**.
- All schema properties must include descriptions and explicit typing.

### Conformance Validator (`tools/onehundo-validate/`)
- Written in Rust.
- Must reject path traversal (`..` zip slip), symbolic links inside archives, and compression bombs (decompression ratio limit: 100:1).
- Must run cleanly with `cargo test` and `cargo clippy`.

---

## 3. Pull Request Workflow

1. Fork the repository and create your feature branch.
2. Run test suites locally:
   ```bash
   # C tests
   cmake -B build -S .
   cmake --build build
   ctest --test-dir build --output-on-failure

   # Rust validator
   cargo test --manifest-path tools/onehundo-validate/Cargo.toml
   ```
3. Ensure all documentation and schema changes are updated synchronously.
4. If modifying protocol contracts, include an Architecture Decision Record (ADR) under `docs/decisions/`.
