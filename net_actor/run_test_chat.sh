#!/bin/bash
# ============================================================
#  MMO Actor 模型测试 - 构建 & 运行脚本
#  模拟: GatewayActor + 每玩家 PlayerActor + 跨 Actor 聊天广播
#
#  用法: chmod +x run_test.sh && ./run_test.sh
#
#  注意: 如果在 Linux 上遇到 '\r' 错误，先执行:
#    sed -i 's/\r$//' run_test.sh Makefile.chattest test_actor_msg.cc
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo "  Building MMO Actor Test..."
echo "============================================"

# 清理旧产物
make -f Makefile.chattest clean 2>/dev/null || true

# 构建测试
make -f Makefile.chattest -j$(nproc)

echo ""
echo "============================================"
echo "  Running MMO Actor Test..."
echo "  Architecture:"
echo "    GatewayActor (one per server)"
echo "      +-- PlayerActor1 (per-player)"
echo "      +-- PlayerActor2 (per-player)"
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
