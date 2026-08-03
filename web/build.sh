#!/bin/sh
# Build the browser demo. Everything lands in a single self-contained dosemu.js
# (wasm embedded via SINGLE_FILE) so web/ serves as plain static files.
# Needs emscripten on PATH, or EMCC pointing at emcc.
set -e
cd "$(dirname "$0")/.."
EMCC=${EMCC:-emcc}
command -v "$EMCC" >/dev/null 2>&1 || { echo "emcc not found; set EMCC=/path/to/emcc"; exit 1; }

SOURCES=$(ls src/*.cpp | grep -v '/main\.cpp$')

echo "== building web/dosemu.js"
"$EMCC" -std=c++17 -O2 -Isrc \
    $SOURCES web/wasm_api.cpp \
    -o web/dosemu.js \
    -sMODULARIZE=1 -sEXPORT_NAME=createDosemu \
    -sSINGLE_FILE=1 -sALLOW_MEMORY_GROWTH=1 \
    -sEXPORTED_FUNCTIONS='["_dosemu_run","_dosemu_cwd","_malloc","_free"]' \
    -sEXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPU8","FS"]' \
    -sFORCE_FILESYSTEM=1 -sENVIRONMENT=web,worker,node --no-entry
ls -l web/dosemu.js
echo "done"
