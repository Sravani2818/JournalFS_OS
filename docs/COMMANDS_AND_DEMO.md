# JournalFS — Commands, Functions & Presentation Manual

---

## 1. What is JournalFS in Plain English?

Think of **JournalFS** as a **Crash-Proof Mini Operating System for Storage**.

### The Problem it Solves
Normally, when an operating system saves a file, it has to update multiple places on the disk:
1. The **Inode** (metadata: size, permissions, modification time).
2. The **Directory** (links the filename to the inode).
3. The **Data Blocks** (the actual text or binary content).

If power cuts or your computer crashes in the middle of these steps, the filesystem becomes corrupted (e.g., the directory points to a file whose data blocks don't exist yet).

### How JournalFS Prevents Corruption
JournalFS uses **Write-Ahead Logging (WAL)**:
1. Before touching the actual storage blocks, it writes a "receipt" of the intended changes into a special log called the **Journal**.
2. Once that receipt is stamped with a durable **COMMIT** record, it applies the changes to the disk.
3. If a crash occurs:
   - **Before COMMIT**: JournalFS ignores the incomplete transaction (old clean state is kept).
   - **After COMMIT**: On restart, JournalFS reads the receipt and automatically finishes writing the changes (**Replay Recovery**).

The filesystem is **never** left in a broken, half-written state!

---

## 2. Interactive CLI Commands (What Each Command Does)

When you are inside the `jfs:/>` prompt, these are the commands you can run:

| Command | Example | What it actually does |
|---|---|---|
| `format <img_file>` | `format demo.img` | Creates and formats a fresh 4MB virtual disk image. |
| `mount <img_file>` | `mount demo.img` | "Plugs in" the disk, runs crash recovery, and prepares it for reading/writing. |
| `unmount` | `unmount` | Safely flushes all cached data to disk and "unplugs" the image. |
| `mkdir <dir>` | `mkdir /notes` | Creates a new folder/directory. |
| `create <file>` | `create /notes/todo.txt` | Creates a new empty file. |
| `write <file> <text>` | `write /notes/todo.txt Hello` | Replaces the contents of a file with the given text. |
| `append <file> <text>`| `append /notes/todo.txt World` | Adds text to the end of an existing file without deleting old content. |
| `cat <file>` | `cat /notes/todo.txt` | Reads and prints the text stored inside the file to your screen. |
| `ls [path]` | `ls /notes` | Lists the files and folders inside a directory. |
| `stat <file>` | `stat /notes/todo.txt` | Shows file metadata: Inode number, size in bytes, permissions, and log block address. |
| `chmod <mode> <file>` | `chmod 0400 /notes/todo.txt` | Changes permissions (e.g. `0400` makes a file read-only; `0644` makes it normal read-write). |
| `rm <file>` | `rm /notes/todo.txt` | Deletes a file and frees its data blocks. |
| `rmdir <dir>` | `rmdir /notes` | Deletes an empty directory. |
| `df` | `df` | Shows disk space: total size, live data bytes used, and free blocks remaining. |
| `stats` | `stats` | Displays performance counters: transactions committed, syncs, and LRU buffer cache hits/misses. |
| `journal` | `journal` | Inspects the journal ring buffer to show active un-checkpointed transaction records. |
| `check` | `check` | Runs a consistency audit across all blocks, inodes, and directory trees to verify 0 corruption. |
| `sync` | `sync` | Flushes all dirty buffer frames and writes a durable checkpoint immediately. |
| `set group-commit <N>` | `set group-commit 8` | Configures group-commit batching threshold at runtime. |
| `set color <on\|off\|auto>` | `set color on` | Toggles ANSI terminal color highlighting for output differentiation. |
| `exit` | `exit` | Safely unmounts and exits the JournalFS shell. |

---

## 3. Complete C Function Reference

### High-Level Filesystem API (`src/fs.c`, `include/journalfs.h`)
- `int jfs_format(const char *path, const jfs_format_opts *opts, char *err, size_t errsz)`: Writes superblock, dual checkpoints, and empty root inode.
- `int jfs_mount(const char *path, bool quiet, char *err, size_t errsz)`: Validates disk image, starts buffer cache, and runs recovery.
- `int jfs_unmount(void)`: Flushes dirty cache frames, writes clean checkpoint, and closes disk.
- `int jfs_create(const char *path, uint32_t mode)`: Allocates an inode and creates an empty file within an atomic transaction.
- `int jfs_mkdir(const char *path, uint32_t mode)`: Allocates an inode and creates a directory entry in the parent directory.
- `int jfs_write(const char *path, const void *data, size_t len, uint64_t off, bool append, bool do_truncate)`: Writes data through the journal into log-structured storage.
- `int jfs_read(const char *path, jfs_buf *out)`: Reads the complete data of a file into memory.
- `int jfs_readdir(const char *path, void (*emit)(...), void *ud)`: Iterates through entries in a directory.
- `int jfs_rm(const char *path, bool recursive)`: Removes a file or directory tree.
- `int jfs_rmdir(const char *path)`: Removes an empty directory.
- `int jfs_rename(const char *from, const char *to)`: Atomically renames or moves a file.
- `int jfs_chmod(const char *path, uint32_t mode)`: Modifies permission mode bits (`0755`, `0644`, `0400`).
- `int jfs_stat(const char *path, jfs_inode *out)`: Retrieves inode metadata (size, blocks, timestamps).
- `int jfs_df(uint64_t *total, uint64_t *used, uint64_t *inodes, uint64_t *max_inodes, uint64_t *log_free)`: Returns disk utilization metrics.
- `int jfs_check_consistency(struct jfs_fs *fs, char *err, size_t errsz)`: Verifies structural consistency of all on-disk metadata.

### Memory Management: LRU Block Buffer Cache (`src/bcache.c`, `include/bcache.h`)
- `int jfs_bcache_init(jfs_bcache *bc, int fd, uint32_t capacity)`: Initializes 64 fixed cache frames (256 KiB) and LRU list.
- `int jfs_bcache_read_block(jfs_bcache *bc, uint64_t block_no, void *buf)`: Reads block from cache frame or fetches from disk on cache miss.
- `int jfs_bcache_write_block(jfs_bcache *bc, uint64_t block_no, const void *buf)`: Writes block to frame, marks dirty bit, and promotes to MRU.
- `int jfs_bcache_flush_all(jfs_bcache *bc)`: Writes all dirty frames back to the image file.
- `int jfs_bcache_flush_range(jfs_bcache *bc, uint64_t start_block, uint64_t end_block)`: Flushes a specific range of blocks.
- `void jfs_bcache_get_stats(const jfs_bcache *bc, jfs_bcache_stats *out)`: Retrieves cache metrics (`hits`, `misses`, `evictions`, `writebacks`).

### Transaction & Write-Ahead Logging (`src/journal.c`, `include/journal.h`)
- `int jfs_txn_begin(struct jfs_fs *fs, jfs_txn **out)`: Starts a new transaction with a monotonic `txid`.
- `int jfs_txn_commit(jfs_txn *tx, bool *needs_apply)`: Appends redo operations, fsyncs, writes `COMMIT` record, and fsyncs again.
- `int jfs_txn_apply(jfs_txn *tx)`: Replays redo operations into the log-structured storage.
- `void jfs_txn_abort(jfs_txn *tx)`: Cancels transaction in memory without writing to disk.
- `int jfs_journal_append(struct jfs_fs *fs, const jfs_journal_rec_hdr *hdr, const void *payload, size_t payload_len)`: Appends a record to the journal ring.

### Crash Fault Injection & Recovery (`src/journal.c`, `include/crash.h`, `include/recovery.h`)
- `int jfs_recover(struct jfs_fs *fs, jfs_recovery_stats *stats)`: Scans the journal, checks CRC-32C, replays committed transactions, and discards uncommitted transactions.
- `void jfs_crash_set(jfs_crash_point p)`: Sets the active crash injection point (e.g. `post_commit_sync`).
- `void jfs_crash_check(jfs_crash_point p, const char *tag)`: Simulates sudden power loss / crash by terminating process immediately with `_exit(99)`.

### Log-Structured Storage & Checkpoints (`src/fs.c`, `include/storage.h`)
- `int jfs_log_append(...)`: Appends immutable records to the active half of the log.
- `int jfs_log_compact(struct jfs_fs *fs)`: Migrates live records to the alternate half and reclaims dead blocks.
- `int jfs_checkpoint_write_slot(struct jfs_fs *fs, const jfs_checkpoint *ck, int slot, bool sync)`: Writes dual-slot checkpoint (slot 0 or 1).

---

## 4. Live Presentation Script (Step-by-Step)

Run these exact commands during your presentation or demonstration:

### Step 1: Start and Mount the Disk
```bash
./journalfs -i demo.img
```
Inside the prompt:
```text
jfs:/> format demo.img
formatted demo.img (4096 KiB, 1024 blocks of 4096 bytes)

jfs:/> mount demo.img
mounted demo.img: 252 inodes max, log 486 blocks per half, checkpoint seq 1, applied txid 0
recovery: journal window is empty, nothing to redo
```
> **What to explain:** "We format a 4MB virtual disk image and mount it. Mounting automatically validates the superblock and checks the journal for crash recovery."

---

### Step 2: Create Files and Directories
```text
jfs:/> mkdir /docs
created directory /docs

jfs:/> create /docs/report.txt
created /docs/report.txt (0644)

jfs:/> write /docs/report.txt Operating Systems Project
wrote /docs/report.txt

jfs:/> cat /docs/report.txt
Operating Systems Project

jfs:/> ls /docs
- rw-r--r--       25  report.txt       ino=3
1 entry
```
> **What to explain:** "JournalFS supports hierarchical directories, inodes, file size tracking, and regular file read/write operations."

---

### Step 3: Demonstrate Permissions & Protection
```text
jfs:/> chmod 0400 /docs/report.txt
changed mode of /docs/report.txt to 0400

jfs:/> write /docs/report.txt Hacked!
error: write: permission denied

jfs:/> chmod 0644 /docs/report.txt
changed mode of /docs/report.txt to 0644
```
> **What to explain:** "We have a simulated POSIX permission model. Setting mode `0400` makes the file read-only, so subsequent write operations are denied with an error."

---

### Step 4: Inspect Memory Management & Buffer Cache
```text
jfs:/> stats
Engine counters:
  txns committed: 4
  txns replayed:  0
  redo ops:       8
  bytes written:  28672
  syncs:          10
  bcache_hits:    18
  bcache_misses:  6
  bcache_evictions: 0
  bcache_writebacks: 4
```
> **What to explain:** "JournalFS manages memory using a fixed 64-frame (256KB) LRU block buffer cache. We track cache hits, misses, evictions, and dirty write-backs."

---

### Step 5: Check Integrity and Unmount
```text
jfs:/> check
check: filesystem is consistent: 3 inode(s), 61440 live bytes in the log

jfs:/> unmount
unmounted demo.img (checkpoint written, image marked clean)

jfs:/> exit
```
> **What to explain:** "The `check` command verifies that all inodes, directories, and log blocks satisfy all filesystem invariants with 0 errors."

---

### Step 6: The Grand Finale — Live Crash Recovery Demo
Exit JournalFS and run this in your terminal:

```bash
# 1. Inject a crash right after the journal commits:
./journalfs -i demo.img --crash post_commit_sync -c "mount demo.img" -c "append /docs/report.txt --RECOVERED_DATA"
```
**Output:**
```text
mounted demo.img:
  inodes: 252 max
  log blocks: 486 per half
  checkpoint seq: 6
  applied txid: 4
recovery: journal window is empty, nothing to redo

[crash] injected crash at point 'post_commit_sync' (durable commit, changes not applied)
        -- terminating with _exit(99)
        -- no image write was flushed by this process
```

Now restart JournalFS and see it automatically recover:
```bash
./journalfs -i demo.img -c "mount demo.img" -c "cat /docs/report.txt" -c "unmount"
```
**Output:**
```text
mounted demo.img:
  inodes: 252 max
  log blocks: 486 per half
  checkpoint seq: 7
  applied txid: 5
recovery ran: 1 transaction(s) replayed, 0 incomplete ignored

Operating Systems Project--RECOVERED_DATA
unmounted demo.img (checkpoint written, image marked clean)
```
> **What to explain:** "Even though the crash killed the process with `_exit(99)` before the changes reached the main log storage, the journal was durable. Upon restarting, the recovery engine detected the committed transaction and replayed it seamlessly. The data was 100% preserved!"
