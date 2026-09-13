#!/usr/bin/env bash
# Compiles the storage engine to WebAssembly and assembles the demo site in
# web/dist. Needs em++ on PATH (https://emscripten.org). CI runs this for Pages.
set -euo pipefail
cd "$(dirname "$0")"
rm -rf dist && mkdir -p dist
em++ -std=c++17 -O2 -fexceptions -Wall -Wextra -I../include \
  ../src/db.cpp db_web.cpp \
  -fexceptions -sMODULARIZE=1 -sEXPORT_NAME=NexusDB -sENVIRONMENT=web \
  -sFORCE_FILESYSTEM=1 -sEXPORTED_RUNTIME_METHODS=FS,ccall,cwrap,UTF8ToString \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864 \
  -o dist/nexus_db.js
cp index.html app.js demo.css dist/
echo "built web/dist"
