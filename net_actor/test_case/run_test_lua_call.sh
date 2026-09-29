#!/bin/bash
# ============================================================
#  LuaActor Phase 3 协程融合测试 - 构建 & 运行脚本
#  验证 Lua coroutine.yield ↔ C++ co_await Call 桥
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ACTOR_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ACTOR_DIR"

echo "============================================"
echo "  Building LuaActor Phase 3 actor.call Yield Test..."
echo "  (Lua coroutine.yield ↔ C++ co_await Call)"
echo "============================================"

make -f Makefile.lua_call clean 2>/dev/null || true
make -f Makefile.lua_call -j$(nproc)

echo ""
echo "============================================"
echo "  Running LuaActor Phase 3 actor.call Yield Test..."
echo "============================================"
echo ""

export LD_LIBRARY_PATH="$SCRIPT_DIR/../../lib:${LD_LIBRARY_PATH}"
./test_actor_lua_call
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo ">>> PHASE 3 PASSED <<<"
else
    echo ">>> PHASE 3 FAILED (exit code: $EXIT_CODE) <<<"
fi

exit $EXIT_CODE
