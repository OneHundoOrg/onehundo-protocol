# OneHundo Protocol `v0.1.0` (Provisional Freeze)

This directory marks the `v0.1.0` protocol baseline for consumption by `onehundo-client` and `onehundo-platform`.

## Frozen Contracts

- **Public C99 ABI:** [`include/onehundo/onehundo.h`](../../include/onehundo/onehundo.h) (`ONEHUNDO_API_VERSION = 0.1.0`)
- **IPC Wire & Slot Protocol:** [`instructions/client-and-ipc.md`](../../instructions/client-and-ipc.md)
- **Pack Manifest Schema:** [`schemas/pack/manifest.schema.json`](../../schemas/pack/manifest.schema.json)
- **Semantic Event Schema:** [`schemas/events/semantic-event.schema.json`](../../schemas/events/semantic-event.schema.json)
- **Memory Arena Schemas:** [`schemas/memory-arena/region-descriptor.schema.json`](../../schemas/memory-arena/region-descriptor.schema.json), [`schemas/memory-arena/snapshot-header.schema.json`](../../schemas/memory-arena/snapshot-header.schema.json)
- **External Observation Schema:** [`schemas/external-observation/observation-record.schema.json`](../../schemas/external-observation/observation-record.schema.json)
- **Cloud Sync OpenAPI Contract:** [`schemas/sync/openapi.yaml`](../../schemas/sync/openapi.yaml)
- **Conformance CLI:** [`tools/onehundo-validate/`](../../tools/onehundo-validate/)

## Provisional Status Note

The `.onehundo` pack archive format carries provisional status under `v0.1.0` per [ADR-001](../../docs/decisions/0001-provisional-pack-format-and-three-repo-boundary.md).
