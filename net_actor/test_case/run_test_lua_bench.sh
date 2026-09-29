#!/bin/bash
# ============================================================
#  LuaActor Phase 4 性能基准 - 构建 & 运行脚本
#  对比 Lua actor.call 与 C++ co_await Call 的延迟 / 吞吐
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ACTOR_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ACTOR_DIR"

echo "============================================"
echo "  Building LuaActor Phase 4 Benchmark..."
echo "  (Lua actor.call vs C++ co_await Call)"
echo "============================================"

make -f Makefile.lua_bench clean 2>/dev/null || true
make -f Makefile.lua_bench -j$(nproc)

echo ""
echo "============================================"
echo "  Running LuaActor Phase 4 Benchmark..."
echo "============================================"
echo ""

export LD_LIBRARY_PATH="$SCRIPT_DIR/../../lib:${LD_LIBRARY_PATH}"
./test_actor_lua_bench
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo ">>> BENCHMARK PASSED <<<"
else
    echo ">>> BENCHMARK FAILED (exit code: $EXIT_CODE) <<<"
fi

exit $EXIT_CODE
