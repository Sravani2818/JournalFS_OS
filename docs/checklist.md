# JournalFS project checklist

Kept in the repository on purpose: it is the state of the project, not a plan
of what might happen.  Anything marked done has been built and tested; the
evidence is the log file named next to it.

## Build and tooling

- [x] `make` builds `build/journalfs` and `build/fsstress` — warning free
      (`-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wwrite-strings
      -Wpointer-arith -Wcast-align -Wformat=2`)
- [x] `make clean`, `make help`
- [x] `make smoke` — fast functional subset
- [x] `make test`, `make test-crash`, `make test-all`
- [x] `make sanitize` (ASan + UBSan build) and `make test-sanitize` (all six
      suites under the instrumented binary, leaks on)
- [x] `make benchmark` — writes `results/benchmark.{csv,md,png}`

## M1 — image, CLI, core file operations

- [x] custom disk image, superblock, geometry, `format` (refuses to clobber
      without `--force`)
- [x] CLI: 29 commands, options, script files, exit codes (0/1/2, 99 = crash)
- [x] create/write/append/read/cat/ls/stat/rm/rename/truncate/df/sync/
      mkdir/cd/pwd/rmdir/chmod/fill/hash/help/exit
- [x] `tests/test_functional.sh` — 56 checks, 0 failed
- [x] `tests/test_errors.sh` — 49 checks, 0 failed

## M2 — journal and transaction manager

- [x] journal record format (type, version, txid, seq, payload len, payload,
      two checksums) documented in `docs/journal_protocol.md` before use
- [x] two-phase commit with fsync barriers; COMMIT record as the durability
      point
- [x] crash injection at `pre_write`, `post_write`, `post_sync`, `post_commit`,
      `post_commit_sync`, `post_apply`, `after_imap`
- [x] room planned before the first write; clean refusal instead of a partly
      applied durable transaction
- [x] `--no-journal` baseline mode (with a warning; benchmark use only)

## M3 — log-structured storage, checkpoints, compaction

- [x] append-only log in two halves; block offset 0 reserved in each half
- [x] inode map, inode, data and directory records; addresses in the inode
- [x] double-buffered checkpoint; newest valid `seq` wins
- [x] compaction into the other half, crash points `compact_mid`/`compact_post`
- [x] checkpoint batching (`set checkpoint-interval N`) with a free-block guard
- [x] crash points `checkpoint_mid`, `checkpoint_post`
- [x] `tests/test_persistence.sh` — 26 checks, 0 failed

## M4 — recovery

- [x] mount time scan → group → validate → replay → checkpoint
- [x] window re-derived from the checkpoint plus newer transactions; residue
      distinguished from live records (transaction id rule)
- [x] damage reported (`journal_truncated`, `** DAMAGED **` in `inspect`,
      warning from `check`, warning at mount)
- [x] replay is idempotent; crash during recovery is survivable (`recover_mid`)
- [x] `tests/test_crash_recovery.sh` — 57 checks, 0 failed

## M5 — directories, metadata, permissions

- [x] directory data blocks, nested paths, `cd`/`pwd`, `rm -r`, `rmdir`
- [x] inode metadata (size, mode, times, link count, generation, log address)
- [x] simulated single-user permission checks (owner bits), `chmod`
- [x] persistence of content, metadata and deletions across restarts
- [x] data limit enforced (64 768 bytes), `df` reporting

## M6 — corruption, stress, benchmarks, documentation

- [x] `tests/test_corruption.c` — 35 checks, 0 failed (13 named cases,
      8-image fuzz, no silent repair)
- [x] `tests/test_bcache.c` — 15 checks, 0 failed (bounded LRU cache)
- [x] `tests/fsstress.c` + `tests/test_stress.c` — 41 checks, 0 failed
      (steady state, adopt, 7 crash points, crash during recovery, full image,
      determinism, clean inspect)
- [x] `tests/test_comparison.c` — 27 checks, 0 failed (Add-On 1 & 2:
      Without-journal failure modes vs With-journal recovery + group commit)
- [x] benchmarks with measured numbers, CSV, markdown and PNG plot
- [x] docs: README, architecture, disk format, journal protocol, recovery
      design, diagrams, testing plan, report, presentation, demo script

## Add-Ons (Syllabus Distinction & Specification Expansion)

- [x] **Add-On 1: Group Commit Engine & Benchmark (Stretch Goal)**:
      batching fsync barriers across $N$ transactions (`--group-commit N`,
      `set group-commit N`), measured 1.38× throughput gain on append benchmarks.
- [x] **Add-On 2: Without-Journal vs With-Journal Failure Modes Suite**:
      automated testing in `tests/test_comparison.c` proving uncommitted/torn
      write failure modes without WAL vs atomic recovery with WAL.
- [x] **Terminal UI Formatting**:
      Clean multi-line output across all terminal widths (`mounted` stats,
      `[crash]` reporting) preventing ugly wrapping or stacking.

## Final demonstration

- [x] `HELLO → JOURNALFS` demo with one pre-commit crash and one post-commit
      crash, scripted in `docs/demo_script.md` and reproducible from a terminal
- [x] clean-checkout build (`make`) and full suite (`make test-all`, 306 checks) verified
- [x] limitations disclosed (`README.md` §Limitations, `docs/report.md` §7)
