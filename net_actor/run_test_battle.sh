#!/bin/bash
# ============================================================
#  Actor 战斗测试 - 构建 & 运行脚本
#  演示: BattleActor 中介者模式解决多线程战斗问题
#
#  用法: chmod +x run_test_battle.sh && ./run_test_battle.sh
#
#  注意: 如果遇到 '\r' 错误，先执行:
#    sed -i 's/\r$//' run_test_battle.sh Makefile.battle test_actor_battle.cc
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo "  Building Actor Battle Test..."
echo "============================================"

make -f Makefile.battle clean 2>/dev/null || true
make -f Makefile.battle -j$(nproc)

echo ""
echo "============================================"
echo "  Running Actor Battle Test..."
echo "  Warrior (ATK=15) VS Mage (ATK=20)"
echo "  BattleActor serializes all combat logic"
echo "============================================"
echo ""

./test_actor_battle
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo ">>> BATTLE TEST PASSED <<<"
else
    echo ">>> BATTLE TEST FAILED (exit code: $EXIT_CODE) <<<"
fi

exit $EXIT_CODE
