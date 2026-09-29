#!/bin/bash
# ============================================================
#  P2/P3 高级功能测试 - 构建 & 运行脚本
#  测试: Payload, Priority, Metrics, HotSwap, Cluster, Stats
#
#  用法: bash test_case/run_test_advanced.sh
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo "  Building Advanced Features Test..."
echo "============================================"

make -f Makefile.advanced clean 2>/dev/null || true
make -f Makefile.advanced -j$(nproc)

echo ""
echo "============================================"
echo "  Running Advanced Features Test..."
echo "  Tests: Payload, Priority, Metrics,"
echo "         HotSwap, Cluster, Stats"
echo "============================================"
echo ""

./test_advanced_features
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo ">>> ALL ADVANCED TESTS PASSED <<<"
else
    echo ">>> SOME TESTS FAILED (exit code: $EXIT_CODE) <<<"
fi

exit $EXIT_CODE
