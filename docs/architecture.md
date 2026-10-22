# JournalFS architecture

JournalFS is a user-space filesystem *simulator*: it implements the parts of a
journaling, log-structured filesystem that make crash consistency interesting,
and it stores everything inside one disk image file that it manages itself.
This document describes the layers, who owns which decision, and why the
boundaries are where they are.

---

## 1. The six layers

```
        +-----------------------------------------------------------+
   L1   |  CLI                        src/cli.c, src/main.c          |
        |  commands, options, crash injection, output               |
        +-----------------------------------------------------------+
                    |  jfs_create / jfs_write / jfs_read / ...
        +-----------------------------------------------------------+
   L2   |  Filesystem operations      src/filesystem.c              |
        |  paths, directories, file contents, permissions, df       |
        +-----------------------------------------------------------+
                    |  jfs_txn_begin / add_* / commit / apply / end
        +-----------------------------------------------------------+
   L3   |  Transaction manager        src/transaction.c, journal.c  |
        |  write ahead logging, commit protocol, fsync barriers     |
        +-----------------------------------------------------------+
                    |  jfs_journal_append, groups of records
        +-----------------------------------------------------------+
   L4   |  Recovery manager           src/recovery.c, crash.c       |
        |  mount time scan/group/replay, consistency check          |
        +-----------------------------------------------------------+
                    |  jfs_log_append / jfs_log_compact / checkpoints
        +-----------------------------------------------------------+
   L5   |  Storage engine             src/storage.c, checkpoint.c   |
        |  log-structured log, two halves, compaction, imap records |
        +-----------------------------------------------------------+
                    |  jfs_pread_full / jfs_pwrite_full / fsync
        +-----------------------------------------------------------+
   L6   |  Persistent disk image      src/disk_image.c              |
        |  image creation, superblock, geometry, raw block I/O      |
        +-----------------------------------------------------------+
```

Supporting modules that no single layer owns: `util.c` (status codes, size
parsing, `jfs_buf`), `checksum.c` (CRC-32C), `inode.c` (inode validation and
the inode map), `directory.c` (directory blocks and views), `permissions.c`
(the simulated single user model).

The rule that keeps the layers honest: **only L6 touches the image file.**  No
operation writes "directly to disk"; it describes a change as a transaction
and the transaction goes through L3/L5.  That is what makes the crash points
meaningful — there is exactly one path where bytes reach the medium.

## 2. What each layer is responsible for

**L1 CLI** (`cli.c`) parses options and commands, maintains the simulated
working directory, formats output, owns the exit codes (0 success, 1 operation
error, 2 usage error, 99 injected crash) and arms crash injection.  It never
touches the image directly.

**L2 Filesystem operations** (`filesystem.c`) resolves paths, builds redo
operations, and decides what a user-level operation means: which blocks a file
has, which inode entries change, what a directory looks like.  Everything it
does becomes a transaction.

**L3 Transaction manager** (`transaction.c`, `journal.c`) is the heart of the
crash-consistency story: it plans the journal records a transaction needs,
checks room *before* writing anything, runs the two-phase commit with its
fsync barriers, applies the redo operations, and advances the journal window.

**L4 Recovery manager** (`recovery.c`, `crash.c`) runs at mount time: it
re-derives the journal window, groups records into transactions, replays the
committed ones, and verifies that the result is self consistent.

**L5 Storage engine** (`storage.c`, `checkpoint.c`) owns the log-structured
log: appending records, patching addresses, compaction into the other half, and
the double-buffered checkpoint that describes which half and which inode map
are current.

**L6 Disk image** (`disk_image.c`) creates and opens the image, validates the
superblock, computes the geometry, and provides the byte level primitives
(`jfs_pread_full`, `jfs_pwrite_full`, `fsync`) that everyone else uses.  It is
the only layer that knows the file exists.

## 3. One write, end to end

`write /f HELLO` in an image that already holds `/f` with 4 bytes of content:

```
CLI          parse "write", read the argument, call jfs_write()
L2           look up /f (jfs_resolve -> imap -> inode)
             read the old content (read_content -> log addresses -> blocks)
             build: 1 DATA redo op, 1 INODE redo op (provided_mask = bit 0)
L3  commit   plan the OP record (1 block) + COMMIT (1 block); room check
             pwrite OP record        ] crash points: pre_write, post_write
             fsync                   ] barrier #1        post_sync
             pwrite COMMIT record    ]                   post_commit
             fsync                   ] barrier #2, durable
L5  apply    append DATA block  -> address A           (after_imap)
             patch inode.addrs[0] = A, append INODE record
             append a new IMAP record
L3  end      write the other checkpoint slot (journal window advanced)
             fsync, adopt the new checkpoint        checkpoint_mid/post
L5           compact if the active half is > 3/4 full  compact_mid/post
L1           print "wrote /f"
```

Everything a crash can interrupt is named, and each name has a documented
expected outcome in `journal_protocol.md` §8.  The test suites assert them.

## 4. Key design decisions

**Why a journal *and* a log?**  A log-structured filesystem is already
copy-on-write, but that alone is not enough: an operation like "rename" changes
a directory block and an inode, and a crash between the two appends leaves a
tree that never existed.  The journal makes the *set* of changes atomic.  The
log then keeps the current version of everything, so the journal only has to
remember a short window.

**Why redo logging rather than undo?**  With undo, recovery has to *find* the
old versions of things, which means keeping them reachable or saved somewhere —
more bookkeeping than redoing a copy of the new bytes.  Redo also makes replay
idempotent, which is what allows the crash points between apply and checkpoint.

**Why two checkpoints?**  Writing a checkpoint is the moment the filesystem
declares "everything up to here is applied".  If that block is torn, the image
must not become unusable, so the previous one is kept untouched until the new
one is durable, and the valid slot with the highest `seq` wins.

**Why block offset 0 of each log half is unusable?**  A log address has to
distinguish "no block" from "block zero".  Reserving offset 0 in each half (and
starting the log at offset 1) removes the ambiguity at the cost of two blocks
per image — a bug class worth more than two blocks.

**Why `provided_mask` is not persisted.**  A transaction carries the data
blocks it writes and the inode that refers to them; the apply step needs to
know which of the inode's 16 addresses belong to *this* transaction so it can
patch them.  Storing that mask in the on-disk inode confused later updates (a
`chmod` looked like "this inode declares no blocks"), so it lives only in the
transaction.

**Why the inode map is a rewrite-everything record.**  It is 4048 bytes and
fits one log record exactly.  Appending a fresh copy on any change is simple,
needs no partial-update protocol, and gives the map the same atomicity as any
other object.  The cost is a 4 KiB write per transaction that touches an inode,
which the benchmark shows is small compared with the fsyncs.

**Why a crash flag and not a signal?**  `--crash POINT` calls `_exit(99)`
inside the engine at a named point.  It is deterministic, needs no external
tooling, works under a debugger and under the sanitizers, and cannot "helpfully"
flush something the crash was supposed to lose.  `_exit` skips stdio buffers,
which the injection flushes first (console only — image writes use
`pwrite`/`pread`/`fsync` and never pass through stdio).

## 5. Data structures in memory

`struct jfs_fs` (in `journalfs.h`) is the whole mounted state: the file
descriptor, the superblock, the current checkpoint, the in-memory inode map,
the log head/sequence, the journal window bookkeeping, the crash-injection
state, and counters.  Operations never cache file contents between calls; they
read through the log every time, which keeps the on-disk state the single
source of truth (and keeps the code honest about it).

`jfs_buf` is the only buffering: a growable byte buffer used for file
contents, payloads and formatted output.

## 6. Where to look next

| topic                            | document                       |
|----------------------------------|--------------------------------|
| byte layout, offsets, checksums  | `disk_format.md`               |
| WAL protocol, crash points       | `journal_protocol.md`          |
| mount time scan and replay       | `recovery_design.md`           |
| diagrams (block layout, timeline)| `diagrams.md`                  |
| how it is tested                 | `testing_plan.md`              |
| measured performance             | `report.md`, `../results/`     |
