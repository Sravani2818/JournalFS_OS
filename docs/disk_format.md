# JournalFS on-disk format

Everything a JournalFS image contains is described in this document.  The
authoritative definitions live in `include/disk_format.h`; the compile-time
checks in that header fail the build if a structure changes size, so this
document and the code cannot drift apart silently.

Throughout: **one block is 4096 bytes**, all integers are little-endian, all
structures are packed (no padding inside them), and every structure that can
be damaged carries a **CRC-32C** checksum (polynomial 0x1EDC6F41, reflected
form 0x82F63B78 — the same one the `jfs_crc32c()` function in
`src/checksum.c` computes).

A checksum field is computed over the structure **with the four checksum bytes
skipped** (not zeroed), which is what `jfs_crc_struct()` does: it chains
`jfs_crc32c_update()` over the bytes before the field and over the bytes after
it.  `tests/corrupt.py` reimplements the same rule so it can build records
that are "valid but wrong".

---

## 1. Image layout

| block(s)      | region      | contents                                        |
|---------------|-------------|-------------------------------------------------|
| 0             | superblock  | geometry, written once by `format`              |
| 1 – 2         | checkpoint  | two checkpoint slots, written alternately       |
| 3 – 50        | journal     | circular array of 48 blocks (write ahead log)   |
| 51 – end      | log         | append-only log-structured storage, 2 halves    |

The layout is derived from the image size at format time:

* journal: fixed at `JFS_JOURNAL_BLOCKS` = 48 blocks (192 KiB);
* log: everything else, split into two equal halves used alternately, so that
  a compaction can write the next half while the current one still holds the
  authoritative copy of the filesystem;
* an image must leave at least 48 blocks per log half
  (`JFS_MIN_LOG_HALF_BLOCKS`), so 1 MiB is the smallest image that is
  accepted.  A 1 MiB image has 102 blocks per half, an 8 MiB image 2021.

Example, 1 MiB image: `total_blocks = 256`, `checkpoint_block = 1`,
`journal_block = 3`, `journal_blocks = 48`, `log_block = 51`,
`log_blocks = 204` (2 × 102, one block unused).

## 2. Superblock (block 0)

```
offset  size  field             meaning
     0     4  magic             0x4A534652 "JSFR"
     4     2  version           format version (1)
     6     2  block_size        4096
     8     8  total_blocks      image size in blocks
    16     8  checkpoint_block  first checkpoint slot (1)
    24     8  journal_block     first journal block (3)
    32     4  journal_blocks    journal length in blocks (48)
    36     8  log_block         first log block (51)
    44     8  log_blocks        log length in blocks (both halves)
    52     4  max_inodes        252
    56     4  max_file_bytes    64768 (16 blocks x 4048 bytes of payload)
    60     4  flags             bit 0 JFS_SB_CLEAN: unmounted cleanly
    64     4  image_id          random id, repeated in every record
    68    16  created_at        seconds + nanoseconds since the epoch
    84     4  crc               CRC-32C over the whole block, field skipped
    88  4008  pad               zero
```

Mount rejects the image when the magic or version is wrong ("not a JournalFS
image"), when the checksum fails, when the geometry is inconsistent, or when
the file is shorter than `total_blocks` says.

`flags` is rewritten on every mount (`CLEAN` cleared) and unmount (`CLEAN`
set); the checkpoint is what actually describes the filesystem, so a lost
superblock flag costs a recovery scan, not data.

## 3. Checkpoint (blocks 1 and 2)

Two slots are written alternately.  Every write goes to the slot that is *not*
the current one, is followed by `fsync()`, and only then does the filesystem
start using it; the older slot keeps the previous consistent description until
the new one is durable.  A slot that was interrupted mid-write fails its
checksum and is ignored, so at least one of the two slots always describes a
state that existed.

```
offset  size  field             meaning
     0     4  magic             0x4A434B50 "JCKP"
     4     2  version           1
     6     2  flags             reserved, 0
     8     8  seq               monotonically increasing; larger seq wins
    16     8  applied_txid      every journal transaction <= this is applied
    24     4  log_half          0 or 1: the log half in use
    28     8  log_head          blocks used in that half
    36     8  log_seq           last log record sequence number
    44     8  imap_addr         log address of the current inode map record
    52     8  next_txid        next transaction id to hand out
    60     8  journal_start     first block of the live journal window
    68     4  journal_used      blocks in the live journal window
    72     4  n_inodes         inodes in use
    76     8  live_bytes        live bytes in the log (diagnostics)
    84     8  replayed_total    transactions replayed over the image's life
    92     4  crc               CRC-32C over the whole block, field skipped
    96  4000  pad               zero
```

`journal_start`/`journal_used` are a **hint**, not the truth: mount re-derives
the window by scanning the journal region (see `journal_protocol.md`), because
a stale hint must never make recovery read a torn record.

## 4. Journal region (blocks 3 – 50)

A circular array of 48 blocks holding *journal records*.  Each record occupies
whole blocks:

```
offset  size  field         meaning
     0     4  magic         0x4A4A524E "JJRN"
     4     2  version       1
     6     2  type          1 = OP, 2 = COMMIT, 3 = ABORT, 4 = CHECKPOINT
     8     8  txid           transaction this record belongs to
    16     8  lsn            global record sequence (strictly increasing)
    24     4  seq            record index inside the transaction
    28     4  nblocks        blocks occupied by this record
    32     4  payload_len    payload bytes
    36     4  payload_crc    CRC-32C over the payload
    40     4  image_id       must match the superblock
    44     4  ops            number of redo operations in the payload
    48     4  hdr_crc        CRC-32C over bytes 0..55 (this field skipped)
    52     4  flags          reserved, 0
    56   ...  payload        redo operations, zero padded to `nblocks` blocks
```

An `OP` payload is a sequence of redo operations:

```
offset  size  field    meaning
     0     2  op       1 = INODE, 2 = DATA, 3 = DIRBLK
     2     2  flags    reserved
     4     4  ino      target object
     8     4  index    block index inside the object
    12     4  length   payload bytes that follow
    16     4  crc      CRC-32C over the payload
    20   ...  payload  inode image / data bytes / directory block image
```

A `COMMIT` record has `payload_len = 0` and is the only thing that makes a
transaction durable.

## 5. Log region (blocks 51 – end)

The log region is the log-structured home of **all** filesystem content: file
data, directory blocks, inodes and the inode map.  It is append-only; a new
version of an object is appended and the old version stays where it is until a
compaction stops copying it.

Each log record occupies whole blocks:

```
offset  size  field         meaning
     0     4  magic         0x4A4C4F47 "JLOG"
     4     2  version       1
     6     2  type          1 = IMAP, 2 = INODE, 3 = DATA, 4 = DIRBLK
     8     8  addr          self address: (half << 32) | block offset
    16     8  seq           append sequence, strictly increasing
    24     4  obj_ino       object this record belongs to
    28     4  block_index   index inside the object
    32     4  payload_len   payload bytes
    36     4  payload_crc   CRC-32C over the payload
    40     4  image_id      must match the superblock
    44     4  hdr_crc       CRC-32C over bytes 0..47 (this field skipped)
    48   ...  payload       object image, zero padded to whole blocks
```

A log address is a 64-bit value: the high 32 bits are the half (0 or 1) and
the low 32 bits are the block offset inside that half.  **Block offset 0 in
each half is permanently unusable** so that a zero address can mean "no
block" — `format` starts the log at offset 1 and compaction never copies
anything to offset 0.  This avoids the classic bug where address 0 is both a
valid block and the "empty" sentinel.

A record is one block when its payload is 4048 bytes or less, which is why
`JFS_DATA_BYTES` is 4048 (48 byte header + 4048 = 4096).

### 5.1 Inode map record (type 1)

The inode map is a single record holding 252 entries of 16 bytes:

```
offset  size  field     meaning
     0     4  magic     0x4A494D50 "JIMP"
     4     4  count      number of USED entries
     8     4  next_ino   next inode number to consider
    12     4  crc        CRC-32C over the structure, field skipped
    16  4032  entries[252]
```

Each entry is `ino` (4), `state` (4: 0 free / 1 used), `addr` (8: log address of
that inode's newest record).  The whole map is rewritten (appended as a new
record) whenever an inode address changes.  Slot *i* of the map corresponds to
inode number *i + 1*; inode 1 is the root directory.

### 5.2 Inode record (type 2)

An inode record's payload is a 276 byte inode:

```
offset  size  field          meaning
     0     4  ino            inode number (0 = unused)
     4     2  type           1 = file, 2 = directory
     6     2  mode           permission bits (0777 mask)
     8     4  uid            simulated owner id
    12     4  gid            simulated group id
    16     4  flags          bit 0 JFS_INODE_FREE = deleted
    20     4  generation       bumped on every rewrite
    24     8  size           logical size in bytes
    32    16  ctime          creation time
    48    16  mtime          modification time
    64     4  nblocks        valid entries in addrs[]
    68     4  linkcount      1 for files, 2 + subdirectories for directories
    72     8  self           log address of this record
    80   128  addrs[16]      log addresses of the data/directory blocks
   208    64  blklen[16]     meaningful bytes in each of those blocks
   272     4  provided_mask  transaction-only field, always 0 on disk
```

`provided_mask` exists so that one transaction can carry a data block *and* an
inode that refers to it: bit *i* tells the apply step "block *i* of this inode
is being replaced by this transaction, use its new address".  Persisting it
would make a metadata-only update (a `chmod`) look like it re-declared blocks,
so it is cleared on load and always written as 0.

The inode is *self checking*: `jfs_inode_sane()` verifies that `blklen[i]` is
at most 4048 for every block below `nblocks`, that no block below `nblocks`
claims zero length but a valid address, that `addrs[i]`/`blklen[i]` are zero
for every block at or above `nblocks`, and that the sizes add up to `size`.
Corrupt inodes are refused rather than used.

### 5.3 Data record (type 3) and directory block record (type 4)

A data record's payload is up to 4048 bytes of file content; the file's last
block may be shorter, and `blklen[i]` records how many bytes are meaningful.
Bytes past `blklen[i]` are zero padding written by the engine.

A directory block record's payload is a 4016 byte directory block:

```
offset  size  field     meaning
     0     4  magic     0x4A444952 "JDIR"
     4     4  count      number of valid entries
     8     4  crc        CRC-32C over the structure, field skipped
    12     4  pad        zero
    16  4000  ents[100]
```

A directory entry is 40 bytes: `name[32]` (NUL terminated, at most 31 bytes),
`ino` (4), `type` (2), `flags` (2, bit 0 = valid).  Empty slots have
`flags = 0`, which is what makes a directory block rewrite a pure
"copy, change one entry, append" operation.

## 6. What makes a state recoverable

The rules that the whole design rests on:

1. **The checkpoint is the root of trust.**  It names the log half, the head
   position, the inode map address and the applied transaction id.  Everything
   reachable from it is validated; nothing else is trusted.
2. **A log record is never overwritten while it is live.**  The log is
   append-only inside a half, and the other half is only overwritten after a
   checkpoint describes the copy that has taken over.
3. **A journal record is never freed before a checkpoint makes it
   unnecessary.**  The window is advanced only after the checkpoint naming the
   new window is durable.
4. **Every structure carries its own checksum**, so a torn write is detected
   rather than interpreted.  A damaged checkpoint slot falls back to the other
   slot; a damaged journal record truncates the window and is reported.

## 7. Limits

| limit                     | value            | where it comes from              |
|---------------------------|------------------|----------------------------------|
| block size                | 4096 bytes       | `JFS_BLOCK_SIZE`                 |
| file data per block       | 4048 bytes       | header + payload = one block     |
| blocks per file           | 16 (direct only) | `JFS_MAX_FILE_BLOCKS`            |
| maximum file size         | 64768 bytes      | 16 × 4048                        |
| inodes per image          | 252              | one inode map record             |
| directory entries/block   | 100              | 4016 byte directory block        |
| file name length          | 31 bytes         | `JFS_NAME_MAX`                   |
| image size                | 1 MiB – 64 MiB   | log halves, `format_check_size`  |
| journal size              | 48 blocks        | `JFS_JOURNAL_BLOCKS`             |

There is no indirect block, no sparse file representation, no extended
attribute and no real user model: the filesystem simulates a single user
(uid/gid 1000) and stores permission bits without enforcing them against the
host.  This is by design and is stated again in the README's limitations
section.
