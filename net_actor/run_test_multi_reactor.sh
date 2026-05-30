#!/bin/bash
# ============================================================
#  主从 Reactor (Multi-Reactor) 测试 - 构建 & 运行脚本
#  用法: chmod +x run_test_multi_reactor.sh && ./run_test_multi_reactor.sh
#
#  这个测试验证：
#    1. SubReactor 池启动正常
#    2. 新连接被 Round-Robin 派发到不同 sub loop
#    3. fd → loop 映射正确（Actor::SendToNetwork 能找到对应 loop 发送）
#    4. 多客户端并发收发，无消息丢失
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo "  Building Multi-Reactor Test..."
echo "============================================"

# 清理旧产物
make -f Makefile.multi_reactor clean 2>/dev/null || true

# 构建测试
make -f Makefile.multi_reactor -j$(nproc)

echo ""
echo "============================================"
echo "  Running Multi-Reactor Test..."
echo "============================================"
echo ""

# 运行测试
./test_multi_reactor
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo ">>> ALL TESTS PASSED <<<"
else
    echo ">>> SOME TESTS FAILED (exit code: $EXIT_CODE) <<<"
fi

exit $EXIT_CODE
