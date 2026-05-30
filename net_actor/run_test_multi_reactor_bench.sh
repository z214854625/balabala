#!/bin/bash
# ============================================================
#  多 Reactor 完整压测套件
#  跑 5 组对比测试，输出性能数据表
#
#  注意：用 -O2 编译，跑完约 1~2 分钟
# ============================================================

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "========================================"
echo "  Building Multi-Reactor Benchmark..."
echo "========================================"

make -f Makefile.multi_reactor_bench clean 2>/dev/null || true
make -f Makefile.multi_reactor_bench -j$(nproc)

# 调系统 backlog，避免高并发连接被限制
echo "Current /proc/sys/net/core/somaxconn:"
cat /proc/sys/net/core/somaxconn 2>/dev/null || true

echo ""
echo "========================================"
echo "  Running Benchmark Suite..."
echo "  (takes about 1-2 minutes)"
echo "========================================"
echo ""

./test_multi_reactor_bench
