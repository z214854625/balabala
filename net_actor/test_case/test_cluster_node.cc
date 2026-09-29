/**
 * @file test_cluster_node.cc
 * @brief 跨进程集群通信测试 — 两个独立进程通过 TCP 通信（name-based routing v2）
 *
 * 用法:
 *   ./test_cluster_node -a          # 启动 NodeA（服务端，监听 19700）
 *   ./test_cluster_node -b          # 启动 NodeB（客户端，连接 NodeA）
 *
 * 测试场景:
 *   NodeA（服务端进程）：
 *     - TcpClusterTransport 监听 19700 端口
 *     - "echo_service"   Actor：收到远程消息后，用 RespondRemote() 回传 "echo:<data>"
 *     - "counter_service" Actor：计数收到的消息，"get_count" 时用 RespondRemote() 回传计数
 *
 *   NodeB（客户端进程）：
 *     - TcpClusterTransport 连接 NodeA (127.0.0.1:19700)
 *     - "result_collector" Actor：收集来自 NodeA 的响应（通过 sourceNodeId + sourceActorName 路由回来）
 *     - 方式一：通过 ClusterProxy 向 NodeA 的 echo_service 发送消息
 *     - 方式二：通过 ActorSystem::SendToRemote() 便捷方法发送
 *     - 验证收到的响应数量和内容
 *
 * 数据流（name-based routing）:
 *   NodeB                                       NodeA
 *   ClusterProxy ─────TCP────> GatewayActor ──(SendByName)──> echo_service
 *   (sourceActorName="result_collector")                          │
 *                                                                 │ RespondRemote()
 *   result_collector <──(SendByName)── GatewayActor <───TCP──────┘
 *   (msg.sourceNodeId="nodeA", msg.sourceActorName="echo_service")
 *
 * 编译: make -f Makefile.cluster
 * 运行: ./run_test_cluster.sh
 */

#include "precompiled.h"
#include "Actor.h"
#include "ActorSystem.h"
#include "EventLoop.h"
#include "ClusterProxy.h"
#include "Message.h"

#include <cassert>
#include <chrono>
#include <csignal>

using namespace bllsll;

static const int CLUSTER_PORT = 19700;
static const int ECHO_MSG_COUNT = 10;
static const int COUNTER_MSG_COUNT = 5;

// ============================================================
//  全局退出标志（NodeA 用 SIGTERM 优雅退出）
// ============================================================
static std::atomic<bool> g_running{true};

static void SignalHandler(int sig)
{
    (void)sig;
    g_running.store(false);
}

// ============================================================
//  NodeA 的 Actor 定义
// ============================================================

/**
 * EchoServiceActor（NodeA 上运行）
 * - 收到远程消息后，用 RespondRemote() 将 "echo:<data>" 自动回传给发送方
 * - RespondRemote() 内部根据 msg.sourceNodeId + msg.sourceActorName 自动路由
 * - 不再需要持有 TcpClusterTransport* 或手动构建 ClusterPacket
 */
class EchoServiceActor : public Actor
{
public:
    std::atomic<int> recvCount{0};

    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        if (msg.type != MsgType::UserMessage) co_return;

        int cnt = recvCount.fetch_add(1) + 1;
        std::cout << "[NodeA:echo_service] recv #" << cnt << ": " << msg.data
                  << " (from " << msg.sourceNodeId << "::" << msg.sourceActorName << ")" << std::endl;

        // 使用 RespondRemote() 自动回复给来源 Actor
        // 内部会根据 msg.sourceNodeId + msg.sourceActorName 路由回去
        if (msg.IsRemote()) {
            RespondRemote(msg, "echo:" + msg.data);
        }
        co_return;
    }
};

/**
 * CounterServiceActor（NodeA 上运行）
 * - 收到 "get_count" 时，用 RespondRemote() 将计数值回传给发送方
 * - 收到其他消息时，计数 +1
 * - 不再需要持有 TcpClusterTransport* 或手动构建 ClusterPacket
 */
class CounterServiceActor : public Actor
{
public:
    std::atomic<int> counter{0};

    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        if (msg.type != MsgType::UserMessage) co_return;

        if (msg.data == "get_count") {
            std::cout << "[NodeA:counter_service] get_count request, count="
                      << counter.load()
                      << " (from " << msg.sourceNodeId << "::" << msg.sourceActorName << ")" << std::endl;
            if (msg.IsRemote()) {
                RespondRemote(msg, "count:" + std::to_string(counter.load()));
            }
        } else {
            int cnt = counter.fetch_add(1) + 1;
            std::cout << "[NodeA:counter_service] recv #" << cnt << ": " << msg.data << std::endl;
        }
        co_return;
    }
};

// ============================================================
//  NodeB 的 Actor 定义
// ============================================================

/**
 * ResultCollectorActor（NodeB 上运行）
 * - 收集来自 NodeA 的 echo 响应和 counter 结果
 */
class ResultCollectorActor : public Actor
{
public:
    std::atomic<int> echoCount{0};
    std::atomic<int> countResultRecv{0};
    std::atomic<int> lastCount{-1};
    bllsll::SpinLockQueue<std::string> responses;

    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        if (msg.type != MsgType::UserMessage) co_return;

        std::cout << "[NodeB:result_collector] recv: " << msg.data << std::endl;
        responses.push(msg.data);

        if (msg.data.find("echo:") == 0) {
            echoCount.fetch_add(1);
        } else if (msg.data.find("count:") == 0) {
            countResultRecv.fetch_add(1);
            lastCount.store(std::stoi(msg.data.substr(6)));
        }
        co_return;
    }

    bool WaitForEchos(int expected, int timeoutMs = 10000)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (echoCount.load() < expected && std::chrono::steady_clock::now() < deadline) {
            usleep(10000);
        }
        return echoCount.load() >= expected;
    }

    bool WaitForCountResult(int timeoutMs = 5000)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (countResultRecv.load() < 1 && std::chrono::steady_clock::now() < deadline) {
            usleep(10000);
        }
        return countResultRecv.load() >= 1;
    }
};

// ============================================================
//  NodeA 主函数（服务端）
// ============================================================

int RunNodeA()
{
    std::cout << "=================================================" << std::endl;
    std::cout << "  NodeA (Server) - Cluster Test" << std::endl;
    std::cout << "  Listening on port " << CLUSTER_PORT << std::endl;
    std::cout << "=================================================" << std::endl;

    // 设置信号处理
    signal(SIGTERM, SignalHandler);
    signal(SIGINT, SignalHandler);

    // 1. 启动 EventLoop + ActorSystem
    EventLoop loop;
    loop.Create();

    ActorSystem sys;
    sys.Start(4, &loop);
    loop.SetActorSystem(&sys);

    // 2. 创建 TcpClusterTransport 并注册到 ActorSystem
    TcpClusterTransport transport(&sys, &loop);
    transport.SetLocalNodeId("nodeA");
    transport.Listen(CLUSTER_PORT);
    sys.RegisterTransport(&transport);  // 注册 transport，使 SendToRemote/RespondRemote 可用

    // 3. 注册服务 Actor（不再需要传入 transport 指针）
    auto echoPtr = std::make_unique<EchoServiceActor>();
    EchoServiceActor* echo = echoPtr.get();
    uint32_t echoId = sys.RegisterActor(std::move(echoPtr));
    sys.RegisterName("echo_service", echoId);

    auto counterPtr = std::make_unique<CounterServiceActor>();
    CounterServiceActor* counter = counterPtr.get();
    uint32_t counterId = sys.RegisterActor(std::move(counterPtr));
    sys.RegisterName("counter_service", counterId);

    std::cout << "[NodeA] echo_service(id=" << echoId << "), counter_service(id="
              << counterId << ") registered." << std::endl;
    std::cout << "[NodeA] Waiting for connections from NodeB..." << std::endl;

    // 4. 运行直到收到信号
    while (g_running.load()) {
        usleep(100000);  // 100ms
    }

    // 5. 输出统计
    std::cout << "\n[NodeA] Shutting down..." << std::endl;
    std::cout << "[NodeA] echo_service received: " << echo->recvCount.load() << " messages" << std::endl;
    std::cout << "[NodeA] counter_service received: " << counter->counter.load() << " messages" << std::endl;

    sys.Stop();
    std::cout << "[NodeA] Done." << std::endl;

    return 0;
}

// ============================================================
//  NodeB 主函数（客户端）
// ============================================================

int RunNodeB()
{
    std::cout << "=================================================" << std::endl;
    std::cout << "  NodeB (Client) - Cluster Test" << std::endl;
    std::cout << "  Connecting to NodeA at 127.0.0.1:" << CLUSTER_PORT << std::endl;
    std::cout << "=================================================" << std::endl;

    // 1. 启动 EventLoop + ActorSystem
    EventLoop loop;
    loop.Create();

    ActorSystem sys;
    sys.Start(4, &loop);
    loop.SetActorSystem(&sys);

    // 2. 创建 TcpClusterTransport 并注册到 ActorSystem
    TcpClusterTransport transport(&sys, &loop);
    transport.SetLocalNodeId("nodeB");
    transport.ConnectToNode("nodeA", "127.0.0.1", CLUSTER_PORT);
    sys.RegisterTransport(&transport);  // 注册 transport，使 SendToRemote 可用

    // 3. 等待握手完成
    std::cout << "[NodeB] Waiting for handshake with NodeA..." << std::endl;
    auto handshakeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (transport.GetFdByNodeId("nodeA") < 0 &&
           std::chrono::steady_clock::now() < handshakeDeadline) {
        usleep(50000);  // 50ms
    }

    if (transport.GetFdByNodeId("nodeA") < 0) {
        std::cerr << "[NodeB] FAIL: handshake timeout! Cannot connect to NodeA." << std::endl;
        sys.Stop();
        return 1;
    }
    std::cout << "[NodeB] Handshake completed! fd=" << transport.GetFdByNodeId("nodeA") << std::endl;

    // 4. 注册本地 Actor
    auto collectorPtr = std::make_unique<ResultCollectorActor>();
    ResultCollectorActor* collector = collectorPtr.get();
    uint32_t collectorId = sys.RegisterActor(std::move(collectorPtr));
    sys.RegisterName("result_collector", collectorId);

    // 5. 创建 ClusterProxy 指向 NodeA 的 echo_service
    RemoteActorRef echoRef("nodeA", "echo_service");
    auto echoProxyPtr = std::make_unique<ClusterProxy>(echoRef, &transport);
    uint32_t echoProxyId = sys.RegisterActor(std::move(echoProxyPtr));

    // 6. 创建 ClusterProxy 指向 NodeA 的 counter_service
    RemoteActorRef counterRef("nodeA", "counter_service");
    auto counterProxyPtr = std::make_unique<ClusterProxy>(counterRef, &transport);
    uint32_t counterProxyId = sys.RegisterActor(std::move(counterProxyPtr));

    std::cout << "[NodeB] result_collector(id=" << collectorId
              << "), echoProxy(id=" << echoProxyId
              << "), counterProxy(id=" << counterProxyId << ")" << std::endl;

    // 7. 发送消息到 NodeA 的 echo_service
    std::cout << "\n[NodeB] Sending " << ECHO_MSG_COUNT << " messages to echo_service..." << std::endl;
    for (int i = 0; i < ECHO_MSG_COUNT; ++i) {
        std::string data = "hello_from_nodeB_" + std::to_string(i);
        sys.Send(echoProxyId, ActorMessage{MsgType::UserMessage, collectorId, -1, data});
        usleep(20000);  // 20ms 间隔
    }

    // 8. 发送消息到 NodeA 的 counter_service
    std::cout << "[NodeB] Sending " << COUNTER_MSG_COUNT << " messages to counter_service..." << std::endl;
    for (int i = 0; i < COUNTER_MSG_COUNT; ++i) {
        std::string data = "count_msg_" + std::to_string(i);
        sys.Send(counterProxyId, ActorMessage{MsgType::UserMessage, collectorId, -1, data});
        usleep(20000);
    }

    // 9. 请求 counter_service 返回计数值
    usleep(500000);  // 等待 500ms 让消息处理完
    std::cout << "[NodeB] Requesting count from counter_service..." << std::endl;
    sys.Send(counterProxyId, ActorMessage{MsgType::UserMessage, collectorId, -1, "get_count"});

    // 10. 等待 echo 响应
    std::cout << "[NodeB] Waiting for echo responses..." << std::endl;
    collector->WaitForEchos(ECHO_MSG_COUNT, 10000);
    bool countOk = collector->WaitForCountResult(5000);

    // 11. 验证结果
    std::cout << "\n=================================================" << std::endl;
    std::cout << "  Verification Results" << std::endl;
    std::cout << "=================================================" << std::endl;

    bool ok = true;

    // 检查 echo 响应数量
    int echoRecv = collector->echoCount.load();
    std::cout << "  Echo responses: " << echoRecv << "/" << ECHO_MSG_COUNT;
    if (echoRecv >= ECHO_MSG_COUNT) {
        std::cout << " [OK]" << std::endl;
    } else {
        std::cout << " [FAIL]" << std::endl;
        ok = false;
    }

    // 检查 counter 结果
    int countResult = collector->lastCount.load();
    std::cout << "  Counter result: " << countResult << " (expected " << COUNTER_MSG_COUNT << ")";
    if (countOk && countResult == COUNTER_MSG_COUNT) {
        std::cout << " [OK]" << std::endl;
    } else if (countOk) {
        std::cout << " [FAIL: wrong count]" << std::endl;
        ok = false;
    } else {
        std::cout << " [FAIL: no response]" << std::endl;
        ok = false;
    }

    // 验证 echo 内容
    int verified = 0;
    auto resp = collector->responses.pop();
    while (resp) {
        if (resp->find("echo:hello_from_nodeB_") == 0) {
            verified++;
        }
        resp = collector->responses.pop();
    }
    std::cout << "  Echo content verified: " << verified << "/" << ECHO_MSG_COUNT;
    if (verified >= ECHO_MSG_COUNT) {
        std::cout << " [OK]" << std::endl;
    } else {
        std::cout << " [FAIL]" << std::endl;
        ok = false;
    }

    // 检查跨进程连通性
    auto knownNodes = transport.GetKnownNodes();
    bool nodeAlive = false;
    for (const auto& n : knownNodes) {
        if (n.nodeId == "nodeA" && n.alive) {
            nodeAlive = true;
        }
    }
    std::cout << "  NodeA alive: " << (nodeAlive ? "yes" : "no");
    if (nodeAlive) {
        std::cout << " [OK]" << std::endl;
    } else {
        std::cout << " [FAIL]" << std::endl;
        ok = false;
    }

    // 总结
    std::cout << "\n=================================================" << std::endl;
    if (ok) {
        std::cout << "  [PASS] Cross-process cluster test passed!" << std::endl;
        std::cout << std::endl;
        std::cout << "  Verified:" << std::endl;
        std::cout << "    1. TCP connection + handshake between 2 processes" << std::endl;
        std::cout << "    2. ClusterProxy -> TcpClusterTransport -> Remote Actor" << std::endl;
        std::cout << "    3. Echo: " << ECHO_MSG_COUNT << " messages sent and echoed back" << std::endl;
        std::cout << "    4. Counter: " << COUNTER_MSG_COUNT << " messages counted correctly" << std::endl;
        std::cout << "    5. Bidirectional: NodeB->NodeA (request) + NodeA->NodeB (response)" << std::endl;
    } else {
        std::cout << "  [FAIL] Some checks failed!" << std::endl;
    }
    std::cout << "=================================================" << std::endl;

    // 12. 清理
    sys.Stop();
    return ok ? 0 : 1;
}

// ============================================================
//  main — 根据参数决定角色
// ============================================================

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage:" << std::endl;
        std::cerr << "  " << argv[0] << " -a    # Run as NodeA (server)" << std::endl;
        std::cerr << "  " << argv[0] << " -b    # Run as NodeB (client)" << std::endl;
        return 1;
    }

    std::string role = argv[1];

    if (role == "-a") {
        return RunNodeA();
    } else if (role == "-b") {
        return RunNodeB();
    } else {
        std::cerr << "Unknown role: " << role << std::endl;
        std::cerr << "Use -a for server (NodeA) or -b for client (NodeB)" << std::endl;
        return 1;
    }
}
