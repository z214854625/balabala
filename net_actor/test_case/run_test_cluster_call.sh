#!/bin/bash
# ============================================================
#  跨进程协程 RPC 测试 — co_await ClusterCall()
#
#  测试内容：
#    1. CoroutineActor 使用 co_await ClusterCall() 调用远端服务
#    2. 协程挂起，远端 RespondRemote() 回复后协程恢复
#    3. 验证连续 ClusterCall、混合 Call + ClusterCall
#
#  用法: bash test_case/run_test_cluster_call.sh
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo "  Building ClusterCall Coroutine RPC Test..."
echo "============================================"

make -f Makefile.clustercall clean 2>/dev/null || true
make -f Makefile.clustercall -j$(nproc)

echo ""
echo "============================================"
echo "  Running ClusterCall Coroutine RPC Test"
echo "  (single process, dual-node TCP)"
echo "============================================"
echo ""

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
