#!/usr/bin/env bash
# Build the WebAssembly target. Needs the Emscripten SDK activated
# (source .../emsdk/emsdk_env.sh). Single-threaded; serves on any static host.
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; cd "$DIR"
command -v emcc >/dev/null || { echo "emcc not found — source emsdk_env.sh first"; exit 1; }
mkdir -p build_wasm && cd build_wasm
emcmake cmake .. -DPOMIIGS_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
emmake make -j"$(nproc)"
echo "Built: build_wasm/POMIIGS.html (.js/.wasm). Serve with:"
echo "  cd build_wasm && python3 -m http.server 8000   # then open /POMIIGS.html"
