/**
 * @file test_actor_msg.cc
 * @brief Actor间互相发送消息的测试用例（使用 EventLoop + 网络栈）
 *
 * 架构：
 *   Server 端（EventLoop + ActorSystem + Acceptor）：
 *     GatewayActor   ── 收到网络数据(NetworkRecv)后，通过 SendToActor 转发给 ProcessorActor
 *     ProcessorActor ── 收到 UserMessage 后处理数据，通过 SendToActor 转发给 ResponderActor
 *     ResponderActor ── 收到 UserMessage 后，通过 SendToNetwork 将结果写回客户端
 *
 *   Client 端（EventLoop + ActorSystem + Connector）：
 *     ClientActor    ── 连接成功后发送测试消息，收到响应后计数
 *
 * 数据流：
 *   Client --TCP--> GatewayActor --SendToActor--> ProcessorActor
 *           --SendToActor--> ResponderActor --SendToNetwork(TCP)--> Client
 *
 * 编译方式见 Makefile.test / run_test.sh
 */

#include "precompiled.h"
#include "Actor.h"
#include "ActorSystem.h"
#include "EventLoop.h"
#include "Acceptor.h"
#include "Connector.h"
#include "Message.h"

#include <cassert>
#include <chrono>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using namespace bllsll;

static const int TEST_PORT = 19527;  // 测试端口（避免与正式服务冲突）
static const int TEST_MSG_COUNT = 10;
static const std::string MSG_PREFIX = "testmsg:";
static const std::string PROCESSED_PREFIX = "processed:";

// ============================================================
//  Server 端 Actor 定义
// ============================================================

/**
 * GatewayActor: 网络网关
 * - Connected: 记录客户端 fd
 * - NetworkRecv: 转发数据给 ProcessorActor（SendToActor）
 * - Disconnected: 记录断开
 */
class GatewayActor : public Actor
{
public:
    uint32_t processorActorId = 0;
    std::atomic<int> recvCount{0};
    std::atomic<int> connCount{0};

    void OnMessage(ActorMessage& msg) override
    {
        switch (msg.type) {
        case MsgType::Connected:
            connCount.fetch_add(1);
            std::cout << "[GatewayActor] client connected, fd=" << msg.fd << std::endl;
            break;

        case MsgType::NetworkRecv: {
            recvCount.fetch_add(1);
            std::cout << "[GatewayActor] recv from network, fd=" << msg.fd
                      << ", data=" << msg.data << std::endl;
            // 转发给 ProcessorActor，保留 fd 以便后续回复
            SendToActor(processorActorId,
                ActorMessage{MsgType::UserMessage, 0, msg.fd, msg.data});
            break;
        }

        case MsgType::Disconnected:
            std::cout << "[GatewayActor] client disconnected, fd=" << msg.fd << std::endl;
            break;

        default:
            break;
        }
    }
};

/**
 * ProcessorActor: 业务处理
 * - 收到 GatewayActor 转发的 UserMessage，加工数据后转发给 ResponderActor
 */
class ProcessorActor : public Actor
{
public:
    uint32_t responderActorId = 0;
    std::atomic<int> processCount{0};

    void OnMessage(ActorMessage& msg) override
    {
        if (msg.type != MsgType::UserMessage) return;

        processCount.fetch_add(1);
        // 加工数据：添加 "processed:" 前缀
        std::string processed = PROCESSED_PREFIX + msg.data;
        std::cout << "[ProcessorActor] processed: " << processed
                  << ", forward to ResponderActor" << std::endl;

        // 转发给 ResponderActor，保留原始 fd
        SendToActor(responderActorId,
            ActorMessage{MsgType::UserMessage, 0, msg.fd, std::move(processed)});
    }
};

/**
 * ResponderActor: 网络响应
 * - 收到 ProcessorActor 转发的 UserMessage，通过 SendToNetwork 写回客户端
 */
class ResponderActor : public Actor
{
public:
    std::atomic<int> respondCount{0};

    void OnMessage(ActorMessage& msg) override
    {
        if (msg.type != MsgType::UserMessage) return;

        respondCount.fetch_add(1);
        std::cout << "[ResponderActor] sending response to fd=" << msg.fd
                  << ", data=" << msg.data << std::endl;

        // 通过 EventLoop 的 Connection 发送网络数据
        SendToNetwork(msg.fd, msg.data.c_str(), msg.data.size());
    }
};

// ============================================================
//  Client 端 Actor 定义
// ============================================================

/**
 * TestClientActor: 测试客户端
 * - Connected: 连接成功后发送 TEST_MSG_COUNT 条消息
 * - NetworkRecv: 收到服务端响应，验证并计数
 */
class TestClientActor : public Actor
{
public:
    std::atomic<int> recvCount{0};
    std::atomic<bool> allReceived{false};
    // 记录收到的响应数据（仅用于验证）
    bllsll::SpinLockQueue<std::string> responses;

    void OnMessage(ActorMessage& msg) override
    {
        switch (msg.type) {
        case MsgType::Connected:
            std::cout << "[TestClientActor] connected to server, fd=" << msg.fd << std::endl;
            // 连接成功后发送测试消息
            for (int i = 0; i < TEST_MSG_COUNT; ++i) {
                std::string data = MSG_PREFIX + std::to_string(i);
                SendToNetwork(msg.fd, data.c_str(), data.size());
            }
            std::cout << "[TestClientActor] sent " << TEST_MSG_COUNT << " messages" << std::endl;
            break;

        case MsgType::NetworkRecv: {
            std::cout << "[TestClientActor] recv response: " << msg.data << std::endl;
            responses.push(msg.data);
            int cnt = recvCount.fetch_add(1) + 1;
            if (cnt >= TEST_MSG_COUNT) {
                allReceived.store(true);
            }
            break;
        }

        case MsgType::Disconnected:
            std::cout << "[TestClientActor] disconnected, fd=" << msg.fd << std::endl;
            break;

        default:
            break;
        }
    }
};

// ============================================================
//  测试主函数
// ============================================================

int main()
{
    std::cout << "======================================" << std::endl;
    std::cout << "  Actor Inter-Message Test (with EventLoop)" << std::endl;
    std::cout << "======================================" << std::endl;

    // -------------------------------------------------------
    //  1. 启动 Server 端
    // -------------------------------------------------------
    std::cout << "\n[Setup] Starting server..." << std::endl;

    EventLoop serverLoop;
    ActorSystem serverActorSys;

    serverLoop.Create();
    serverActorSys.Start(4, &serverLoop);
    serverLoop.SetActorSystem(&serverActorSys);

    // 创建 3 个服务端 Actor
    auto gatewayPtr   = std::make_unique<GatewayActor>();
    auto processorPtr = std::make_unique<ProcessorActor>();
    auto responderPtr = std::make_unique<ResponderActor>();

    GatewayActor*   gateway   = gatewayPtr.get();
    ProcessorActor* processor = processorPtr.get();
    ResponderActor* responder = responderPtr.get();

    uint32_t gatewayId   = serverActorSys.RegisterActor(std::move(gatewayPtr));
    uint32_t processorId = serverActorSys.RegisterActor(std::move(processorPtr));
    uint32_t responderId = serverActorSys.RegisterActor(std::move(responderPtr));

    // 建立 Actor 间的转发链路：Gateway → Processor → Responder
    gateway->processorActorId   = processorId;
    processor->responderActorId = responderId;

    // 启动 Acceptor，新连接绑定到 GatewayActor
    // Acceptor 构造时通过 loop_->AddConnection(this) 注册，生命周期由 EventLoop 管理
    new Acceptor(TEST_PORT, &serverLoop, gatewayId);

    std::cout << "[Setup] Server started on port " << TEST_PORT << std::endl;
    std::cout << "[Setup] Actor chain: GatewayActor(id=" << gatewayId
              << ") -> ProcessorActor(id=" << processorId
              << ") -> ResponderActor(id=" << responderId << ")" << std::endl;

    // 等待服务端就绪
    usleep(100000);  // 100ms

    // -------------------------------------------------------
    //  2. 启动 Client 端
    // -------------------------------------------------------
    std::cout << "\n[Setup] Starting client..." << std::endl;

    EventLoop clientLoop;
    ActorSystem clientActorSys;

    clientLoop.Create();
    clientActorSys.Start(2, &clientLoop);
    clientLoop.SetActorSystem(&clientActorSys);

    auto clientActorPtr = std::make_unique<TestClientActor>();
    TestClientActor* clientActor = clientActorPtr.get();
    uint32_t clientActorId = clientActorSys.RegisterActor(std::move(clientActorPtr));

    // 创建 Connector 连接服务器（构造时即发起连接）
    // Connector 构造时通过 loop_->AddConnection(this) 注册，生命周期由 EventLoop 管理
    new Connector(&clientLoop, TEST_PORT, "127.0.0.1", clientActorId);

    std::cout << "[Setup] Client connecting to 127.0.0.1:" << TEST_PORT << std::endl;

    // -------------------------------------------------------
    //  3. 等待客户端收到全部响应
    // -------------------------------------------------------
    std::cout << "\n[Test] Waiting for " << TEST_MSG_COUNT << " round-trip messages..." << std::endl;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!clientActor->allReceived.load() && std::chrono::steady_clock::now() < deadline) {
        usleep(10000);  // 10ms
    }

    // -------------------------------------------------------
    //  4. 验证结果
    // -------------------------------------------------------
    std::cout << "\n[Verify] Checking results..." << std::endl;

    bool ok = true;
    int clientRecv = clientActor->recvCount.load();
    int gatewayRecv = gateway->recvCount.load();
    int processorCount = processor->processCount.load();
    int responderCount = responder->respondCount.load();
    int gatewayConn = gateway->connCount.load();

    std::cout << "  GatewayActor:   connCount=" << gatewayConn
              << ", recvCount=" << gatewayRecv << std::endl;
    std::cout << "  ProcessorActor: processCount=" << processorCount << std::endl;
    std::cout << "  ResponderActor: respondCount=" << responderCount << std::endl;
    std::cout << "  TestClientActor: recvCount=" << clientRecv << std::endl;

    // 检查连接数
    if (gatewayConn < 1) {
        std::cout << "[FAIL] GatewayActor connCount=" << gatewayConn << ", expected >= 1" << std::endl;
        ok = false;
    }

    // 检查各 Actor 处理的消息数量
    // 注意：由于 TCP 流式传输，消息可能被合并或拆分，
    // 所以用 >= 来判断（至少收到了数据）
    if (gatewayRecv < 1) {
        std::cout << "[FAIL] GatewayActor recvCount=" << gatewayRecv << ", expected >= 1" << std::endl;
        ok = false;
    }
    if (processorCount < 1) {
        std::cout << "[FAIL] ProcessorActor processCount=" << processorCount << ", expected >= 1" << std::endl;
        ok = false;
    }
    if (responderCount < 1) {
        std::cout << "[FAIL] ResponderActor respondCount=" << responderCount << ", expected >= 1" << std::endl;
        ok = false;
    }
    if (clientRecv < 1) {
        std::cout << "[FAIL] TestClientActor recvCount=" << clientRecv << ", expected >= 1" << std::endl;
        ok = false;
    }

    // 验证 Actor 间转发链路完整：Gateway == Processor == Responder
    if (gatewayRecv != processorCount) {
        std::cout << "[FAIL] Gateway->Processor chain broken: gatewayRecv=" << gatewayRecv
                  << " != processorCount=" << processorCount << std::endl;
        ok = false;
    }
    if (processorCount != responderCount) {
        std::cout << "[FAIL] Processor->Responder chain broken: processorCount=" << processorCount
                  << " != responderCount=" << responderCount << std::endl;
        ok = false;
    }

    // 验证响应内容包含 processed 前缀
    bool contentOk = true;
    auto resp = clientActor->responses.pop();
    while (resp) {
        // 响应可能是多条消息合并的（TCP流），检查是否包含 processed 前缀
        if (resp->find(PROCESSED_PREFIX) == std::string::npos) {
            std::cout << "[FAIL] Response missing prefix '" << PROCESSED_PREFIX
                      << "': " << *resp << std::endl;
            contentOk = false;
        }
        resp = clientActor->responses.pop();
    }
    if (!contentOk) ok = false;

    // -------------------------------------------------------
    //  5. 清理
    // -------------------------------------------------------
    std::cout << "\n[Cleanup] Stopping..." << std::endl;

    clientActorSys.Stop();
    serverActorSys.Stop();
    // EventLoop 的析构会 join loop 线程并清理连接

    // -------------------------------------------------------
    //  6. 输出结果
    // -------------------------------------------------------
    std::cout << "\n======================================" << std::endl;
    if (ok) {
        std::cout << "  [PASS] All checks passed!" << std::endl;
        std::cout << "  Data flow verified:" << std::endl;
        std::cout << "    Client --TCP--> GatewayActor(" << gatewayRecv << " msgs)" << std::endl;
        std::cout << "      --SendToActor--> ProcessorActor(" << processorCount << " msgs)" << std::endl;
        std::cout << "      --SendToActor--> ResponderActor(" << responderCount << " msgs)" << std::endl;
        std::cout << "      --SendToNetwork(TCP)--> Client(" << clientRecv << " msgs)" << std::endl;
    } else {
        std::cout << "  [FAIL] Some checks failed!" << std::endl;
    }
    std::cout << "======================================" << std::endl;

    return ok ? 0 : 1;
}
