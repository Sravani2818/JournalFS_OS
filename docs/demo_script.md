# JournalFS demonstration script (3–5 minutes)

Two crash scenarios on one image, then the summary.  Every command below has
been run; the expected output is what the current build prints.  Copy-paste
one block at a time and talk over it.

Set-up (do this before the timer starts):

```bash
cd ~/JournalFS
make                 # build
mkdir -p /tmp/demo   # scratch space for the images
```

---

## 0. What the audience is about to see (20 s)

> This is a filesystem simulator.  Everything lives inside one disk image file:
> a superblock, two checkpoint slots, a 48-block write-ahead journal, and a
> log-structured area holding the actual data.  I am going to kill the process
> at two precise moments — once *before* a transaction is durable and once
> *after* — and show what the next mount makes of each one.

## 1. Create the filesystem and the file (30 s)

```bash
B=build/journalfs
IMG=/tmp/demo/demo.img

$B -q -c "format $IMG --size 1m"
$B -i $IMG -c "mount" -c "create /hello" -c 'write /hello HELLO' -c "cat /hello" -c unmount
```

Expected:

```
formatted /tmp/demo/demo.img (1024 KiB, 256 blocks of 4096 bytes)
created /hello (0644)
wrote /hello
HELLO
unmounted /tmp/demo/demo.img (checkpoint written, image marked clean)
```

> `/hello` now contains HELLO, and the image was shut down cleanly.

## 2. Crash *before* the commit is durable (60 s)

The transaction below builds its redo records, writes them into the journal,
syncs them, and is killed *before* the COMMIT record exists.  By the rule in
`docs/journal_protocol.md`, that transaction must not survive.

```bash
$B -i $IMG --quiet --crash post_sync -c "mount" -c 'append /hello _WORLD'
echo "exit status: $?"
$B -i $IMG -c "mount" -c "cat /hello" -c unmount
```

Expected (the crash line is on stderr, the recovery line on stdout):

```
[crash] injected crash at point 'post_sync' (journal records synced, no commit record)
        -- terminating with _exit(99)
        -- no image write was flushed by this process
exit status: 99
mounted /tmp/demo/demo.img:
  inodes: 252 max
  log blocks: 102 per half
  checkpoint seq: 3
  applied txid: 2
recovery ran: 0 transaction(s) replayed, 1 incomplete ignored

HELLO
```

> Two things to point at: nothing was replayed, and the one transaction in the
> journal was counted as *incomplete* — its redo blocks are physically there,
> and they are ignored because no COMMIT record certified them.

> The redo blocks are physically on the medium — and they were ignored, because
> no COMMIT record certified them.  The file is byte-for-byte what it was.

## 3. Crash *after* the commit is durable (60 s)

Same command, one crash point later: the COMMIT record has been written and
synced, but the process dies before the change is applied to the log.

```bash
$B -i $IMG --quiet --crash post_commit_sync -c "mount" -c 'append /hello _WORLD'
echo "exit status: $?"
$B -i $IMG -c "mount" -c "cat /hello" -c unmount
```

Expected:

```
[crash] injected crash at point 'post_commit_sync' (durable commit, changes not applied)
        -- terminating with _exit(99)
        -- no image write was flushed by this process
exit status: 99
mounted /tmp/demo/demo.img:
  inodes: 252 max
  log blocks: 102 per half
  checkpoint seq: 4
  applied txid: 3
recovery ran: 1 transaction(s) replayed, 1 incomplete ignored

HELLO_WORLD
```

> Now one transaction was replayed — the committed one — while the incomplete
> leftover from step 2 is still ignored.  Two different fates decided by a
> single record: the one that says COMMIT.

> This time the transaction is durable.  The mount that follows replays it, and
> the file holds the new value — the change the user was told had happened.

Then show that recovery did its job exactly once, and that the journal is
clean again:

```bash
$B -i $IMG -c "mount" -c "cat /hello" -c journal -c unmount
$B -c "inspect $IMG" | sed -n '1,5p'
```

Expected: the content stays `HELLO_WORLD`, the journal reports
`journal window: start=8 blocks, used=0 of 48, applied_txid=4` — the window is
empty again — and `inspect` shows the image `CLEAN` with the same content.

## 4. Show the machinery (45 s)

```bash
$B -c "inspect $IMG"          # superblock, both checkpoint slots, imap, journal
$B -i $IMG -c mount -c stats -c "fill /big 40000 A" -c check -c unmount
```

Expected highlights: two checkpoint slots with increasing `seq`,
`journal_start`/`used`, the inode map counts, and then `stats` counters
transactions committed / syncs / bytes written, plus
`check: filesystem is consistent`.

## 5. Prove it with the suites, not with the demo (30 s)

```bash
make test-crash | tail -3
make test-all   | tail -8
```

Expected:

```
test_crash_recovery: 57 passed, 0 failed
test_functional: 56 passed, 0 failed
test_errors: 49 passed, 0 failed
test_persistence: 26 passed, 0 failed
test_corruption: 35 passed, 0 failed
test_crash_recovery: 57 passed, 0 failed
test_stress: 41 passed, 0 failed
```

> 264 checks.  The crash suite walks all fourteen injection points and asserts
> the expected outcome for each; the corruption suite damages one structure at
> a time and requires the filesystem to refuse or to report, never to guess.

## 6. Close (20 s)

```bash
sed -n '1,20p' results/benchmark.md
```

> Journaling costs about 9–31 % on transactional workloads (the two fsync
> barriers per transaction) and about 1.8× on bulk writes, and recovery is
> milliseconds because the journal bounds how much redo can ever be pending.
> These are simulator numbers: the image is a file on this host, and a crash is
> `_exit`, so what is demonstrated is the *ordering* of writes and syncs, not
> the behaviour of a physical disk.

---

## If something goes wrong on stage

* `format` refuses with "image exists": add `--force`, or use a fresh
  `/tmp/demo/demo2.img`.
* The crash line does not appear: check that `--crash` is spelled with the
  point name from `build/journalfs --help` (e.g. `post_sync`), and that the
  command after `-c` is quoted so the shell does not split it.
* `make test-all` on a laptop takes under a minute; if it is slower, run
  `make test-crash` only and mention the rest.

## Backing commands for the Q&A

```bash
# what does the engine think the journal holds?
build/journalfs -c "inspect /tmp/demo/demo.img" | tail -8

# damage one journal record and watch the refusal
tests/corrupt.py jrec-flip-payload /tmp/demo/demo.img 0
build/journalfs -i /tmp/demo/demo.img -c "mount" -c "cat /hello"

# the same, but damaging something recovery can live with
tests/corrupt.py ckpt-zero /tmp/demo/demo.img 0
build/journalfs -i /tmp/demo/demo.img -c "mount" -c "cat /hello" -c check -c unmount
```
