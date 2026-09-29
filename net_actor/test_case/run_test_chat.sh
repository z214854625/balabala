#!/bin/bash
# ============================================================
#  MMO Actor 聊天广播测试 - 构建 & 运行脚本
#  模拟: GatewayActor + 每玩家 PlayerActor + 跨 Actor 聊天广播
#
#  用法: bash test_case/run_test_chat.sh
#
#  [修复] 原 Makefile.chattest 误用 test_actor_msg.cc 作为源，实际构建的是
#         test_actor_msg 而非 test_actor_msg_chat。已修正 Makefile.chattest 的
#         SOURCES 与 TARGET，本脚本对应改为 ./test_actor_msg_chat。
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo "  Building MMO Actor Chat Test..."
echo "============================================"

make -f Makefile.chattest clean 2>/dev/null || true
make -f Makefile.chattest -j$(nproc)

echo ""
echo "============================================"
echo "  Running MMO Actor Chat Test..."
echo "  Architecture:"
echo "    GatewayActor (one per server)"
echo "      +-- PlayerActor1 (per-player)"
echo "      +-- PlayerActor2 (per-player)"
echo "============================================"
echo ""

./test_actor_msg_chat
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo ">>> CHAT TEST PASSED <<<"
else
    echo ">>> CHAT TEST FAILED (exit code: $EXIT_CODE) <<<"
fi

exit $EXIT_CODE
