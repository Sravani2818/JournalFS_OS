# Recovery design

Recovery is the part of a journaling filesystem that has to be right: it runs
when the system is already in an unknown state, it is the only thing standing
between a crash and a broken image, and it gets one shot.  This document
describes what JournalFS does at mount time, why each step is safe, and what it
does when the journal itself is damaged.

---

## 1. Mount sequence

```
open image
  verify superblock (magic, version, geometry, checksum, file length)
  pick the checkpoint:      best of two slots by seq, checksum validated
  open the log half the checkpoint names
  re-derive the journal window (scan, see below)
  mark the image in use (clear the CLEAN flag)
  recover: scan -> group -> validate -> replay -> checkpoint
  report what was done
```

Each step can fail, and each failure has a defined answer:

| what is wrong                       | what mount does                                  |
|-------------------------------------|--------------------------------------------------|
| not a JournalFS image               | refuse: "not a JournalFS image"                   |
| superblock checksum bad             | refuse (mentioning the superblock)                |
| file shorter than `total_blocks`    | refuse: image is truncated                        |
| one checkpoint slot bad             | use the other one                                 |
| both checkpoint slots bad           | refuse: nothing describes the log region          |
| a journal record inside the window is damaged | truncate the window there and report it |
| a *committed* transaction cannot be replayed | refuse: a state that cannot be made consistent |
| the inode map record is damaged     | refuse: nothing describes the inodes              |
| recovery itself replays fine        | write a checkpoint, mount                     |

The principle: **refuse rather than guess.**  A filesystem that mounts and
silently serves an inconsistent tree is worse than one that refuses and says
why, because the user may write to it and destroy the evidence.

## 2. Re-deriving the journal window

Mount never trusts the checkpoint's window blindly; it rebuilds it:

1. **Prefix** — walk `[journal_start, journal_start + journal_used)` and
   re-validate every record header.  The length comes from the checkpoint
   (only a checkpoint frees records), but a damaged block inside the prefix
   truncates the walk instead of making recovery read beyond it.
2. **Tail** — continue past the checkpointed end while the records are
   *newer than the checkpoint's applied txid*.  This is how a transaction that
   was committed but not yet applied — the whole point of the journal — is
   found after a crash.
3. **Stop rule** — the scan stops at the first record whose `txid <=
   applied_txid`.  Such a record is residue of a transaction that a checkpoint
   has already accounted for, so it cannot be live.
4. **Damage detection** — if the walk stopped on something unusable and a
   valid, newer record still exists further along the ring, the window is
   marked `journal_truncated` (with the block index) and the reason is printed
   at mount, by `inspect` (`** DAMAGED **`) and by `check`.

Why step 3 is needed: freeing a journal record is logical.  Advancing the
window hands the blocks back to the append pointer, but the old bytes stay on
the medium until a newer record overwrites them.  Once the 48 block ring has
wrapped, residue looks exactly like live data — correct magic, correct
checksum, increasing sequence numbers.  Without a stop rule a scan would
resurrect already-applied transactions and, worse, move the append pointer on
top of live records.  The transaction id is the one field that separates
"newer than what the checkpoint knows" from "already accounted for".

## 3. Grouping, and what makes a transaction committed

Records are scanned in window order and grouped:

* a group starts at an OP record, collects contiguous records with the same
  `txid`, and closes when a COMMIT record with that txid is found;
* if the group's records are not contiguous, or the txid changes, or the window
  ends first, the group is **incomplete**: nothing from it is replayed, and it
  is counted in "incomplete (ignored)".  An incomplete transaction is exactly
  what `post_sync` simulates: the redo blocks are on the medium, the DONE page
  is not;
* a group whose `txid <= applied_txid` is already applied and is skipped;
* a group that is committed and newer than the checkpoint is **replayed**.

Recovery therefore applies the *logical* rule "committed and not applied", not
the physical rule "present on disk".

## 4. Replay, and why it is idempotent

Replaying a transaction means running its redo operations through the same
apply path a live transaction uses:

1. append the data and directory block records to the log;
2. append the inode records, with the block addresses of step 1 patched in;
3. append a new inode map record.

This is idempotent because every step appends a *new version* and updates a
pointer to it.  Replaying the same transaction twice produces the same logical
state, with some unreachable records in the log that the next compaction
discards.  That matters for the crash points between applying and checkpointing
(`post_apply`, `after_imap`): the transaction is applied, the checkpoint is
not, so the next mount replays it again — and ends up with the same tree.

Because the same code path is used, a replayed transaction cannot produce a
state the live path could not have produced.  That is the property tests
exercise by hashing file contents before and after a crash.

Before anything is committed or replayed, `jfs_txn_validate()` checks the
transaction as a whole: the types make sense, block lengths are in range, block
addresses are non-zero where required, and every inode it carries is *self
consistent* (`blklen[]` fits the block, no references past `nblocks`, sizes add
up to `size`).  A transaction that fails validation is refused; the redo
information is never used to build a state the filesystem itself considers
impossible.

## 5. After replay

Recovery finishes by:

* writing a checkpoint that names the replayed transaction, which frees the
  journal records that are no longer needed;
* leaving the journal window empty (a clean mount reports
  `used=0 of 48`).

The image is then in the same condition as if the crashed session had simply
unmounted, and the next mount has nothing to replay.  The `replayed_total`
counter in the checkpoint accumulates across the image's life, so `inspect`
shows how often recovery has had to work.

## 6. Failure handling inside the engine

Two failure modes deserve their own rules:

* **A commit that cannot be applied.**  If the apply step fails (I/O error,
  out of room), the transaction is durable but only partly applied.  The
  engine sets a `failed` flag and refuses every later operation with
  "filesystem stopped after a failed update: unmount and mount again to replay
  it".  Continuing would fill the journal with transactions that can never be
  reclaimed and would serve a state that is a mixture of the two.  A remount
  replays the transaction and the filesystem is usable again.
* **Running out of room.**  The room check happens *before* the first journal
  write: the engine computes how many journal records and how many log blocks
  the transaction needs.  If either is short it forces a checkpoint (which
  reclaims the journal and compacts the log) and tries once more; a second
  refusal is reported to the caller as a clean error.  The transaction either
  fully happens or does not happen at all.

## 7. What recovery does not do

* It does not repair a damaged journal record by guessing.  A record with a
  broken payload checksum in a committed transaction means the redo
  information is lost; the image is refused, because replaying the rest would
  produce a state that no run of the system ever had.
* It does not erase evidence.  A truncated window is reported on every mount
  until the data is checkpointed past it, and `check` repeats the warning.
* It does not run `fsck`-style heuristics.  Everything is derived from
  structures with checksums: there is no "this looks like an inode" guessing,
  which is also why a random file is reported as "not a JournalFS image"
  rather than being interpreted.

## 8. How this is verified

`tests/test_crash_recovery.sh` walks all 14 crash points and asserts the
expected outcome for each (old value, new value, or "old or new, never a
mix"), then re-mounts to prove idempotency (`results/test_crash_recovery.log`).
`tests/test_corruption.sh` damages one structure at a time with
`tests/corrupt.py` and asserts that each case is either refused with a clear
message or reported as damaged (`results/test_corruption.log`).  Both suites
run against the AddressSanitizer + UndefinedBehaviorSanitizer build as well,
so a recovery path that reads out of bounds fails the build's tests.
