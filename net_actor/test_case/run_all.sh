#!/bin/bash
# ============================================================
#  一键构建并运行全部测试用例（不含 multi_reactor_bench，太慢）
#  用法: bash test_case/run_all.sh
# ============================================================

# 注意：不用 set -e，单测失败继续跑后面的
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
export LD_LIBRARY_PATH="$SCRIPT_DIR/../../lib:${LD_LIBRARY_PATH}"

# 测试脚本列表（顺序：先框架自带、再 Lua 集成）
declare -a SCRIPTS=(
    "run_test.sh"                # Actor 间消息（test_actor_msg）
    "run_test_no_eventloop.sh"   # 无 EventLoop 纯 Actor 消息
    "run_test_chat.sh"           # MMO 聊天广播
    "run_test_battle.sh"         # 战斗 Actor 中介者
    "run_test_advanced.sh"       # P2/P3 高级特性
    "run_test_coroutine.sh"      # C++20 协程
    "run_test_cluster.sh"        # 跨进程集群（双节点 TCP）
    "run_test_cluster_call.sh"   # 跨进程协程 RPC
    "run_test_multi_reactor.sh"  # 主从 Reactor
    "run_test_lua.sh"            # Phase 1: Lua 同步 cmsgpack
    "run_test_lua_state.sh"      # Phase 2: LuaActor 状态服务
    "run_test_lua_call.sh"       # Phase 3: Lua actor.call yield
    "run_test_lua_runco.sh"      # 嵌套协程 propagate（actor.run_co）
    "run_test_lua_cluster.sh"    # Phase 4: Lua actor.cluster_call
    "run_test_lua_battle.sh"     # 跨服战斗业务示例（A 服战斗 + B 服道具 + DB）
    "run_test_lua_bench.sh"      # Phase 4: Lua vs C++ benchmark
    # "run_test_multi_reactor_bench.sh"  # 跳过：-O2 编译慢、跑 1-2 分钟，按需手动
)

PASS=0
FAIL=0
FAILED=()

for s in "${SCRIPTS[@]}"; do
    echo ""
    echo "############################################"
    echo "  Running: $s"
    echo "############################################"
    if bash "$SCRIPT_DIR/$s" 2>&1 | tail -40; then
        rc=${PIPESTATUS[0]}
    else
        rc=$?
    fi
    if [ "$rc" -eq 0 ]; then
        echo "  [PASS] $s"
        PASS=$((PASS+1))
    else
        echo "  [FAIL] $s (exit=$rc)"
        FAIL=$((FAIL+1))
        FAILED+=("$s")
    fi
done

echo ""
echo "============================================"
echo "  Summary: $PASS passed, $FAIL failed"
if [ $FAIL -gt 0 ]; then
    echo "  Failed: ${FAILED[*]}"
fi
echo "============================================"
exit $FAIL
