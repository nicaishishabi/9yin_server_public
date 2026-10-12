#!/usr/bin/env bash
# 九阴 C++ 服务端编译脚本
# 用法：
#   ./scripts/build.sh          # Release 构建
#   ./scripts/build.sh debug    # Debug 构建
#   ./scripts/build.sh clean    # 清理后重新构建
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
BUILD_TYPE="Release"

case "${1:-}" in
  debug|Debug|DEBUG)
    BUILD_TYPE="Debug"
    ;;
  clean|Clean|CLEAN)
    echo "[build] cleaning ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
    ;;
  ""|release|Release|RELEASE)
    ;;
  *)
    echo "Usage: $0 [release|debug|clean]"
    exit 1
    ;;
esac

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

echo "[build] configuring (${BUILD_TYPE}) ..."
cmake .. \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DNINEYIN_BUILD_TESTS=ON \
  -DNINEYIN_USE_MYSQL=ON

echo "[build] compiling ..."
cmake --build . -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "[build] running unit tests ..."
# 在部分环境（fuse 等）可执行位可能丢失，先尝试 chmod
chmod +x transport_key_test transport_frame_test protocol_probe 2>/dev/null || true
if [[ -x ./transport_key_test ]]; then
  ctest --output-on-failure || {
    echo "[build] ctest failed, trying direct run ..."
    ./transport_key_test
    ./transport_frame_test
  }
else
  # 回退：复制到 /tmp 再跑（兼容某些沙箱文件系统）
  cp -f transport_key_test transport_frame_test /tmp/ 2>/dev/null || true
  chmod +x /tmp/transport_key_test /tmp/transport_frame_test 2>/dev/null || true
  /tmp/transport_key_test
  /tmp/transport_frame_test
fi

echo ""
echo "[build] OK"
echo "  binary : ${BUILD_DIR}/protocol_probe"
echo "  run    : ${ROOT}/scripts/run.sh"
echo ""
