# minidb

A relational database engine written from scratch in C++20, with a disk-backed page store, an LRU buffer pool, slotted-page tables, a hand-written SQL parser, B+ tree indexes, and a simple query planner. No external libraries.

```
minidb> CREATE TABLE users (id INT, name TEXT, age INT);
minidb> INSERT INTO users VALUES (1, 'ann', 30), (2, 'bob', 17), (3, 'cat', 45);
minidb> CREATE INDEX idx_age ON users (age);
minidb> SELECT name FROM users WHERE age > 20;
+------+
| name |
+------+
| ann  |
| cat  |
+------+
2 row(s) (0.11 ms)
plan: index scan using idx_age
```

## Architecture

```
  SQL text
     │
     ▼
 Tokenizer ──► Recursive-descent parser ──► AST (std::variant of statements)
                                               │
                                               ▼
                         Executor + planner (catalog in page 0)
                    ┌──────────────┴───────────────┐
                    ▼                              ▼
          Table heap (slotted pages)        B+ tree indexes
                    └──────────────┬───────────────┘
                                   ▼
                     Buffer pool (LRU, pin counts, dirty write-back)
                                   ▼
                     Disk manager (4 KB pages in one file)
```

| Layer | What it does |
|---|---|
| **Disk manager** | Reads and writes fixed 4 KB pages. Page *N* lives at byte offset *N* × 4096. |
| **Buffer pool** | Caches pages in a fixed number of frames. Pinned pages are never evicted. The least recently used unpinned page is evicted, and written back first if it's dirty. Eviction is O(1) using a list plus a hash map. |
| **Slotted pages** | Variable-length rows. The slot directory grows forward and the data grows backward. Deletes leave tombstones, so row IDs (page, slot) stay stable for indexes. |
| **Table heap** | Each table is a linked list of slotted pages, with insert, get, update, delete, and scan. An update that grows a row moves it and returns the new row ID. |
| **SQL front end** | Hand-written tokenizer and recursive-descent parser for `CREATE TABLE`, `CREATE INDEX`, `INSERT` (multi-row), `SELECT` (projection, `WHERE … AND …`, `JOIN … ON`, `LIMIT`), `UPDATE`, and `DELETE`. Errors report their location. |
| **Executor** | Resolves names and checks types before touching any rows. `UPDATE` and `DELETE` collect matching rows before changing them, so a scan never sees its own writes. |
| **B+ tree** | One node per page: 255 keys per leaf and 204 per internal node. Entries are ordered by (key, row ID), so duplicate keys work. Leaves are linked for range scans. Nodes split on insert, and deletes are lazy (no merging). |
| **Planner** | Turns `WHERE` conditions on an indexed column into a key range and uses an index scan, otherwise a full scan. Joins use an index nested-loop join when either side is indexed on the join column, otherwise a plain nested loop. |
| **Catalog** | Table schemas and index locations are stored in page 0, so everything survives restarts. |

## Benchmarks

Apple M-series MacBook, Release build. The whole dataset fits in the buffer pool (100% hit rate), so these measure in-memory query execution, not disk I/O. Times include SQL parsing and materializing the results.

**Index scan vs. full scan** (`bench_index`, 100,000 rows, average of 200 queries)

| Query | Full scan | Index scan | Speedup |
|---|---|---|---|
| Point lookup (`val = k`) | 7.44 ms | 0.002 ms | about 3,200× |
| Range (about 1% of rows) | 7.95 ms | 0.28 ms | 28× |

Building the index over 100K rows takes 132 ms, and the tree is 3 levels deep. Range queries gain less because each matching row must still be fetched from the table by row ID. That's why real planners fall back to full scans when a query matches a large share of the table.

**Join strategies** (`bench_join`, 2,000 users × 20,000 orders)

| Strategy | Time |
|---|---|
| Nested loop join | 98.4 ms |
| Index nested loop join | 7.5 ms (13× faster) |

## Testing

Eight test programs, one per component, each with its own `CHECK` macro (because `assert` is disabled in Release builds):
- **Storage and caching:** most tests use tiny buffer pools (3 or 4 frames) to force evictions, and check that data survives closing and reopening the file.
- **B+ tree:** 3,000 random keys with duplicates go into a tree with 4 keys per node (a deep tree with many splits), and every lookup, range scan, and delete is checked against `std::multimap`.
- **Index correctness:** two identical columns, one indexed and one not, must return the same rows for every query, through updates, deletes, row moves, and a restart.
- **Joins:** every join strategy must return exactly the same rows.

```bash
cmake -B build && cmake --build build
for t in build/test_*; do ./$t || break; done
```

## Build and run

Requires CMake 3.20+ and a C++20 compiler.

```bash
cmake -B build && cmake --build build
./build/minidb mydata.db        # REPL; .tables lists tables, .exit quits
./build/bench_index             # optional: rows, e.g. ./build/bench_index 500000
./build/bench_join              # optional: users orders
```

## Limitations and next steps

- No transactions, write-ahead log, or crash recovery. Dirty pages are written on eviction and on exit.
- Single-threaded, with no locking.
- Deleted rows and emptied B+ tree nodes aren't reclaimed (no page compaction or node merging).
- Indexes support INT columns only. There's no `NULL`, `ORDER BY`, aggregates, or joins of more than two tables.
- Possible next steps: a hash join for unindexed joins, a write-ahead log, and cost-based choice between index and full scans.
