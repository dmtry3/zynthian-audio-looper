#!/bin/bash
# Собирает плагин в build/audio_looper.lv2 (на Pi и в WSL одинаково).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build/audio_looper.lv2"
mkdir -p "$B"
cp "$ROOT/lv2/manifest.ttl" "$ROOT/lv2/audio_looper.ttl" "$B/"
g++ -std=c++17 -O2 -fPIC -shared -Wall -Wextra -fno-exceptions -fno-rtti -fvisibility=hidden \
    $(pkg-config --cflags lv2) \
    -o "$B/audio_looper.so" "$ROOT/lv2/audio_looper.cpp" -lm
echo "Built: $B"
