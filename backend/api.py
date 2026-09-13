from contextlib import asynccontextmanager
from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
import ctypes
import os
import time


@asynccontextmanager
async def lifespan(app: FastAPI):
    yield
    # Runs ~NexusDB, which flushes the active memtable to an SSTable. Without
    # this the destructor never ran and every write since the last automatic
    # flush was lost when the API process exited.
    nexus_lib.db_destroy(db_ptr)


app = FastAPI(lifespan=lifespan)

# Enable CORS for the React frontend
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# --- Load the C++ Shared Library ---
# Resolved relative to this file, not the working directory. os.path.abspath("..")
# is CWD-relative, so the import blew up unless uvicorn happened to be started
# from inside backend/.
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
lib_path = os.path.join(PROJECT_ROOT, "libnexus.so")
if not os.path.exists(lib_path):
    raise RuntimeError(f"Cannot find {lib_path}. Run 'make' in the project root first.")

nexus_lib = ctypes.CDLL(lib_path)

# Define C-types signatures for safety
nexus_lib.db_create.argtypes = [ctypes.c_char_p]
nexus_lib.db_create.restype = ctypes.c_void_p

nexus_lib.db_put.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]

nexus_lib.db_get.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
nexus_lib.db_get.restype = ctypes.POINTER(ctypes.c_char)

nexus_lib.db_free_string.argtypes = [ctypes.POINTER(ctypes.c_char)]

nexus_lib.db_destroy.argtypes = [ctypes.c_void_p]

# Initialize Database Instance. The data directory is anchored to the project
# root for the same reason the library path is.
DATA_DIR = os.path.join(PROJECT_ROOT, "dashboard_data")
db_ptr = nexus_lib.db_create(DATA_DIR.encode("utf-8"))


# --- Metrics Tracking ---
metrics = {
    "total_reads": 0,
    "total_writes": 0,
    "start_time": time.time(),
    # Stands in for a key browser, since the engine cannot iterate yet. A dict
    # rather than a set: dicts keep insertion order, so "recent" means recent.
    "known_keys": {},
}

# --- API Routes ---
class PutRequest(BaseModel):
    key: str
    value: str

@app.post("/api/put")
def put_data(req: PutRequest):
    nexus_lib.db_put(db_ptr, req.key.encode('utf-8'), req.value.encode('utf-8'))
    metrics["total_writes"] += 1
    metrics["known_keys"].pop(req.key, None)
    metrics["known_keys"][req.key] = None
    return {"status": "success", "key": req.key}

@app.get("/api/get/{key:path}")
def get_data(key: str):
    metrics["total_reads"] += 1

    start_ns = time.perf_counter_ns()
    res_ptr = nexus_lib.db_get(db_ptr, key.encode('utf-8'))
    latency_us = (time.perf_counter_ns() - start_ns) / 1000.0

    if not res_ptr:
        raise HTTPException(status_code=404, detail="Key not found")

    value = ctypes.cast(res_ptr, ctypes.c_char_p).value.decode('utf-8')
    nexus_lib.db_free_string(res_ptr)

    return {"key": key, "value": value, "latency_us": round(latency_us, 2)}

@app.get("/api/metrics")
def get_metrics():
    uptime = time.time() - metrics["start_time"]
    return {
        "reads": metrics["total_reads"],
        "writes": metrics["total_writes"],
        "keys_tracked": len(metrics["known_keys"]),
        "uptime_seconds": round(uptime, 2),
        "recent_keys": list(metrics["known_keys"])[-10:][::-1],
    }