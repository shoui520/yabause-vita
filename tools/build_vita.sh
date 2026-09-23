#!/usr/bin/env bash
set -euo pipefail
: "${VITASDK:?Set VITASDK to the VitaSDK installation}"
root="$(cd "$(dirname "$0")/.." && pwd)"
cmake -S "$root" -B "$root/build" -G Ninja "$@"
cmake --build "$root/build" -j12
