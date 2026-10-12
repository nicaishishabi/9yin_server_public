#!/usr/bin/env bash
# Linux / macOS / Git Bash / WSL 启动脚本（仅依赖已编译的二进制，不再需要 cmake）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN=""
for c in "$ROOT/build/protocol_probe" "$ROOT/build/Release/protocol_probe" "$ROOT/build/protocol_probe.exe" "$ROOT/build/Release/protocol_probe.exe"; do
  if [[ -f "$c" ]]; then BIN="$c"; break; fi
done
if [[ -z "$BIN" ]]; then
  echo "[错误] 找不到已编译程序，请先执行: bash scripts/compile.sh"
  exit 1
fi
export NINEYIN_SERVER_ROOT="${NINEYIN_SERVER_ROOT:-$ROOT}"
echo "[start] binary=$BIN"
echo "[start] root=$NINEYIN_SERVER_ROOT"
exec "$BIN" --root "$NINEYIN_SERVER_ROOT" --listen "${1:-127.0.0.1:19061}"
