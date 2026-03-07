#!/bin/bash
# ============================================================
#  跨进程协程 RPC 测试 — co_await ClusterCall()
#
#  测试内容：
#    1. CoroutineActor 使用 co_await ClusterCall() 调用远端服务
#    2. 协程挂起，远端 RespondRemote() 回复后协程恢复
#    3. 验证连续 ClusterCall、混合 Call + ClusterCall
#
#  用法:
#    chmod +x run_test_cluster_call.sh && ./run_test_cluster_call.sh
#
#  注意: 如果遇到 '\r' 错误，先执行:
#    sed -i 's/\r$//' run_test_cluster_call.sh Makefile.clustercall
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo "  Building ClusterCall Coroutine RPC Test..."
echo "============================================"

# 清理旧产物
make -f Makefile.clustercall clean 2>/dev/null || true

# 构建
make -f Makefile.clustercall -j$(nproc)

echo ""
echo "============================================"
echo "  Running ClusterCall Coroutine RPC Test"
echo "  (single process, dual-node TCP)"
echo "============================================"
echo ""

# 运行测试
./test_cluster_call
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo ">>> CLUSTER CALL COROUTINE RPC TEST PASSED <<<"
else
    echo ">>> CLUSTER CALL COROUTINE RPC TEST FAILED <<<"
    echo "(exit code: $EXIT_CODE)"
fi

exit $EXIT_CODE
