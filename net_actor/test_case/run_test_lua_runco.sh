#!/bin/bash
# ============================================================
#  LuaActor actor.run_co 嵌套协程 propagate 测试
#  用法: bash test_case/run_test_lua_runco.sh
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo "  Building LuaActor run_co Test..."
echo "  (C++ coroutine ↔ Lua nested coroutine)"
echo "============================================"

make -f Makefile.lua_runco clean 2>/dev/null || true
make -f Makefile.lua_runco -j$(nproc)

echo ""
echo "============================================"
echo "  Running LuaActor run_co Test..."
echo "============================================"
echo ""

export LD_LIBRARY_PATH="$SCRIPT_DIR/../../lib:${LD_LIBRARY_PATH}"
./test_actor_lua_runco
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo ">>> RUN_CO TEST PASSED <<<"
else
    echo ">>> RUN_CO TEST FAILED (exit code: $EXIT_CODE) <<<"
fi

exit $EXIT_CODE
