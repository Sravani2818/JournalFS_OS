# The JournalFS write-ahead log protocol

This document describes the journal: its record format, the order in which
bytes reach the image, what each injected crash point means, and what a client
is allowed to assume after `sync`.

The on-disk structures are in `disk_format.md`; the recovery side is in
`recovery_design.md`.

---

## 1. The problem being solved

Changing a file is never one single change on disk.  Writing 10 KiB into a file
updates the data blocks, then the inode that points at them, then the inode map
that points at the inode, and maybe a directory block if the name is new.  If
the machine loses power between two of those steps, the image contains a mix of
old and new: an inode that claims blocks that were never written, or a map
entry that points at an inode from a transaction that never happened.  That
state is not "slightly old data" — it is a state that *never existed*, which is
exactly what a filesystem must never expose.

JournalFS solves this the way a database does: **the change is described in a
write-ahead log, made durable, and only then applied to its final location.**

Hyper-simple mental model:

1. Write down *what you are going to do* in a little notebook and make sure
   the notebook page is really on disk.
2. Write "DONE" at the bottom of that page and make sure *that* is on disk.
3. Now do the work.
4. Once the work is known to be on disk, cross the page out; it is no longer
   needed.

If the power fails at any point, the recovery step reads the notebook: pages
with "DONE" are re-done, pages without it are ignored.  Doing it twice is
harmless because the operations are idempotent (they write whole blocks to
fixed addresses).

## 2. Roles of the three areas

| area        | role                                                          |
|-------------|---------------------------------------------------------------|
| journal     | the notebook: short lived, circular, holds what is not applied |
| log region  | the filesystem itself: append-only, log-structured, permanent  |
| checkpoint  | the bookmark: "everything up to transaction N is in the log"    |

The journal and the log are deliberately *different* things:

* the **journal** holds redo descriptions (a copy of the block images a
  transaction is about to write) for a short window, and is reclaimed as soon
  as a checkpoint says those transactions are applied;
* the **log** holds the actual current bytes of every object, with old versions
  left behind until a compaction copies only what is still reachable.

A transaction therefore touches the journal first (twice), the log second, and
the checkpoint last.  The journal never stores "old" data and never needs to:
the log keeps the previous version alive until the checkpoint moves forward.

## 3. Transaction protocol

```
begin    : build redo operations in memory         (no disk writes)
commit   : 1. write OP record(s)     -> pwrite
           2. fsync                 -> barrier #1
           3. write COMMIT record   -> pwrite
           4. fsync                 -> barrier #2   <-- durability point
apply    : 5. append data/dir records to the log
           6. append the inode records
           7. append a new inode map record
end      : 8. write the next checkpoint slot, fsync
           9. reclaim the journal records it makes unnecessary
          10. compact the log if the active half is running out of room
```

Notes on each step:

* **Step 2** guarantees the redo descriptions are on the medium before the
  commit record can be.  Without it, a crash could leave a "DONE" page whose
  content was never written — the one failure a journal must not have.
* **Step 4** is the durability point a client cares about.  After it returns,
  the transaction survives any crash, even one that happens before step 5.
* **Steps 5–7** write the *new* versions into the log.  Data and directory
  blocks are appended before the inodes that reference them, so a crash during
  the apply can never leave an inode pointing at a block that was not written;
  at worst the new block is unreachable garbage.
* **Step 8** publishes the fact that everything up to transaction *N* is in the
  log.  It goes to the *other* checkpoint slot and is fsynced before the
  in-memory state adopts it, so a torn checkpoint falls back to the previous
  one.
* **Step 9** may only run after step 8 is durable.

### Two barriers, not one

A single barrier is not enough.  Writing the OP records, the COMMIT record and
then calling `fsync()` once leaves the order of those writes to the device's
write cache: the COMMIT record can hit the medium before the OP records it is
supposed to certify.  Recovery would then find a committed transaction whose
redo description is missing and have to refuse to mount.  The protocol pays for
the second barrier instead.

### Grouping

One transaction may produce several OP records (a 10 KiB rewrite does not fit
in one 96 KiB limit-safe chunk on a big transaction).  They are written in
sequence with increasing `seq`, and recovery groups *contiguous records with
the same txid* ending in a COMMIT record.  A COMMIT record therefore certifies
all of them.

## 4. Journal record types

| type | name       | payload                                        |
|------|------------|------------------------------------------------|
| 1    | `OP`       | redo operations (header + payload per op)       |
| 2    | `COMMIT`   | none — the durability marker                    |
| 3    | `ABORT`    | none — reserved for aborted transactions        |
| 4    | `CHECKPOINT` | none — reserved, informational                |

Byte-level layout of the record header and of the redo operation header is
tabled in `disk_format.md` §4.  Fields worth explaining:

* `txid` — the transaction the record belongs to.  Recovery groups by it, and
  it is what tells a re-derived window where residue begins (see below).
* `lsn` — a strictly increasing record sequence assigned by the engine.  It is
  used to reject a scan result that goes backwards in time.
* `nblocks` — how many whole blocks this record occupies.  It is validated
  against the region size before use, so a corrupt value cannot make the scan
  walk outside the journal.
* `payload_crc` / `hdr_crc` — the two checksums.  A damaged header means the
  record (and everything after it in the window) is unusable; a damaged payload
  means a committed transaction cannot be replayed, which is reported as a
  refusal to mount rather than silently dropped.

## 5. The journal window

The window is `[journal_start, journal_start + journal_used)` inside the
48 block ring.  Records are written at the end of the window; the front is
reclaimed when a checkpoint makes it unnecessary.

Two rules make this safe:

* **Freeing is logical.**  Advancing `journal_start` does not erase anything;
  the old bytes stay on the medium until a new record is written over them.
* **A scan must know where the live window ends.**  Because of the previous
  rule, a ring that has wrapped looks like a continuous stream of perfectly
  valid records, including ones that are already applied and free.  Mount
  re-derives the window as:
  1. the *prefix* the checkpoint declares, re-validated record by record, so a
     damaged block inside it shrinks the window instead of being trusted; then
  2. the records written *after* that checkpoint — a committed transaction that
     no checkpoint mentions yet, which is precisely the case the journal exists
     for.  The scan stops at the first record whose `txid` is not newer than the
     checkpoint's `applied_txid`, since a record is only ever freed after its
     transaction's txid is <= the applied txid.  That is the test that tells
     residue from live data.

If step 1 or 2 stops on a block that is not a valid record *and* a valid,
newer record still exists later in the ring, the filesystem reports
`journal_truncated` with the block index: transactions after the damage were
lost, and saying so is the only honest answer.

## 6. Checkpoint and reclamation

A checkpoint is written when the configured interval of transactions has been
applied (`set checkpoint-interval N`, default 1) and unconditionally by `sync`
and `unmount`.  It records the new `journal_start`/`journal_used`, which frees
the leading records.  Sequence for each checkpoint:

1. write the checkpoint into the slot that is not current (no fsync yet);
2. this is crash point `checkpoint_post` — the new slot may or may not be
   durable, the old slot is still untouched;
3. `fsync`;
4. adopt the new window and continue.

Batching (`checkpoint-interval N > 1`) trades recovery work for throughput:
fewer checkpoints and fewer fsyncs, but more transactions for recovery to redo
after a crash.  The deferral is bounded twice: at most `N-1` transactions, and
never when the journal has fewer than 8 free blocks.  `sync`, `unmount` and the
"journal is full" retry path force a checkpoint regardless of the setting.

## 7. Log compaction

The log has two halves.  When the active half is more than three quarters
full, the filesystem walks the reachable objects (the inode map, every inode,
every data and directory block it references), appends them into the *other*
half starting at block offset 1, then writes a checkpoint that switches
`log_half` and the addresses.  The previous half becomes scratch space.

Compaction changes addresses but never content, which is what makes it
idempotent: a crash in the middle of it leaves the old half and the old
checkpoint intact, so recovery replays whatever was pending and the next
compaction simply starts over.

## 8. Crash points

`--crash POINT` (or `JFS_CRASH`) makes the process call `_exit(99)` at a
precise stage.  This is deterministic and happens inside the engine, not by
killing the process from outside, so tests can name the exact instant.  Image
writes never go through stdio, so `_exit` loses nothing that mattered; the
console buffers are flushed first so the test can see what the process did.

| point               | when                                    | what must be true after a crash |
|---------------------|-----------------------------------------|---------------------------------|
| `pre_write`         | before the OP records are written       | old value                          |
| `post_write`        | OP records written, no fsync            | old value (they may vanish)        |
| `post_sync`         | OP records synced, no COMMIT            | old value; the transaction is incomplete and must be ignored |
| `post_commit`       | COMMIT written, no fsync                | old *or* new, then complete after recovery |
| `post_commit_sync`  | COMMIT synced, not applied              | recovery replays it: new value     |
| `post_apply`        | log updated, checkpoint not written     | recovery replays it: new value     |
| `after_imap`        | block records written, inodes not yet   | recovery replays it: new value     |
| `checkpoint_mid`    | checkpoint update starting              | old checkpoint still valid         |
| `checkpoint_post`   | new checkpoint slot written, not synced | old *or* new checkpoint, never a mix |
| `compact_mid`       | compaction copied some records          | old half + old checkpoint still valid |
| `compact_post`      | new half filled, checkpoint not durable | old half still authoritative       |
| `recover_mid`       | during recovery, before replay          | image still mounts and replays     |
| `pre_format`        | before `format` creates the image       | previous image byte-for-byte unchanged |

`--crash-after N` arms the injection only from the Nth commit onwards, which is
how tests get a *specific* transaction committed before the crash.

## 9. Disabling the journal (baseline mode)

`--no-journal` runs the engine with the journal writes and the two fsync
barriers skipped.  The log, the checkpoints and every other mechanism are
unchanged, so this measures the *cost of the journal* and nothing else.  It is
a benchmark and comparison mode, and it is not crash safe: a crash during the
apply can leave a half-written log record, and there is no redo information to
repair it.  The CLI prints a warning every time it is enabled.

## 10. What is guaranteed, and what is not

Guaranteed after a successful `sync` (or `unmount`):

* every operation reported as successful is present after a crash and recovery;
* the recovered image is one of the states the sequence of operations really
  passed through — never a mixture of two;
* recovery never invents data: it replays exactly the committed transactions
  and nothing else.

Not guaranteed:

* durability of an operation that was reported as successful *before* the
  next `sync` in baseline mode (no journal, no barrier);
* anything about the physical disk: the image is a file on the host
  filesystem, and a crash of the *host* is emulated by `_exit`, not by cutting
  power to a real device.  This is a simulator; the phase ordering is real, the
  storage stack underneath it is not modelled.
