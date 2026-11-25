# Client & IPC Specification

This document defines the wire contracts, memory ordering semantics, and handshake lifecycle for the OneHundo client daemon (`onehundod`) and game runtimes.

---

## 1. IPC Architecture

Communication between game runtimes (emulators, source ports) and the local daemon (`onehundod`) operates across two channels:
1. **Control / Event Channel:** Bi-directional messaging for lifecycle, registration, semantic events, and acknowledgements.
   - POSIX: Unix Domain Socket (`$ONEHUNDO_SOCKET` override, then `$XDG_RUNTIME_DIR/onehundo.sock`, falling back to `/tmp/onehundo.sock`)
   - Windows: Named Pipe (`\\.\pipe\onehundo`)
2. **Memory Arena Channel (Plane A only):** Shared memory ring buffer for zero-copy state handoff.
   - POSIX: POSIX shared memory (`shm_open`)
   - Windows: Named Shared Memory (`CreateFileMappingW`)

---

## 2. Framing & Message Structure

All control/event messages are framed with a standard 24-byte, 64-bit aligned binary header, followed by payload (structured binary or UTF-8 JSON):

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Magic (0x31303048 = '100H')                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Version (1)          |         Message Type          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                             Flags                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Payload Length                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                   Sequence Number (64-bit)                    +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Payload Data...                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### Message Types

| ID | Name | Direction | Description |
| :--- | :--- | :--- | :--- |
| `0x0001` | `HELLO` | Runtime -> Daemon | Handshake initiating session with client info, `ingestion_plane`, `progression_mode`, and `trust_tier`. |
| `0x0002` | `HELLO_ACK` | Daemon -> Runtime | Handshake acknowledgement with daemon session ID and capabilities. |
| `0x0003` | `ARENA_REGISTER` | Runtime -> Daemon | Plane A: registers memory regions and shared memory token. |
| `0x0004` | `ARENA_ACK` | Daemon -> Runtime | Confirms shared memory mapping. |
| `0x0010` | `EVENT_PUBLISH` | Runtime -> Daemon | Plane B: semantic event (`STAT_SET`, `STAT_INC`, `UNLOCK_REQUEST`). |
| `0x0011` | `EVENT_ACK` | Daemon -> Runtime | Confirms durable disk commit of the event (Commit-Before-Ack). |
| `0x0020` | `LIFECYCLE_SIGNAL` | Runtime -> Daemon | Frame tick, save state load, reset, or gap signal. |
| `0x00FF` | `GOODBYE` | Runtime <-> Daemon | Clean session termination. |

---

## 3. Plane A Shared Memory Slot Protocol

For zero-copy guest memory snapshotting, the shared memory segment is organized into a ring of snapshot slots. Each slot is guarded by an atomic state variable:

```text
+-------------------------------------------------------+
|                 Slot Ownership Lifecycle              |
+-------------------------------------------------------+
|                                                       |
|        +----------+                  +----------+     |
|        |          |  Producer writes |          |     |
|   +--->|   FREE   |----------------->| WRITING  |     |
|   |    |          |                  |          |     |
|   |    +----------+                  +----------+     |
|   |                                        |          |
|   | Consumer                               | Release  |
|   | completes                              v          |
|   |    +----------+  Acquire         +----------+     |
|   |    |          |<-----------------|          |     |
|   +----| READING  |                  |  READY   |     |
|        |          |                  |          |     |
|        +----------+                  +----------+     |
|                                                       |
+-------------------------------------------------------+
```

### Slot States:
1. `ONEHUNDO_SLOT_FREE (0)`: Slot available for the producer (game runtime / emulator).
2. `ONEHUNDO_SLOT_WRITING (1)`: Producer is copying guest memory into the slot buffer.
3. `ONEHUNDO_SLOT_READY (2)`: Producer has finalized the write (memory fence released).
4. `ONEHUNDO_SLOT_READING (3)`: Consumer (client daemon) is evaluating memory in the slot.

If no slots are in state `FREE` when the emulator advances a frame, the runtime must emit `ONEHUNDO_SIGNAL_FRAME_GAP` with the count of skipped frames. Silent frame drops are prohibited.

---

## 4. Plane B Commit-Before-Ack Protocol

For semantic events:
1. Game runtime emits `EVENT_PUBLISH` containing:
   - `producer_id` (UUID or unique string)
   - `session_id` (UUID)
   - `sequence_number` (strictly monotonic integer starting at 1)
   - Idempotency key: `producer_id + session_id + sequence_number`
2. Daemon receives event, validates schema, and writes it to the local SQLite WAL journal with an fsync flush.
3. Daemon emits `EVENT_ACK` with `sequence_number`.
4. If runtime loses connection before receiving `EVENT_ACK`, it retransmits upon reconnection using the same sequence number.
