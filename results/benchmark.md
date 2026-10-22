# JournalFS benchmark

- image: 8 MiB formatted before each timed run, median of 3 run(s)
- binary: `build/journalfs` (baseline mode is `--no-journal`)

| workload | mode | time (ms) | ops/s | MB/s | payload bytes |
|---|---|---|---|---|---|
| create 240 files | journaled | 6691 | 143 | 0.00 | 16000 |
| create 240 files | baseline | 2237 | 429 | 0.01 | 16000 |
| append x3000 | journaled | 22441 | 133 | 0.00 | 60000 |
| append x3000 | baseline | 12733 | 235 | 0.00 | 60000 |
| rewrite 8 KiB | journaled | 8173 | 73 | 0.57 | 4915200 |
| rewrite 8 KiB | baseline | 3615 | 166 | 1.30 | 4915200 |
| bulk 62.5 KiB | journaled | 600 | 66 | 2.03 | 1280000 |
| bulk 62.5 KiB | baseline | 262 | 152 | 4.66 | 1280000 |
| churn x600 | journaled | 17624 | 102 | 0.00 | 0 |
| churn x600 | baseline | 5233 | 343 | 0.00 | 0 |
| mkdir x1500 | journaled | 28273 | 106 | 0.00 | 0 |
| mkdir x1500 | baseline | 10948 | 274 | 0.00 | 0 |
| read 32 KiB | journaled | 612 | 4901 | 153.19 | 98304000 |
| read 32 KiB | baseline | 551 | 5444 | 170.15 | 98304000 |
| append x3000 (gc=1) | per-tx | 21731 | 138 | 0.00 | 60000 |
| append x3000 (gc=4) | group-commit | 15734 | 190 | 0.00 | 60000 |
| append x3000 (gc=8) | group-commit | 18690 | 160 | 0.00 | 60000 |

## Crash recovery

| case | transactions pending | journal blocks used | redo payload bytes | mount + replay (ms) |
|---|---|---|---|---|
| clean mount (nothing to replay) | 0 | 0 | 0 | 35 |
| 16 small appends pending | 16 | 17 | 112 | 35 |
| 5 x 8 KiB rewrites pending | 5 | 15 | 40960 | 31 |

## Methodology and Notes

* `ops/s` measures transactions committed and persisted per second.
* Journaling performs synchronous write-ahead logging before updates reach home location.
* Memory management uses the bounded block buffer cache with fixed frames and LRU eviction.
