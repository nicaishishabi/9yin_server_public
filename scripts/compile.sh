#!/usr/bin/env bash
# Linux / macOS / Git Bash / WSL 编译脚本
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
bash scripts/build.sh
