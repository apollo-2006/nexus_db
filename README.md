# NexusDB

A high-performance embedded key-value store written in C++17, inspired by LevelDB and RocksDB. Features a full-stack interface: a Python/FastAPI REST layer via FFI and a React/TypeScript dashboard for live telemetry.

## Stack

| Layer | Technology |
|---|---|
| Storage Engine | C++17 — Skip List MemTable, WAL, SSTables |
| API | Python/FastAPI — `ctypes` FFI over `libnexus.so` |
| Frontend | React/Vite/TypeScript — Tailwind CSS, Lucide Icons |

## Quick Start

Requires: `g++`, Python 3.8+, Node.js/npm.

**Terminal 1 — build the engine:**
```bash
cd nexus_db
make clean && make
```

**Terminal 2 — start the API:**
```bash
cd nexus_db/backend
pip install -r requirements.txt
uvicorn api:app --reload
# Listening on http://localhost:8000
```

**Terminal 3 — launch the dashboard:**
```bash
cd nexus_db/frontend
npm install
npm run dev
```

## Internals

**MemTable** — Writes land in a probabilistic skip list in RAM, providing O(log n) insertion and point lookup without tree-rebalancing overhead.

**Write-Ahead Log** — Every operation is appended to `active.wal` (unbuffered, O_DIRECT) before touching the MemTable, ensuring durability across crashes.

**SSTables** — When the MemTable exceeds 1 MB, it is frozen and flushed to an immutable `.sst` file on disk via a sequential write.

**Tombstones** — Deletes write a `@@TOMBSTONE@@` marker. The key is masked at read time and physically reclaimed during background compaction.

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

Then access the dashboard at `http://127.0.0.1:5174` — not `localhost`.
