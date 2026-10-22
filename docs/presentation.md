# JournalFS — presentation outline (12 slides, ~10 minutes)

Each slide: what goes on it, what to say, and the evidence to have ready.
Speaker notes are written to be spoken, not read.

---

## Slide 1 — Title

**On the slide:** JournalFS — a crash-consistent journaling + log-structured
filesystem simulator in C.  Your name, date, repository path.

**Say:** "I built a filesystem that survives crashes, in user space, with a disk
image of its own.  This talk is about the three things that make it work — the
journal, the log, and recovery — and the evidence that they do."

## Slide 2 — The problem: a torn write is not "old data"

**On the slide:** a small diagram of one operation touching four places
(data block → inode → inode map → directory) with a lightning bolt between two
of them.  Caption: *"the image now holds a state that never existed."*

**Say:** updating a file is never one write.  If the power fails between two of
them, the tree is inconsistent in a way that can lose other data too.  A
filesystem must never expose that state.

## Slide 3 — The answer: a notebook and a bookmark

**On the slide:** the four-step protocol as a numbered list, plus the two
barriers marked as fences.

1. write "what I am about to do" into the journal, `fsync`
2. write "DONE" (`COMMIT`), `fsync` ← the durability point
3. do the work in the log
4. note in the checkpoint how far the log is known good; then the journal page
   can be reused

**Say:** the journal is the notebook, the log region *is* the filesystem, the
checkpoint is the bookmark.  If the power fails, mount reads the notebook:
pages with DONE are redone, pages without it are dropped.

## Slide 4 — Why two barriers and an idempotent replay

**On the slide:** the timeline with the 14 crash points marked.

**Say:** with one barrier the device may reorder the commit record before the
redo records, and recovery would find a commit it cannot replay.  Two barriers
remove that.  Replay is idempotent because it only ever appends new versions,
which is why a crash *during* the apply is survivable.

**Have ready:** `docs/journal_protocol.md` §8 table (point → expected outcome).

## Slide 5 — The log-structured half

**On the slide:** two log halves, live objects copied into the free half by
compaction, addresses in inodes, block offset 0 reserved.

**Say:** the log is append-only: new versions are appended, old ones stay until
compaction copies the live tree into the other half.  A crash mid-compaction
leaves the old half and old checkpoint intact — the image is simply the previous
consistent state.

## Slide 6 — Recovery has to be suspicious

**On the slide:** the four-step window re-derivation, and the three damage
verdicts (use the other checkpoint / truncate and report / refuse to mount).

**Say:** freeing a journal record is logical — the bytes stay on the medium
until overwritten — so once the ring wraps, old records look perfectly valid.
Recovery re-derives the window from the checkpoint and only extends it with
transactions the checkpoint has not accounted for.  The rule is the transaction
id: a record that is not newer than the checkpoint is residue, not data.

## Slide 7 — What a crash leaves behind: 14 named moments

**On the slide:** the crash point table with "old value", "new value", "old or
new, never a mix".

**Say:** the engine can kill itself deterministically at 14 points.  Each has a
documented expectation, and each is asserted by the test suite — including two
crashes in a row, a crash during recovery, and a crash during compaction.

## Slide 8 — Evidence: 264 checks, zero failures

**On the slide:** the six suites with their check counts and the sanitizer
line; a screenshot-free listing of `results/*.log`.

```
functional 56 · errors 49 · persistence 26 · corruption 35 · crash 57 · stress 41
                            = 264 checks, 0 failed
        the same 264 under AddressSanitizer + UndefinedBehaviorSanitizer
```

**Say:** the suites are the argument, not the demo: one damages a single
structure at a time and requires the filesystem to refuse or to report; one
drives randomized workloads and re-reads every file after every operation; one
walks all the crash points.

## Slide 9 — Evidence: what journaling costs

**On the slide:** the benchmark table (or the PNG from
`results/benchmark.png`).

**Say:** the cost shows up exactly where the guarantee is paid for: metadata
churn +20 %, directory operations +29 %, 1.9× on bulk writes, and nothing
measurable on reads.  Recovery is milliseconds because the journal bounds how
much redo can be pending.  These are simulator numbers — the honest claim is
about the *shape*, and the machine's noise floor is a few percent.

## Slide 10 — Six bugs the tests found in my own design

**On the slide:** the list from `docs/report.md` §6 — address 0 as both a valid
block and the "empty" sentinel; residue mistaken for live journal records;
growing/shrinking inside the last block; a durable transaction failing to
apply; a persisted transaction-only field; a journal sequence that restarted
per session.

**Say:** this is the part I would keep if I had to keep one slide.  Each bug
was found by a suite, fixed, and is now covered by a named check.

## Slide 11 — Limitations, stated plainly

**On the slide:** the seven limitations from `README.md`, in short form.

**Say:** the image is a host file and a crash is `_exit`; the ordering is real,
the disk is not.  One process, one simulated user.  64 KiB files, 252 inodes,
192 KiB of journal.  No fsck: damage that redo cannot fix is reported, not
repaired.

## Slide 12 — Where it goes next, and the demo

**On the slide:** group commit, inode-map deltas, indirect blocks, a Python
reference reader, an offline consistency reporter.  Then: "Demo —
`docs/demo_script.md`".

**Say:** the immediate next step is group commit — batching the fsyncs is what
would move the metadata numbers.  Now let me break a filesystem on purpose.

---

## Timing

| slides | minutes |
|---|---|
| 1–3 (problem + protocol) | 3 |
| 4–6 (barriers, log, recovery) | 3 |
| 7–9 (crash points, evidence, cost) | 3 |
| 10–12 (bugs, limits, next steps) | 2, then the 3–5 minute demo |

## Questions to expect

* **"Is this a real filesystem?"** No — it is a simulator with a real protocol.
  It does not mount in the kernel, does not do multi-process access, and does
  not model a device cache.  Point at limitations 1 and 2.
* **"Why not just use FUSE?"** FUSE would add a dependency and an interface to
  the kernel without changing what is being demonstrated, and it would make
  deterministic crash injection harder.
* **"Why redo and not undo logging?"** Redo makes replay idempotent and avoids
  keeping old versions reachable; see `docs/architecture.md` §4.
* **"What happens if the journal is damaged?"** Refusal with a reason, unless
  the damaged part is beyond the committed records; see
  `docs/corruption` cases in `results/test_corruption.log`.
* **"How do you know the numbers?"** `make benchmark`, medians of five runs,
  raw CSV in `results/benchmark.csv`, and the noise floor stated in the report.
