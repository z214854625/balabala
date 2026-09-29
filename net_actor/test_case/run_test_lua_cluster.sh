#!/bin/bash
# ============================================================
#  LuaActor Phase 4 跨进程集群测试 - 构建 & 运行脚本
#  验证 Lua actor.cluster_call ↔ C++ co_await ClusterCall
#  经 LoopbackTransport 双节点 RPC
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ACTOR_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ACTOR_DIR"

echo "============================================"
echo "  Building LuaActor Phase 4 Cluster Call Test..."
echo "  (Lua cluster_call ↔ C++ co_await ClusterCall)"
echo "============================================"

make -f Makefile.lua_cluster clean 2>/dev/null || true
make -f Makefile.lua_cluster -j$(nproc)

echo ""
echo "============================================"
echo "  Running LuaActor Phase 4 Cluster Call Test..."
echo "============================================"
echo ""

export LD_LIBRARY_PATH="$SCRIPT_DIR/../../lib:${LD_LIBRARY_PATH}"
./test_actor_lua_cluster
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo ">>> PHASE 4 PASSED <<<"
else
    echo ">>> PHASE 4 FAILED (exit code: $EXIT_CODE) <<<"
fi

exit $EXIT_CODE
