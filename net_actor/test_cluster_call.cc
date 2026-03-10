/**
 * @file test_cluster_call.cc
 * @brief 跨进程协程 RPC 测试 — co_await ClusterCall()
 *
 * 测试 CoroutineActor 通过 ClusterCall 向远端节点发起跨进程 RPC，
 * 协程挂起等待远端 RespondRemote() 回复后恢复。
 *
 * 架构（单进程双节点，通过 TCP 连接）：
 *
 *   NodeA（远端服务提供方）：
 *     - echo_service（普通 Actor）: 收到消息后 RespondRemote("echo:<data>")
 *     - math_service（普通 Actor）: 收到 "add:X:Y" → RespondRemote("result:<X+Y>")
 *
 *   NodeB（本地调用方）：
 *     - caller_service（CoroutineActor）: 使用 co_await ClusterCall() 调用 NodeA 的服务
 *     - local_service（普通 Actor）: 本地服务，用于测试混合 Call + ClusterCall
 *
 * 测试用例：
 *   Test1: 基础 ClusterCall — 调用 echo_service，验证响应内容
 *   Test2: 连续多次 ClusterCall — 同一协程中连续调用不同服务
 *   Test3: 混合 Call + ClusterCall — 同一协程中混合本地和远程调用
 *
 * 编译: make -f Makefile.clustercall
 * 运行: ./run_test_cluster_call.sh
 */

#include "precompiled.h"
#include "Actor.h"
#include "CoroutineActor.h"
#include "ActorSystem.h"
#include "EventLoop.h"
#include "ClusterProxy.h"
#include "Message.h"

#include <cassert>
#include <chrono>
#include <sstream>

using namespace bllsll;

// 测试端口（与其他测试不冲突）
static const int CLUSTER_CALL_PORT_A = 19800;
static const int CLUSTER_CALL_PORT_B = 19801;

// ============================================================
//  辅助函数
// ============================================================

static bool WaitFor(std::atomic<bool>& flag, int timeoutMs = 10000)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
        usleep(5000);
    }
    return flag.load();
}

static std::vector<std::string> Split(const std::string& s, char delim)
{
    std::vector<std::string> tokens;
    std::istringstream iss(s);
    std::string token;
    while (std::getline(iss, token, delim)) {
        tokens.push_back(token);
    }
    return tokens;
}

// ============================================================
//  NodeA 的服务 Actor（远端，普通 Actor）
// ============================================================

/**
 * EchoService: 收到消息后用 RespondRemote 回传 "echo:<data>"
 */
class EchoService : public Actor
{
public:
    std::atomic<int> recvCount{0};

    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        if (msg.type != MsgType::UserMessage) co_return;

        int cnt = recvCount.fetch_add(1) + 1;
        std::cout << "  [NodeA:echo_service] recv #" << cnt << ": " << msg.data << std::endl;

        if (msg.IsRemote()) {
            RespondRemote(msg, "echo:" + msg.data);
        }
        co_return;
    }
};

/**
 * MathService: 收到 "add:X:Y" → RespondRemote("result:<X+Y>")
 *              收到 "mul:X:Y" → RespondRemote("result:<X*Y>")
 */
class MathService : public Actor
{
public:
    std::atomic<int> recvCount{0};

    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        if (msg.type != MsgType::UserMessage) co_return;

        int cnt = recvCount.fetch_add(1) + 1;
        std::cout << "  [NodeA:math_service] recv #" << cnt << ": " << msg.data << std::endl;

        auto parts = Split(msg.data, ':');
        if (parts.size() >= 3 && msg.IsRemote()) {
            int a = std::stoi(parts[1]);
            int b = std::stoi(parts[2]);
            int result = 0;
            if (parts[0] == "add") {
                result = a + b;
            } else if (parts[0] == "mul") {
                result = a * b;
            }
            RespondRemote(msg, "result:" + std::to_string(result));
        }
        co_return;
    }
};

// ============================================================
//  NodeB 的 Actor（本地调用方）
// ============================================================

/**
 * LocalService: 普通 Actor，用于测试混合 Call + ClusterCall
 * 收到带 sessionId 的消息时回复（模拟本地数据库查询）
 */
class LocalService : public Actor
{
public:
    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        if (msg.type != MsgType::UserMessage) co_return;

        std::cout << "  [NodeB:local_service] recv: " << msg.data << std::endl;

        // 如果是 Call 请求（有 sessionId），回复
        if (msg.sessionId > 0 && !msg.isResponse) {
            ActorMessage response{MsgType::UserMessage, 0, -1, "local_result:" + msg.data};
            RespondToCall(msg, std::move(response));
        }
        co_return;
    }
};

/**
 * CallerActor: CoroutineActor，使用 co_await ClusterCall() 进行跨进程 RPC
 *
 * 通过外部触发消息来启动不同的测试场景：
 *   "test_echo"     → Test1: 基础 ClusterCall
 *   "test_multi"    → Test2: 连续多次 ClusterCall
 *   "test_mixed"    → Test3: 混合 Call + ClusterCall
 */
class CallerActor : public CoroutineActor
{
public:
    uint32_t localServiceId = 0;  // 本地服务 Actor ID（Test3 用）

    // 测试结果存储
    std::atomic<bool> test1Done{false};
    std::atomic<bool> test1Ok{false};
    std::string test1Result;

    std::atomic<bool> test2Done{false};
    std::atomic<bool> test2Ok{false};
    std::string test2EchoResult;
    std::string test2MathResult;

    std::atomic<bool> test3Done{false};
    std::atomic<bool> test3Ok{false};
    std::string test3LocalResult;
    std::string test3RemoteResult;

    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        if (msg.type != MsgType::UserMessage) co_return;

        if (msg.data == "test_echo") {
            // ============================================================
            //  Test1: 基础 ClusterCall — 调用远端 echo_service
            // ============================================================
            std::cout << "  [CallerActor] Test1: co_await ClusterCall to echo_service..." << std::endl;

            auto resp = co_await ClusterCall("nodeA", "echo_service",
                ActorMessage{MsgType::UserMessage, 0, -1, "hello_from_coroutine"});

            test1Result = resp.data;
            test1Ok.store(resp.data == "echo:hello_from_coroutine");
            test1Done.store(true);

            std::cout << "  [CallerActor] Test1: got response: " << resp.data << std::endl;

        } else if (msg.data == "test_multi") {
            // ============================================================
            //  Test2: 连续多次 ClusterCall — 同一协程调用不同远端服务
            // ============================================================
            std::cout << "  [CallerActor] Test2: sequential ClusterCalls..." << std::endl;

            // 第一次调用 echo_service
            auto echoResp = co_await ClusterCall("nodeA", "echo_service",
                ActorMessage{MsgType::UserMessage, 0, -1, "sequential_test"});
            test2EchoResult = echoResp.data;
            std::cout << "  [CallerActor] Test2: echo_service replied: " << echoResp.data << std::endl;

            // 第二次调用 math_service（在同一协程中！）
            auto mathResp = co_await ClusterCall("nodeA", "math_service",
                ActorMessage{MsgType::UserMessage, 0, -1, "add:17:25"});
            test2MathResult = mathResp.data;
            std::cout << "  [CallerActor] Test2: math_service replied: " << mathResp.data << std::endl;

            test2Ok.store(echoResp.data == "echo:sequential_test" &&
                          mathResp.data == "result:42");
            test2Done.store(true);

        } else if (msg.data == "test_mixed") {
            // ============================================================
            //  Test3: 混合 Call + ClusterCall — 本地和远程协程调用混用
            // ============================================================
            std::cout << "  [CallerActor] Test3: mixed local Call + remote ClusterCall..." << std::endl;

            // 先进行本地 Call
            auto localResp = co_await Call(localServiceId,
                ActorMessage{MsgType::UserMessage, 0, -1, "local_query"});
            test3LocalResult = localResp.data;
            std::cout << "  [CallerActor] Test3: local_service replied: " << localResp.data << std::endl;

            // 再进行远程 ClusterCall（在同一协程中！）
            auto remoteResp = co_await ClusterCall("nodeA", "math_service",
                ActorMessage{MsgType::UserMessage, 0, -1, "mul:6:7"});
            test3RemoteResult = remoteResp.data;
            std::cout << "  [CallerActor] Test3: math_service replied: " << remoteResp.data << std::endl;

            test3Ok.store(localResp.data == "local_result:local_query" &&
                          remoteResp.data == "result:42");
            test3Done.store(true);
        }

        co_return;
    }
};

// ============================================================
//  测试主函数
// ============================================================

int main()
{
    std::cout << "=================================================" << std::endl;
    std::cout << "  ClusterCall Test: Cross-Process Coroutine RPC" << std::endl;
    std::cout << "=================================================" << std::endl;

    // -------------------------------------------------------
    //  1. 启动 NodeA（远端服务提供方）
    // -------------------------------------------------------
    std::cout << "\n[Setup] Starting NodeA (service provider)..." << std::endl;

    EventLoop loopA;
    loopA.Create();
    ActorSystem sysA;
    sysA.Start(4, &loopA);
    loopA.SetActorSystem(&sysA);

    TcpClusterTransport transportA(&sysA, &loopA);
    transportA.SetLocalNodeId("nodeA");
    transportA.Listen(CLUSTER_CALL_PORT_A);
    sysA.RegisterTransport(&transportA);

    // 注册远端服务 Actor
    auto echoPtr = std::make_unique<EchoService>();
    EchoService* echo = echoPtr.get();
    uint32_t echoId = sysA.RegisterActor(std::move(echoPtr));
    sysA.RegisterName("echo_service", echoId);

    auto mathPtr = std::make_unique<MathService>();
    MathService* math = mathPtr.get();
    uint32_t mathId = sysA.RegisterActor(std::move(mathPtr));
    sysA.RegisterName("math_service", mathId);

    std::cout << "  NodeA: echo_service(id=" << echoId << "), math_service(id=" << mathId << ")" << std::endl;

    usleep(100000);  // 100ms 等待监听就绪

    // -------------------------------------------------------
    //  2. 启动 NodeB（本地调用方）
    // -------------------------------------------------------
    std::cout << "[Setup] Starting NodeB (coroutine caller)..." << std::endl;

    EventLoop loopB;
    loopB.Create();
    ActorSystem sysB;
    sysB.Start(4, &loopB);
    loopB.SetActorSystem(&sysB);

    TcpClusterTransport transportB(&sysB, &loopB);
    transportB.SetLocalNodeId("nodeB");
    transportB.Listen(CLUSTER_CALL_PORT_B);
    sysB.RegisterTransport(&transportB);

    // NodeB 连接到 NodeA
    transportB.ConnectToNode("nodeA", "127.0.0.1", CLUSTER_CALL_PORT_A);

    // 等待握手
    std::cout << "  Waiting for TCP handshake..." << std::endl;
    usleep(500000);  // 500ms

    // NodeA 反向连接 NodeB（用于 NodeA → NodeB 的回程路由）
    transportA.ConnectToNode("nodeB", "127.0.0.1", CLUSTER_CALL_PORT_B);
    usleep(500000);  // 500ms

    // 验证连接
    int fdAtoB = transportB.GetFdByNodeId("nodeA");
    if (fdAtoB < 0) {
        std::cerr << "[FAIL] NodeB cannot connect to NodeA! Handshake failed." << std::endl;
        sysA.Stop();
        sysB.Stop();
        return 1;
    }
    std::cout << "  TCP connection established. B->A fd=" << fdAtoB << std::endl;

    // 注册 NodeB 的 Actor
    auto localSvcPtr = std::make_unique<LocalService>();
    uint32_t localSvcId = sysB.RegisterActor(std::move(localSvcPtr));

    auto callerPtr = std::make_unique<CallerActor>();
    CallerActor* caller = callerPtr.get();
    caller->localServiceId = localSvcId;
    uint32_t callerId = sysB.RegisterActor(std::move(callerPtr));
    sysB.RegisterName("caller_service", callerId);  // 必须注册名字！ClusterCall 需要回程路由

    std::cout << "  NodeB: caller_service(id=" << callerId
              << "), local_service(id=" << localSvcId << ")" << std::endl;

    usleep(100000);  // 等待一切就绪

    // -------------------------------------------------------
    //  3. Test1: 基础 ClusterCall
    // -------------------------------------------------------
    std::cout << "\n========== Test1: Basic ClusterCall ==========" << std::endl;
    std::cout << "  Sending trigger to CallerActor..." << std::endl;

    sysB.Send(callerId, ActorMessage{MsgType::UserMessage, 0, -1, "test_echo"});

    bool test1Done = WaitFor(caller->test1Done, 10000);

    bool test1Pass = test1Done && caller->test1Ok.load();
    if (test1Pass) {
        std::cout << "  [PASS] Test1: ClusterCall echo response = \"" << caller->test1Result << "\"" << std::endl;
    } else {
        std::cout << "  [FAIL] Test1: " << (test1Done ? ("wrong response: \"" + caller->test1Result + "\"")
                                                       : "timeout!") << std::endl;
    }

    // -------------------------------------------------------
    //  4. Test2: 连续多次 ClusterCall
    // -------------------------------------------------------
    std::cout << "\n========== Test2: Sequential ClusterCalls ==========" << std::endl;
    std::cout << "  Sending trigger to CallerActor..." << std::endl;

    sysB.Send(callerId, ActorMessage{MsgType::UserMessage, 0, -1, "test_multi"});

    bool test2Done = WaitFor(caller->test2Done, 10000);

    bool test2Pass = test2Done && caller->test2Ok.load();
    if (test2Pass) {
        std::cout << "  [PASS] Test2: echo=\"" << caller->test2EchoResult
                  << "\", math=\"" << caller->test2MathResult << "\"" << std::endl;
    } else {
        std::cout << "  [FAIL] Test2: " << (test2Done
            ? ("echo=\"" + caller->test2EchoResult + "\", math=\"" + caller->test2MathResult + "\"")
            : "timeout!") << std::endl;
    }

    // -------------------------------------------------------
    //  5. Test3: 混合 Call + ClusterCall
    // -------------------------------------------------------
    std::cout << "\n========== Test3: Mixed Call + ClusterCall ==========" << std::endl;
    std::cout << "  Sending trigger to CallerActor..." << std::endl;

    sysB.Send(callerId, ActorMessage{MsgType::UserMessage, 0, -1, "test_mixed"});

    bool test3Done = WaitFor(caller->test3Done, 10000);

    bool test3Pass = test3Done && caller->test3Ok.load();
    if (test3Pass) {
        std::cout << "  [PASS] Test3: local=\"" << caller->test3LocalResult
                  << "\", remote=\"" << caller->test3RemoteResult << "\"" << std::endl;
    } else {
        std::cout << "  [FAIL] Test3: " << (test3Done
            ? ("local=\"" + caller->test3LocalResult + "\", remote=\"" + caller->test3RemoteResult + "\"")
            : "timeout!") << std::endl;
    }

    // -------------------------------------------------------
    //  6. 汇总结果
    // -------------------------------------------------------
    std::cout << "\n=================================================" << std::endl;
    std::cout << "  NodeA stats: echo_service recv=" << echo->recvCount.load()
              << ", math_service recv=" << math->recvCount.load() << std::endl;

    int passed = 0, failed = 0;
    if (test1Pass) ++passed; else ++failed;
    if (test2Pass) ++passed; else ++failed;
    if (test3Pass) ++passed; else ++failed;

    std::cout << "\n  Result: " << passed << " passed, " << failed << " failed" << std::endl;

    if (failed == 0) {
        std::cout << "\n  [PASS] All ClusterCall tests passed!" << std::endl;
        std::cout << std::endl;
        std::cout << "  Verified:" << std::endl;
        std::cout << "    1. co_await ClusterCall() suspends coroutine" << std::endl;
        std::cout << "    2. Remote RespondRemote() routes back via TCP" << std::endl;
        std::cout << "    3. Coroutine resumes with correct response data" << std::endl;
        std::cout << "    4. Sequential ClusterCalls work in same coroutine" << std::endl;
        std::cout << "    5. Mixed local Call + remote ClusterCall in same coroutine" << std::endl;
    } else {
        std::cout << "\n  [FAIL] Some tests failed!" << std::endl;
    }
    std::cout << "=================================================" << std::endl;

    // -------------------------------------------------------
    //  7. 清理
    // -------------------------------------------------------
    sysB.Stop();
    sysA.Stop();

    return (failed > 0) ? 1 : 0;
}
