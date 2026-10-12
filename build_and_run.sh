#!/usr/bin/env bash
# 一键编译并启动
set -euo pipefail
cd "$(dirname "$0")"
./scripts/build.sh
./scripts/run.sh "$@"
