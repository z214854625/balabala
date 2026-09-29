#!/bin/bash
# ============================================================
#  LuaActor Phase 1 同步版测试 - 构建 & 运行脚本
#  用法: bash test_case/run_test_lua.sh （从 net_actor 目录）
#        或 cd test_case && bash run_test_lua.sh
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"      # test_case/
ACTOR_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"        # net_actor/
cd "$ACTOR_DIR"

echo "============================================"
echo "  Building LuaActor Phase 1 Sync Test..."
echo "  (cmsgpack roundtrip, no suspension)"
echo "============================================"

make -f Makefile.lua_test clean 2>/dev/null || true
make -f Makefile.lua_test -j$(nproc)

echo ""
echo "============================================"
echo "  Running LuaActor Phase 1 Sync Test..."
echo "============================================"
echo ""

export LD_LIBRARY_PATH="$SCRIPT_DIR/../../lib:${LD_LIBRARY_PATH}"
./test_actor_lua
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo ">>> PHASE 1 PASSED <<<"
else
    echo ">>> PHASE 1 FAILED (exit code: $EXIT_CODE) <<<"
fi

exit $EXIT_CODE
