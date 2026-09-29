#!/bin/bash
# ============================================================
#  C++20 协程 Actor 测试 - 构建 & 运行脚本
#  演示: Skynet 风格的协程服务调用
#    co_await Call()  — 类似 skynet.call()
#    co_await Sleep() — 类似 skynet.sleep()
#    Respond()        — 类似 skynet.ret()
#
#  用法: bash test_case/run_test_coroutine.sh
#  要求: GCC 11+ (C++20 coroutine 支持)
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo "  Building C++20 Coroutine Actor Test..."
echo "  (Skynet-style service coroutines)"
echo "============================================"

make -f Makefile.coroutine clean 2>/dev/null || true
make -f Makefile.coroutine -j$(nproc)

echo ""
echo "============================================"
echo "  Running Coroutine Actor Test..."
echo ""
echo "  Skynet API mapping:"
echo "    skynet.call()  <=> co_await Call()"
echo "    skynet.ret()   <=> Respond()"
echo "    skynet.sleep() <=> co_await Sleep()"
echo "    skynet.send()  <=> SendToActor()"
echo "============================================"
echo ""

./test_coroutine_actor
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo ">>> ALL COROUTINE TESTS PASSED <<<"
else
    echo ">>> SOME TESTS FAILED (exit code: $EXIT_CODE) <<<"
fi

exit $EXIT_CODE
