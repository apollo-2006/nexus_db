# NexusDB

[![demo](https://github.com/apollo-2006/nexus_db/actions/workflows/pages.yml/badge.svg)](https://github.com/apollo-2006/nexus_db/actions/workflows/pages.yml)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**[Crash it in your browser →](https://apollo-2006.github.io/nexus_db/)** The engine is
compiled to WebAssembly and writes a real log and real SSTables into an in-memory
filesystem. Write until the memtable flushes, watch compaction merge files down the
levels, trace a read and see which files its Bloom filters ruled out, decode any file,
then kill the database without a clean shutdown and watch the log replay.

An embedded log-structured merge-tree key-value store written in C++17, in the shape of
LevelDB and RocksDB. Around it: a Python/FastAPI REST layer over the C++ engine via
`ctypes` FFI, and a React/TypeScript dashboard for live telemetry.

Written to understand why storage engines are built this way: why writes go to a log
before they go anywhere useful, why on-disk files are immutable, and why deleting a key
means writing more data rather than less.

## Stack

| Layer | Technology |
|---|---|
| Storage Engine | C++17: skip list memtable, write-ahead log, levelled SSTables with Bloom filters |
| API | Python/FastAPI: `ctypes` FFI over `libnexus.so` |
| Frontend | React/Vite/TypeScript: Tailwind CSS, Lucide Icons |

## Quick Start

Requires: `g++`, Python 3.8+, Node.js/npm.

**Terminal 1: build the engine.** Produces both the `nexus_db` benchmark binary and
`libnexus.so` for the API to load:
```bash
make clean && make
./nexus_db          # optional: 1M-write benchmark, see Benchmarks below
```

**Terminal 2: start the API:**
```bash
cd backend
pip install -r requirements.txt
uvicorn api:app --reload
# Listening on http://localhost:8000
```

**Terminal 3: launch the dashboard:**
```bash
cd frontend
npm install
npm run dev
```

## Internals

**Why an LSM tree at all.** A B-tree updates records in place, which turns a write into a
random seek. An LSM tree only ever appends: writes go to memory and are later flushed to
disk in one sequential pass, trading read complexity for write throughput. That trade is
the whole design, and everything below follows from it.

**MemTable**: Writes land in a probabilistic skip list in RAM: expected O(log n)
insertion and point lookup, and it stays sorted without any of the rebalancing a
tree needs. Sorted order is what makes the eventual flush a single sequential write.

**Write-Ahead Log**: Every write is appended to `wal_N.log` before it touches the
MemTable, so the log records the intent before memory records the effect. `flush()` after
each append pushes the bytes to the OS, which survives a process crash. It is *not* an
`fsync`, so a power cut can still lose the tail of the log. Logs are numbered, one per
memtable, and a log is deleted only once the file holding its records is named by the
manifest.

**Recovery**: Opening a database replays every log it finds, oldest first, into a fresh
MemTable, so a crash between flushes loses nothing that reached one. A crash in the middle of an append
leaves a partial record at the end of the file; replay stops at the last complete record
and truncates the rest, because a torn tail left in place would sit in front of every
record appended after it, where the next replay could never reach them. Lengths are
bounded by the bytes actually left in the file, so a corrupt length is never allocated.

**SSTables**: A full MemTable is handed to a worker thread, which writes it to an
immutable `data_N.sst` in one sequential pass while the writer carries on against a fresh
one. Each file carries what a reader needs to avoid it:

```
magic "NXSSTBL", version, count, tombstones, offsets      48 byte header
records    [flags][klen][key][vlen][value], sorted by key
index      every 16th record, plus the first and last: [key][offset]
bloom      10 bits per key, 7 probes
```

**Levels**: Level 0 holds whatever a flush produced, so its files overlap in key range and
a read has to consult all of them, newest first. Every level below is disjoint and sorted,
so at most one file per level can hold a given key, found by binary search over key ranges.
A level is compacted when it passes its byte budget, ten times the level above; level 0
when it reaches four files.

**Compaction**: A merge takes its inputs plus every overlapping file one level down, keeps
the newest version of each key and writes the result one level down. This is what reclaims
the space an overwrite or a delete costs. A tombstone is only dropped once nothing below
can still hold the value it covers, and a level thick with tombstones earns a compaction
on its own, since otherwise a delete can sit above its value indefinitely while every
level is within budget.

**Manifest**: `MANIFEST` names every live file and its level. Each update writes
`MANIFEST.tmp` and renames it, so it lands whole or not at all. A crash during a compaction
leaves outputs the manifest does not name and inputs that it does; the next open removes
the first and reads the second. Files are retired by reference count rather than unlinked,
so a compaction cannot delete a file a read is about to open.

**Reads**: MemTable first, then the immutable MemTable waiting to be flushed, then the
levels, so a newer value always shadows an older one. A file whose key range or Bloom
filter rules the key out is never opened. The lock is held only long enough to look at the
memtables and take a reference to the files to search; opening and scanning them happens
with no lock held, so reads scale with threads rather than queueing behind each other.

**Tombstones**: A delete writes a record with the tombstone flag set rather than removing
anything. The old value is still sitting in a file further down; the flag is what makes the
search stop and report "not found". This is why a delete in an LSM tree *adds* data, and
why the space comes back only when compaction reaches it.

## Known limits

Where this departs from a real storage engine:

* **No range scans or iteration.** Only point `get`. The API layer keeps its own Python
  record of written keys to fake a key browser, because the engine cannot enumerate. The
  files are sorted and indexed, so the merging iterator this needs is the next thing to
  build.
* **The log is flushed, not `fsync`ed.** A process crash loses nothing; a power cut can
  lose the tail of the log. Real durability means paying for the sync, and the write path
  would want group commit before it does.
* **Compaction reads whole files into memory.** A few megabytes at these file sizes. An
  engine holding terabytes would stream its inputs through a heap instead.
* **Writes serialise.** Readers no longer wait for each other or for a read that has gone
  to disk, but every `put` still takes the exclusive lock, and one worker does all the
  flushing and compacting.
* **No block cache and no checksums.** Reads go through the page cache, and a bit flipped
  on disk is returned as data rather than detected.

## Tests

```bash
make test
```

`tests/test_db.cpp` builds with AddressSanitizer and UndefinedBehaviorSanitizer and covers
overwrites, tombstones shadowing older files, re-adopting files on reopen, the Bloom filter
itself, and the recovery paths. The crash tests fork a child that writes and then
`_exit()`s, skipping every destructor, so the parent can only read back what the log
preserved: unflushed puts and deletes, a crash after several flushes, a torn final record,
writes appended after a torn tail, and a log that starts with a garbage length.

The rest are about the tree: a value that happens to equal the old `@@TOMBSTONE@@` marker
is still a value, compaction merges flushes into fewer files and moves them off level 0,
rewriting every key five times does not keep six copies on disk, deleting every key
releases the space, a file the manifest does not name is removed at open while the ones it
names are untouched, and four readers stay correct against a tree that a writer and the
compactor are changing underneath them. That last one also passes under ThreadSanitizer
with no reports:

```bash
g++ -std=c++17 -O1 -g -fsanitize=thread -I./include tests/test_db.cpp src/db.cpp -o test_tsan -pthread
./test_tsan
```

## Web demo

`web/db_web.cpp` wraps `NexusDB` for JavaScript and `web/build.sh` compiles it with the
unmodified engine using Emscripten, backed by its in-memory filesystem. GitHub Actions
runs `make test`, builds the demo, and publishes it to Pages on every push to `main`.

```bash
web/build.sh                          # needs em++ on PATH
python3 -m http.server -d web/dist    # then open http://localhost:8000
```

The FastAPI and React dashboard in `backend/` and `frontend/` is the local way to drive the
engine; the web demo replaces the HTTP hop with direct WebAssembly calls.

## Benchmarks

`./nexus_db [num_writes] [reads_per_kind]` starts from an empty directory, writes
sequential keys with ~45 byte JSON values, waits for the worker to settle, then times point
reads. Numbers below are 1,000,000 writes and 200 reads per kind, Ryzen 9 5900XT, g++
`-O3`, data directory on tmpfs:

| | |
|---|---|
| 1M writes | **5.55 s**, ~180k ops/s, then 229 ms to settle |
| tree | 30 SSTables over 3 levels, 63.0 MB |
| bytes written | 63.0 MB flushed + 145.5 MB compacted for 51.8 MB of keys and values, **4.0x** |
| read, key still in the memtable | p50 **0.5 µs**, p99 1.9 µs |
| read, random existing key | p50 **12.6 µs**, p99 20.6 µs |
| read, oldest key | p50 **5.3 µs** |
| read, missing key, sorts outside every file | p50 **0.1 µs** |
| read, missing key, sorts inside the data | p50 **0.2 µs**, p99 0.8 µs |

Reads scale with threads, because a read holds the lock only while it looks at the
memtables and takes references to the files it will search:

| threads | 1 | 2 | 4 | 8 |
|---|---|---|---|---|
| random existing keys | 82k/s | 174k/s | 338k/s | 662k/s |

### What each piece bought

Every number here is from this machine, same workload, measured against the commit before
the change.

**Bloom filters and the sparse index.** Before them a lookup opened every file and scanned
it from the first record. 200,000 writes into 9 files, mean latency:

| case | before | after |
|---|---|---|
| random existing key | 19035.7 µs | 11.0 µs |
| oldest key | 39.7 µs | 5.0 µs |
| missing, outside range | 39.6 µs | 0.1 µs |
| missing, inside range | 33217.8 µs | 0.7 µs |

**Compaction.** 200,000 keys written five times over:

| | before | after |
|---|---|---|
| files | 55 | 15 |
| on disk | 69.2 MB | 25.5 MB, for 12.0 MB of live data |
| random read | 9.9 µs | 11.4 µs |

Writes pay for that: 727k ops/s before, 275k ops/s after on the same 200,000 sequential
writes, because compaction rewrites data the flush already wrote. That is the trade an LSM
tree makes, and the benchmark prints the amplification so it stays visible.

**The shared lock.** Reads used to hold one mutex for the whole read, files included:

| threads | 1 | 2 | 4 | 8 |
|---|---|---|---|---|
| before | 97k/s | 92k/s | 89k/s | 86k/s |
| after | 93k/s | 180k/s | 364k/s | 682k/s |
