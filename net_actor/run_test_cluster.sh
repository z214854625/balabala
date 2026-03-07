#!/bin/bash
# ============================================================
#  跨进程集群通信测试 - 构建 & 运行脚本
#
#  测试内容：
#    1. 启动 NodeA（服务端进程），监听 19700 端口
#    2. 启动 NodeB（客户端进程），连接 NodeA
#    3. NodeB 通过 ClusterProxy -> TcpClusterTransport 向 NodeA 发送消息
#    4. NodeA 的 echo_service / counter_service 处理后回传响应
#    5. NodeB 验证收到的响应，输出测试结果
#
#  用法:
#    chmod +x run_test_cluster.sh && ./run_test_cluster.sh
#
#  注意: 如果遇到 '\r' 错误，先执行:
#    sed -i 's/\r$//' run_test_cluster.sh Makefile.cluster test_cluster_node.cc
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo "  Building Cross-Process Cluster Test..."
echo "============================================"

# 清理旧产物
make -f Makefile.cluster clean 2>/dev/null || true

# 构建
make -f Makefile.cluster -j$(nproc)

echo ""
echo "============================================"
echo "  Running Cross-Process Cluster Test"
echo "  NodeA (server) + NodeB (client)"
echo "============================================"
echo ""

# 启动 NodeA（服务端）在后台运行
echo ">>> Starting NodeA (server)..."
./test_cluster_node -a &
NODEA_PID=$!
echo ">>> NodeA PID: $NODEA_PID"

# 等待 NodeA 启动完成（监听端口就绪）
sleep 1

# 检查 NodeA 是否还在运行
if ! kill -0 $NODEA_PID 2>/dev/null; then
    echo ">>> ERROR: NodeA exited prematurely!"
    exit 1
fi

echo ""
echo ">>> Starting NodeB (client)..."
echo ""

# 启动 NodeB（客户端），前台运行，获取退出码
./test_cluster_node -b
NODEB_EXIT=$?

echo ""
echo ">>> NodeB exited with code: $NODEB_EXIT"

# 停止 NodeA
echo ">>> Stopping NodeA (PID=$NODEA_PID)..."
if kill -0 $NODEA_PID 2>/dev/null; then
    kill -TERM $NODEA_PID
    # 等待 NodeA 优雅退出（最多 3 秒）
    for i in $(seq 1 30); do
        if ! kill -0 $NODEA_PID 2>/dev/null; then
            break
        fi
        sleep 0.1
    done
    # 如果还没退出，强制杀掉
    if kill -0 $NODEA_PID 2>/dev/null; then
        echo ">>> NodeA did not exit gracefully, killing..."
        kill -9 $NODEA_PID 2>/dev/null || true
    fi
fi

# 等待 NodeA 进程结束
wait $NODEA_PID 2>/dev/null || true

echo ""
echo "============================================"
if [ $NODEB_EXIT -eq 0 ]; then
    echo "  >>> CROSS-PROCESS CLUSTER TEST PASSED <<<"
else
    echo "  >>> CROSS-PROCESS CLUSTER TEST FAILED <<<"
    echo "  (NodeB exit code: $NODEB_EXIT)"
fi
echo "============================================"

exit $NODEB_EXIT
