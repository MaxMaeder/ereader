#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

EPUB="${1:-book.epub}"
NUM_PAGES="${2:-5}"
BUILD_DIR="build"
OUTPUT_DIR="output"

echo "=== Building ==="
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -4
cmake --build "$BUILD_DIR" --parallel 2>&1 | tail -4

echo ""
echo "=== Rendering $NUM_PAGES pages from $EPUB ==="
rm -rf "$OUTPUT_DIR"
"./$BUILD_DIR/epub_render" "$EPUB" "$OUTPUT_DIR" "$NUM_PAGES"

echo ""
echo "=== Done — output in $OUTPUT_DIR/ ==="
ls -lh "$OUTPUT_DIR/"
