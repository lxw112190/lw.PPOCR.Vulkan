#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "$0")" && pwd)"
export LD_LIBRARY_PATH="$ROOT${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$ROOT/lw-ppocr-vulkan-http-service" --config "$ROOT/http-service.json" "$@"
