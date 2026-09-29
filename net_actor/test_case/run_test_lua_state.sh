#!/bin/bash
# ============================================================
#  LuaActor Phase 2 状态服务测试 - 构建 & 运行脚本
#  验证 C++ co_await Call ↔ LuaActor actor.respond 桥
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ACTOR_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ACTOR_DIR"

echo "============================================"
echo "  Building LuaActor Phase 2 State Service Test..."
echo "  (C++ co_await Call ↔ LuaActor actor.respond)"
echo "============================================"

make -f Makefile.lua_state clean 2>/dev/null || true
make -f Makefile.lua_state -j$(nproc)

echo ""
echo "============================================"
echo "  Running LuaActor Phase 2 State Service Test..."
echo "============================================"
echo ""

export LD_LIBRARY_PATH="$SCRIPT_DIR/../../lib:${LD_LIBRARY_PATH}"
./test_actor_lua_state
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo ">>> PHASE 2 PASSED <<<"
else
    echo ">>> PHASE 2 FAILED (exit code: $EXIT_CODE) <<<"
fi

exit $EXIT_CODE
