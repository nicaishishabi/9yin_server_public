#!/usr/bin/env bash
# 九阴 C++ 服务端启动脚本
# 用法：
#   ./scripts/run.sh                          # 默认 127.0.0.1:19061
#   ./scripts/run.sh 0.0.0.0:19061            # 指定监听地址
#   NINEYIN_SERVER_ROOT=/path ./scripts/run.sh
#   ./scripts/run.sh --root /path --listen 0.0.0.0:19061
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${ROOT}/build/protocol_probe"

LISTEN="127.0.0.1:19061"
EXTRA_ARGS=()

# 解析简单参数
while [[ $# -gt 0 ]]; do
  case "$1" in
    --root)
      export NINEYIN_SERVER_ROOT="$2"
      shift 2
      ;;
    --listen)
      LISTEN="$2"
      shift 2
      ;;
    --help|-h)
      echo "Usage: $0 [--root DIR] [--listen HOST:PORT] [HOST:PORT]"
      echo ""
      echo "Environment:"
      echo "  NINEYIN_SERVER_ROOT   服务器根目录（含 resources/modern/share）"
      echo "  NINEYIN_SHARE_ROOT    可选，覆盖 item 路径"
      echo ""
      echo "Resource layout (relative to server root):"
      echo "  resources/modern/share/          <- 客户端 share 整体放这里"
      echo "  resources/legacy/Npc_Func.xml"
      echo "  data/roles.json  data/faculty.json"
      exit 0
      ;;
    *)
      # 位置参数当作 listen 地址
      if [[ "$1" == *:* ]]; then
        LISTEN="$1"
      else
        EXTRA_ARGS+=("$1")
      fi
      shift
      ;;
  esac
done

# 若未设置 NINEYIN_SERVER_ROOT，默认使用本仓库根目录
if [[ -z "${NINEYIN_SERVER_ROOT:-}" ]]; then
  export NINEYIN_SERVER_ROOT="${ROOT}"
fi

if [[ ! -x "${BIN}" ]]; then
  # 尝试修复可执行位
  if [[ -f "${BIN}" ]]; then
    chmod +x "${BIN}" 2>/dev/null || true
  fi
fi

if [[ ! -f "${BIN}" ]]; then
  echo "[run] binary not found: ${BIN}"
  echo "[run] please run: ./scripts/build.sh"
  exit 1
fi

# 部分环境无法直接执行 build 目录下的文件，复制到 /tmp
RUN_BIN="${BIN}"
if ! "${BIN}" --help &>/dev/null; then
  cp -f "${BIN}" /tmp/protocol_probe
  chmod +x /tmp/protocol_probe
  RUN_BIN="/tmp/protocol_probe"
fi

echo "[run] NINEYIN_SERVER_ROOT=${NINEYIN_SERVER_ROOT}"
echo "[run] listen=${LISTEN}"
echo "[run] binary=${RUN_BIN}"
echo ""

exec "${RUN_BIN}" --root "${NINEYIN_SERVER_ROOT}" --listen "${LISTEN}" "${EXTRA_ARGS[@]}"
