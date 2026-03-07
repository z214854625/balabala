/**
 * @file test_actor_msg.cc
 * @brief Actor间互相发送消息的测试用例（使用 EventLoop + 网络栈）
 *
 * 架构：
 *   Server 端（EventLoop + ActorSystem + Acceptor）3 个 Actor 互相通信：
 *     GatewayActor   -- 收到网络数据(NetworkRecv)后，SendToActor 给 ProcessorActor
 *     ProcessorActor -- 加工数据后 SendToActor 给 ResponderActor，
 *                       同时 SendToActor 回复确认给 GatewayActor（双向通信）
 *     ResponderActor -- 通过 SendToNetwork 将结果写回客户端
 *
 *   Client 端（EventLoop + ActorSystem + Connector）：
 *     TestClientActor -- 连接成功后由 main() 主动调用 SendToNetwork 发送消息
 *
 * 数据流：
 *   main() --SendToNetwork(TCP)--> Server
 *   GatewayActor --SendToActor--> ProcessorActor --SendToActor--> ResponderActor
 *                                 ProcessorActor --SendToActor--> GatewayActor (确认)
 *   ResponderActor --SendToNetwork(TCP)--> Client
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
 * - NetworkRecv: 转发数据给 ProcessorActor（Actor间通信①）
 * - UserMessage: 收到 ProcessorActor 的确认回复（Actor间通信③，双向）
 * - Disconnected: 记录断开
 */
class GatewayActor : public Actor
{
public:
    uint32_t processorActorId = 0;
    std::atomic<int> recvCount{0};
    std::atomic<int> connCount{0};
    std::atomic<int> ackCount{0};   // 收到 ProcessorActor 确认的次数

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
            // Actor间通信①: 转发给 ProcessorActor，保留 fd 以便后续回复
            SendToActor(processorActorId,
                ActorMessage{MsgType::UserMessage, 0, msg.fd, msg.data});
            break;
        }

        case MsgType::UserMessage: {
            // Actor间通信③: 收到 ProcessorActor 的确认回复（双向通信）
            ackCount.fetch_add(1);
            std::cout << "[GatewayActor] got ACK from ProcessorActor: " << msg.data << std::endl;
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
 * - 收到 GatewayActor 转发的 UserMessage
 *   1) 加工数据后 SendToActor 给 ResponderActor（Actor间通信②）
 *   2) SendToActor 回复确认给 GatewayActor（Actor间通信③，双向）
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
        std::cout << "[ProcessorActor] processed: " << processed << std::endl;

        // Actor间通信②: 转发加工后的数据给 ResponderActor
        SendToActor(responderActorId,
            ActorMessage{MsgType::UserMessage, 0, msg.fd, processed});

        // Actor间通信③: 回复确认给 GatewayActor（msg.sourceId 是发送方的 actorId）
        std::string ack = "ack:" + msg.data;
        SendToActor(msg.sourceId,
            ActorMessage{MsgType::UserMessage, 0, msg.fd, std::move(ack)});
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

        // 通过 EventLoop 的 Connection 将处理结果发回客户端
        SendToNetwork(msg.fd, msg.data.c_str(), msg.data.size());
    }
};

// ============================================================
//  Client 端 Actor 定义
// ============================================================

/**
 * TestClientActor: 测试客户端
 * - Connected: 记录连接成功和 fd
 * - NetworkRecv: 收到服务端响应，验证并计数
 */
class TestClientActor : public Actor
{
public:
    std::atomic<int> clientFd{-1};          // 连接成功后的 fd
    std::atomic<bool> connected{false};     // 是否已连接
    std::atomic<int> recvCount{0};
    std::atomic<bool> allReceived{false};
    bllsll::SpinLockQueue<std::string> responses;

    void OnMessage(ActorMessage& msg) override
    {
        switch (msg.type) {
        case MsgType::Connected:
            clientFd.store(msg.fd);
            connected.store(true);
            std::cout << "[TestClientActor] connected to server, fd=" << msg.fd << std::endl;
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
            connected.store(false);
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

    // 建立 Actor 间的转发链路：
    //   Gateway -> Processor -> Responder (正向)
    //   Processor -> Gateway (确认回复，反向)
    gateway->processorActorId   = processorId;
    processor->responderActorId = responderId;

    // 启动 Acceptor，新连接绑定到 GatewayActor
    new Acceptor(TEST_PORT, &serverLoop, gatewayId);

    std::cout << "[Setup] Server started on port " << TEST_PORT << std::endl;
    std::cout << "[Setup] Actor chain:" << std::endl;
    std::cout << "  GatewayActor(id=" << gatewayId << ")" << std::endl;
    std::cout << "    --SendToActor--> ProcessorActor(id=" << processorId << ")" << std::endl;
    std::cout << "      --SendToActor--> ResponderActor(id=" << responderId << ")" << std::endl;
    std::cout << "      --SendToActor--> GatewayActor (ACK, bidirectional)" << std::endl;

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
    new Connector(&clientLoop, TEST_PORT, "127.0.0.1", clientActorId);

    std::cout << "[Setup] Client connecting to 127.0.0.1:" << TEST_PORT << std::endl;

    // -------------------------------------------------------
    //  3. 等待连接建立，然后在 main() 中主动发送消息
    // -------------------------------------------------------
    std::cout << "\n[Test] Waiting for connection..." << std::endl;

    auto connDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!clientActor->connected.load() && std::chrono::steady_clock::now() < connDeadline) {
        usleep(10000);  // 10ms
    }

    if (!clientActor->connected.load()) {
        std::cout << "[FAIL] Client failed to connect to server!" << std::endl;
        clientActorSys.Stop();
        serverActorSys.Stop();
        return 1;
    }

    int fd = clientActor->clientFd.load();
    std::cout << "[Test] Connected! fd=" << fd << std::endl;
    std::cout << "[Test] Sending " << TEST_MSG_COUNT << " messages to server..." << std::endl;

    // 在 main() 中显式发送消息给服务器
    // 通过 EventLoop::GetConnection() -> Connection::Send() 发送 TCP 数据
    for (int i = 0; i < TEST_MSG_COUNT; ++i) {
        std::string data = MSG_PREFIX + std::to_string(i);
        std::cout << "[Test] >>> Sending: " << data << std::endl;
        // 通过 EventLoop 获取 Connection 对象发送 TCP 数据
        auto* conn = clientLoop.GetConnection(fd);
        if (conn) {
            conn->Send(data.c_str(), data.size());
        } else {
            std::cout << "[FAIL] Connection lost! fd=" << fd << std::endl;
            break;
        }
        usleep(10000);  // 10ms 间隔，避免消息粘包
    }

    std::cout << "[Test] All messages sent. Waiting for responses..." << std::endl;

    // -------------------------------------------------------
    //  4. 等待客户端收到全部响应
    // -------------------------------------------------------
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!clientActor->allReceived.load() && std::chrono::steady_clock::now() < deadline) {
        usleep(10000);  // 10ms
    }

    // -------------------------------------------------------
    //  5. 验证结果
    // -------------------------------------------------------
    std::cout << "\n[Verify] Checking results..." << std::endl;

    bool ok = true;
    int clientRecv     = clientActor->recvCount.load();
    int gatewayRecv    = gateway->recvCount.load();
    int gatewayAck     = gateway->ackCount.load();
    int processorCount = processor->processCount.load();
    int responderCount = responder->respondCount.load();
    int gatewayConn    = gateway->connCount.load();

    std::cout << "  GatewayActor:   connCount=" << gatewayConn
              << ", recvCount=" << gatewayRecv
              << ", ackCount=" << gatewayAck << std::endl;
    std::cout << "  ProcessorActor: processCount=" << processorCount << std::endl;
    std::cout << "  ResponderActor: respondCount=" << responderCount << std::endl;
    std::cout << "  TestClientActor: recvCount=" << clientRecv << std::endl;

    // 检查连接数
    if (gatewayConn < 1) {
        std::cout << "[FAIL] GatewayActor connCount=" << gatewayConn << ", expected >= 1" << std::endl;
        ok = false;
    }

    // 检查各 Actor 处理的消息数量（>= 1 即可，TCP 流可能合并/拆分消息）
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

    // 验证 Actor 间转发链路完整：Gateway == Processor == Responder 计数一致
    if (gatewayRecv != processorCount) {
        std::cout << "[FAIL] Gateway->Processor chain broken: " << gatewayRecv
                  << " != " << processorCount << std::endl;
        ok = false;
    }
    if (processorCount != responderCount) {
        std::cout << "[FAIL] Processor->Responder chain broken: " << processorCount
                  << " != " << responderCount << std::endl;
        ok = false;
    }

    // 验证双向通信：ProcessorActor 回复了确认给 GatewayActor
    if (gatewayAck < 1) {
        std::cout << "[FAIL] Processor->Gateway ACK missing: ackCount=" << gatewayAck << std::endl;
        ok = false;
    }
    if (gatewayAck != processorCount) {
        std::cout << "[FAIL] ACK count mismatch: ackCount=" << gatewayAck
                  << " != processCount=" << processorCount << std::endl;
        ok = false;
    }

    // 验证响应内容包含 processed 前缀
    bool contentOk = true;
    auto resp = clientActor->responses.pop();
    while (resp) {
        if (resp->find(PROCESSED_PREFIX) == std::string::npos) {
            std::cout << "[FAIL] Response missing prefix '" << PROCESSED_PREFIX
                      << "': " << *resp << std::endl;
            contentOk = false;
        }
        resp = clientActor->responses.pop();
    }
    if (!contentOk) ok = false;

    // -------------------------------------------------------
    //  6. 清理
    // -------------------------------------------------------
    std::cout << "\n[Cleanup] Stopping..." << std::endl;

    clientActorSys.Stop();
    serverActorSys.Stop();

    // -------------------------------------------------------
    //  7. 输出结果
    // -------------------------------------------------------
    std::cout << "\n======================================" << std::endl;
    if (ok) {
        std::cout << "  [PASS] All checks passed!" << std::endl;
        std::cout << "  Data flow verified:" << std::endl;
        std::cout << "    main() --TCP(Send " << TEST_MSG_COUNT << " msgs)--> Server" << std::endl;
        std::cout << "    GatewayActor(" << gatewayRecv << " msgs)" << std::endl;
        std::cout << "      --SendToActor--> ProcessorActor(" << processorCount << " msgs)" << std::endl;
        std::cout << "      --SendToActor--> ResponderActor(" << responderCount << " msgs)" << std::endl;
        std::cout << "      --SendToActor--> GatewayActor(ACK: " << gatewayAck << " msgs, bidirectional)" << std::endl;
        std::cout << "    ResponderActor --TCP--> TestClientActor(" << clientRecv << " msgs)" << std::endl;
    } else {
        std::cout << "  [FAIL] Some checks failed!" << std::endl;
    }
    std::cout << "======================================" << std::endl;

    return ok ? 0 : 1;
}
