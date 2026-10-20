#!/usr/bin/env bash
# Codec unit tests (no nix, no Qt): g++ and nlohmann-json headers.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
NL=${NLOHMANN_INCLUDE:-$( (find /nix/store -maxdepth 3 -path "*nlohmann_json*/include" 2>/dev/null || true) | sed -n 1p)}
g++ -std=c++20 -Wall -DEXAMPLES_SRC="\"$HERE/../examples/src\"" -I"$HERE/../src" ${NL:+-I"$NL"} "$HERE/codec_test.cpp" -o "${TMPDIR:-/tmp}/lez_codec_test"
"${TMPDIR:-/tmp}/lez_codec_test"
