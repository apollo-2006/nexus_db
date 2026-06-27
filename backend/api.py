from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
import ctypes
import os
import time

app = FastAPI()

# Enable CORS for the React frontend
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# --- Load the C++ Shared Library ---
lib_path = os.path.abspath("../libnexus.so")
if not os.path.exists(lib_path):
    raise RuntimeError(f"Cannot find {lib_path}. Run 'make' in the C++ directory first.")

nexus_lib = ctypes.CDLL(lib_path)

# Define C-types signatures for safety
nexus_lib.db_create.argtypes = [ctypes.c_char_p]
nexus_lib.db_create.restype = ctypes.c_void_p

nexus_lib.db_put.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]

nexus_lib.db_get.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
nexus_lib.db_get.restype = ctypes.POINTER(ctypes.c_char)

nexus_lib.db_free_string.argtypes = [ctypes.POINTER(ctypes.c_char)]

# Initialize Database Instance
db_ptr = nexus_lib.db_create(b"./dashboard_data")

# --- Metrics Tracking ---
metrics = {
    "total_reads": 0,
    "total_writes": 0,
    "start_time": time.time(),
    "known_keys": set() # Simulating a key browser since our C++ DB doesn't have an iterator yet
}

# --- API Routes ---
class PutRequest(BaseModel):
    key: str
    value: str

@app.post("/api/put")
def put_data(req: PutRequest):
    nexus_lib.db_put(db_ptr, req.key.encode('utf-8'), req.value.encode('utf-8'))
    metrics["total_writes"] += 1
    metrics["known_keys"].add(req.key)
    return {"status": "success", "key": req.key}

@app.get("/api/get/{key}")
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
        "recent_keys": list(metrics["known_keys"])[-10:] # Return last 10 keys for browser
    }