# NexusDB

[![demo](https://github.com/apollo-2006/nexus_db/actions/workflows/pages.yml/badge.svg)](https://github.com/apollo-2006/nexus_db/actions/workflows/pages.yml)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**[Crash it in your browser →](https://apollo-2006.github.io/nexus_db/)** The engine is
compiled to WebAssembly and writes a real WAL and real SSTables into an in-memory
filesystem. Write until the memtable flushes, trace a read through the files, decode an
SSTable, then kill the database without a clean shutdown and watch the log replay.

An embedded log-structured merge-tree key-value store written in C++17, in the shape of
LevelDB and RocksDB. Around it: a Python/FastAPI REST layer over the C++ engine via
`ctypes` FFI, and a React/TypeScript dashboard for live telemetry.

Written to understand why storage engines are built this way: why writes go to a log
before they go anywhere useful, why on-disk files are immutable, and why deleting a key
means writing more data rather than less.

## Stack

| Layer | Technology |
|---|---|
| Storage Engine | C++17: Skip List MemTable, WAL, SSTables |
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

**Write-Ahead Log**: Every write is appended to `active.wal` before it touches the
MemTable, so the log records the intent before memory records the effect. `flush()` after
each append pushes the bytes to the OS, which survives a process crash. It is *not* an
`fsync`, so a power cut can still lose the tail of the log.

**Recovery**: Opening a database replays `active.wal` into a fresh MemTable, so a crash
between flushes loses nothing that reached the log. A crash in the middle of an append
leaves a partial record at the end of the file; replay stops at the last complete record
and truncates the rest, because a torn tail left in place would sit in front of every
record appended after it, where the next replay could never reach them. Lengths are
bounded by the bytes actually left in the file, so a corrupt length is never allocated.

**SSTables**: When the MemTable passes 1 MB it is serialized to an immutable
`data_N.sst` file in one sequential pass and a fresh MemTable takes over. Files are never
modified after they are written, which is what makes reads lock-free against writers and
compaction a matter of writing a new file rather than editing an old one.

**Reads**: MemTable first, then SSTables from newest to oldest, so a newer value always
shadows an older one for the same key. Existing files are re-adopted on open, and the
counter resumes past the highest index found, so reopening a database does not lose or
overwrite what is already there.

**Tombstones**: A delete writes a `@@TOMBSTONE@@` marker rather than removing anything.
Because SSTables are immutable, the old value is still sitting in an older file; the
marker is what makes the newest-first read stop and report "not found". This is why a
delete in an LSM tree *adds* data.

## Known limits

Where this departs from a real storage engine:

* **There is no compaction.** SSTables accumulate and are never merged, so tombstoned and
  overwritten values are never physically reclaimed and the file count grows without
  bound.
* **Reads scan SSTables linearly.** No Bloom filter, no index block, no binary search
  within a file: every lookup walks each file from the start until it finds the key. A
  miss therefore costs a pass over every byte on disk, about 50 ms at 67 MB (see
  Benchmarks). Bloom filters and a sparse index per file are the first thing this needs.
* **No range scans or iteration.** Only point `get`. The API layer keeps its own Python
  record of written keys to fake a key browser, because the engine cannot enumerate.
* **`@@TOMBSTONE@@` is a magic string, not a flag.** A value that legitimately equals that
  string would be read back as a deletion.
* **One global mutex.** Every `put` and `get` serializes on it, so the skip list's
  concurrency-friendliness is not actually exploited.

## Tests

```bash
make test
```

`tests/test_db.cpp` builds with AddressSanitizer and UndefinedBehaviorSanitizer and covers
overwrites, tombstones shadowing older files, re-adopting SSTables on reopen, and the
recovery paths. The crash tests fork a child that writes and then `_exit()`s, skipping
every destructor, so the parent can only read back what the WAL preserved: unflushed puts
and deletes, a crash after several flushes, a torn final record, writes appended after a
torn tail, and a log that starts with a garbage length.

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
sequential keys with ~45 byte JSON values, then times point reads of four kinds. Numbers
below are 1,000,000 writes and 200 reads per kind, Ryzen 9 5900XT, g++ `-O3`, data
directory on tmpfs:

| | |
|---|---|
| 1M writes | **1.02 s**, ~985k ops/s, 49 SSTables, 67.3 MB |
| read, key still in the memtable | p50 **0.2 µs**, p99 0.7 µs |
| read, random existing key | p50 **25.5 ms**, p99 51.5 ms |
| read, oldest key (last file scanned) | p50 **49.6 ms** |
| read, missing key (every file scanned) | p50 **50.3 ms**, p99 52.4 ms |

On an NVMe btrfs volume the same run writes at ~263k ops/s. Every `put` flushes the WAL to
the kernel, so write throughput is bounded by the `write` syscall, not the skip list.
Read latencies are unchanged, since the SSTables are served from the page cache either way.

The read numbers are the linear scan described under Known limits: a missing key walks all
67 MB. They used to be much worse. The scanner originally bounds-checked each record with
a seek to end of file and back, and skipped values with `seekg`, which discards the
`ifstream` buffer. Both turned every record into syscalls. Computing the file size once
and skipping with `ignore()` took a missing-key read from **4.53 s to 50 ms** on the
same run.

## License

MIT. See [LICENSE](LICENSE).

## WSL2 / Ubuntu

If the frontend is unreachable on `localhost`, bind Vite to IPv4 explicitly:

```ts
// frontend/vite.config.ts
export default defineConfig({
  plugins: [react()],
  server: {
    host: '0.0.0.0',
    port: 5174,
    watch: { usePolling: true }
  }
})
```

Then open the dashboard at `http://127.0.0.1:5174`, not `localhost`.
