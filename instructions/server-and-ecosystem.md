# Server & Ecosystem Specification

This document defines the cloud synchronization contracts, device registration semantics, and pack distribution protocols between `onehundo-client` and `onehundo-platform`.

---

## 1. Cloud Synchronization Architecture

`onehundo-client` communicates with `onehundo-platform` over HTTPS using OpenAPI 3.1 REST contracts.
The local daemon buffers events offline and synchronizes them opportunistically.

```text
+-------------------+                        +---------------------+
|  onehundo-client  |                        |  onehundo-platform  |
+-------------------+                        +---------------------+
          |                                             |
          |  POST /v1/devices/register                  |
          |-------------------------------------------->|
          |  200 OK (device_token, capabilities)        |
          |<--------------------------------------------|
          |                                             |
          |  GET /v1/packs/resolve?rom_hash=...         |
          |-------------------------------------------->|
          |  200 OK (manifest, pack_url, signature)    |
          |<--------------------------------------------|
          |                                             |
          |  POST /v1/sync/events                       |
          |  (batch of committed journal entries)       |
          |-------------------------------------------->|
          |  200 OK (receipt_status, acceptance_status) |
          |<--------------------------------------------|
```

---

## 2. Ingestion Receipt vs. Acceptance

To maintain strong decoupling between network ingestion and verification policy:

### Receipt Status (`receipt_status`):
- `RECEIVED`: The platform ingested the event batch and wrote it durably to disk or ingest queues.
- `DUPLICATE`: The event batch has already been received previously.

### Acceptance Status (`acceptance_status`):
- `ACCEPTED`: The progression event satisfied all verification rules and has updated canonical user profile stats.
- `PENDING_REVIEW`: The event requires asynchronous anti-cheat heuristics, manual review, or competitive quorum.
- `REJECTED`: The event failed signature validation, exceeded speedrun bounds, or violated trust tier policy.

---

## 3. Idempotency & Batching

- All batch sync requests carry a unique `client_batch_id` (UUID).
- Individual events inside the batch maintain their `producer_id + session_id + sequence_number` composite key.
- Re-transmitting an already acknowledged batch returns `receipt_status: DUPLICATE` with the existing acceptance results.
