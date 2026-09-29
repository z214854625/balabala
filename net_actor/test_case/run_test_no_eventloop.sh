#!/bin/bash
# ============================================================
#  无 EventLoop 版 Actor 消息测试 - 构建 & 运行脚本
#  演示: 不依赖 EventLoop/Acceptor/Connector 的纯 Actor 间消息通信
#        (PingActor/PongActor/ForwarderActor/CollectorActor/BroadcasterActor)
#
#  用法: bash test_case/run_test_no_eventloop.sh
#
#  [修复] 原脚本调用 Makefile.test 构建并运行 ./test_actor_msg，
#         并未实际测试 test_actor_msg_no_eventloop.cc。改为使用新增的
#         Makefile.no_eventloop 构建并运行 ./test_actor_msg_no_eventloop。
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo "  Building No-EventLoop Actor Test..."
echo "============================================"

make -f Makefile.no_eventloop clean 2>/dev/null || true
make -f Makefile.no_eventloop -j$(nproc)

echo ""
echo "============================================"
echo "  Running No-EventLoop Actor Test..."
echo "  (Ping/Pong/Forwarder/Collector/Broadcaster)"
echo "============================================"
echo ""

./test_actor_msg_no_eventloop
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo ">>> NO-EVENTLOOP TEST PASSED <<<"
else
    echo ">>> NO-EVENTLOOP TEST FAILED (exit code: $EXIT_CODE) <<<"
fi

exit $EXIT_CODE
