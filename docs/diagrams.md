# JournalFS diagrams

ASCII diagrams, so they travel with the source and stay readable in a terminal,
a diff, or a printed report.  Each one is referenced from the documents named
in its heading.

---

## 1. Image layout (`disk_format.md`)

```
 one image = one host file, block = 4096 bytes, little-endian structures

 +--------+--------+--------+--------------------------------+-------------------------------+
 | block0 | blk1-2 | blk3..50                        ... 50 | blk51 ...                     |
 | super  | ckpt   | journal region (48 blocks, circular)    | log region, two halves        |
 | block  | A | B  |  ^ records are appended at the end      |  half 0        | half 1       |
 +--------+--------+--------+--------------------------------+-------------------------------+
      |        |            |                                              |
      |        |            |  holds redo descriptions of transactions      holds the current
      |        |            |  that are not applied yet; reclaimed by       version of every object
      |        |            |  checkpoints                                  (append only)
      |        |
      |        +--- the checkpoint names: log half, log head, imap address,
      |             applied txid, journal window     (two slots, newest valid seq wins)
      |
      +--- geometry: sizes of every region, image id, clean flag
```

Log address encoding:

```
  63            32 31                         0
 +----------------+----------------------------+
 | half (0 or 1)  | block offset in that half  |     offset 0 is reserved
 +----------------+----------------------------+     = "no block" sentinel
```

## 2. Transaction timeline (`journal_protocol.md`)

```
  in memory       journal region            log region             checkpoint
  ----------      --------------            ----------             ----------
  build redo
  ops
      |
      |   (1) OP records ---------->  [OP ....]      crash: pre_write
      |   (2) fsync                fsync #1         crash: post_write
      |                                             crash: post_sync
      |   (3) COMMIT record ------>  [COMMIT]       crash: post_commit
      |   (4) fsync                fsync #2  <-- DURABLE
      |                                             |
      |   (5) data/dir blocks ------------------>  append DATA/DIR
      |   (6) inode records -------------------->  append INODE     crash: after_imap
      |   (7) inode map record ---------------->   append IMAP
      |                                                             |
      |   (8) write the other slot ------------------------------>  [slot B]  crash: checkpoint_mid
      |       fsync + adopt                                       [slot B]  crash: checkpoint_post
      |   (9) reclaim journal records  <-- window advances         |
      |  (10) compact if the half is > 3/4 full --> copy live records to the other half
                                                                    crash: compact_mid/post
```

## 3. Mount / recovery flow (`recovery_design.md`)

```
            open image
                |
        superblock valid?  -- no -->  "not a JournalFS image" / checksum / truncated
                | yes
        best checkpoint of two slots  -- none -->  refuse: nothing describes the log
                | one
        re-derive the journal window
                |
        +-------+--------------------------------------------+
        | 1. validate the prefix the checkpoint declares      |
        |      (a damaged block truncates the window)         |
        | 2. extend over records newer than applied_txid      |
        |      (this finds a committed, unapplied transaction)|
        | 3. stop at txid <= applied_txid  (residue, not live)|
        | 4. a valid record beyond the stop point => damage   |
        +-------+--------------------------------------------+
                |
        group records into transactions
                |
        +-------+-----------------------------+
        | commit record found, txid > applied | -> replay (idempotent apply)
        | commit record found, txid <= applied| -> skip (already applied)
        | no commit record                    | -> incomplete, ignored, counted
        | committed but payload unreadable    | -> refuse to mount (report why)
        +-------+-----------------------------+
                |
        write a checkpoint (window becomes empty) -> mount
```

## 4. Log compaction (`architecture.md`, `journal_protocol.md`)

```
   before:  half 0 (active, nearly full)             half 1 (scratch, old copy)
            [imap][inode][data][dead][data]...      [ ... previous generation ... ]
                     ^ live records scattered

   compaction:  walk the live tree from the checkpoint
                (imap -> inodes -> data/dir blocks)
                append each live object into half 1 starting at offset 1

   after:   half 0 (scratch)                half 1 (active)
            [ old generation, unreferenced ] [imap][inode][data][data]...
                                              ^ compact, no dead records
   then:    checkpoint switches log_half to 1 and points at the new addresses
```

A crash before that final checkpoint leaves the old half and the old checkpoint
intact, so the image is still exactly the previous consistent state.

## 5. Layers and file map (`architecture.md`)

```
   +--------------------------------------------------------------+
   |  src/main.c  src/cli.c        commands, options, exit codes   |
   +--------------------------------------------------------------+
   |  src/filesystem.c             paths, files, dirs, permissions |
   +--------------------------------------------------------------+
   |  src/transaction.c            commit protocol, fsync barriers |
   |  src/journal.c                journal records, window         |
   +--------------------------------------------------------------+
   |  src/recovery.c  src/crash.c  mount scan, replay, injection   |
   +--------------------------------------------------------------+
   |  src/storage.c  src/checkpoint.c   log append, compaction     |
   +--------------------------------------------------------------+
   |  src/disk_image.c             create/open image, geometry     |
   +--------------------------------------------------------------+
        shared: src/util.c  src/checksum.c  src/inode.c
                src/directory.c  src/permissions.c
```

## 6. Directory and file representation

```
  directory inode (type = DIR)                 file inode (type = FILE)
  addrs[0] -> dir block 0                      addrs[0] -> data block 0
  addrs[1] -> dir block 1  (a directory         addrs[1] -> data block 1
              grows like a file does)                       ...
  each dir block: 100 entries of 40 bytes       blklen[i] = meaningful bytes
   name[32] ino type flags                      (the last block may be short)
   empty slots have flags = 0

  inode map record (one log record, rewritten on change):
   entry[i] = { ino = i+1, state = FREE/USED, addr = newest inode record }
   inode 1 = root "/"
```

## 7. Single user model

```
  journalfs runs as one simulated user (uid 1000, gid 1000).
  Permission bits are stored, checked and reported (chmod, mode string),
  but they are *not* a security boundary: the process can read the image
  file itself.  This is a model of the checks a filesystem performs, not
  an isolation mechanism.  See README "Limitations".
```
