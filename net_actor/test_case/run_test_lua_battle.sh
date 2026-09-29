#!/bin/bash
# ============================================================
#  跨服战斗业务示例 - A 服战斗发起前到 B 服异步检查并扣道具
#  用法: bash test_case/run_test_lua_battle.sh
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo "  Building Cross-Server Battle Example..."
echo "============================================"

make -f Makefile.lua_battle clean 2>/dev/null || true
make -f Makefile.lua_battle -j$(nproc)

echo ""
echo "============================================"
echo "  Running Cross-Server Battle Example..."
echo "  (A.battle_service → B.item_service → B.db_service)"
echo "============================================"
echo ""

export LD_LIBRARY_PATH="$SCRIPT_DIR/../../lib:${LD_LIBRARY_PATH}"
./test_actor_lua_battle
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo ">>> BATTLE EXAMPLE PASSED <<<"
else
    echo ">>> BATTLE EXAMPLE FAILED (exit code: $EXIT_CODE) <<<"
fi

exit $EXIT_CODE
