#!/bin/bash
# ============================================================
#  P2/P3 高级功能测试 - 构建 & 运行脚本
#  测试: Payload, Priority, Metrics, HotSwap, Cluster, Stats
#
#  用法: chmod +x run_test_advanced.sh && ./run_test_advanced.sh
#
#  注意: 如果遇到 '\r' 错误，先执行:
#    sed -i 's/\r$//' run_test_advanced.sh Makefile.advanced test_advanced_features.cc
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo "  Building Advanced Features Test..."
echo "============================================"

# 清理旧产物
make -f Makefile.advanced clean 2>/dev/null || true

# 构建测试
make -f Makefile.advanced -j$(nproc)

echo ""
echo "============================================"
echo "  Running Advanced Features Test..."
echo "  Tests: Payload, Priority, Metrics,"
echo "         HotSwap, Cluster, Stats"
echo "============================================"
echo ""

# 运行测试
./test_advanced_features
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo ">>> ALL ADVANCED TESTS PASSED <<<"
else
    echo ">>> SOME TESTS FAILED (exit code: $EXIT_CODE) <<<"
fi

exit $EXIT_CODE
