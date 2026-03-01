#!/bin/bash
# ============================================================
#  Actor 间消息通信测试 - 构建 & 运行脚本
#  用法: chmod +x run_test.sh && ./run_test.sh
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo "  Building Actor Inter-Message Test..."
echo "============================================"

# 清理旧产物
make -f Makefile.test clean 2>/dev/null || true

# 构建测试
make -f Makefile.test -j$(nproc)

echo ""
echo "============================================"
echo "  Running Actor Inter-Message Test..."
echo "============================================"
echo ""

# 运行测试
./test_actor_msg
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo ">>> ALL TESTS PASSED <<<"
else
    echo ">>> SOME TESTS FAILED (exit code: $EXIT_CODE) <<<"
fi

exit $EXIT_CODE
