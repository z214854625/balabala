/**
 * @file test_actor_msg.cc
 * @brief MMO 游戏场景的 Actor 间消息测试用例
 *
 * 模拟 MMO 游戏服务器的 Actor 架构：
 *   GatewayActor (单例，绑定 Acceptor)
 *     - 收到新连接 -> 创建 PlayerActor, 重新绑定 fd 到该 PlayerActor
 *     - 收到广播请求 -> 转发给所有其他 PlayerActor
 *
 *   PlayerActor (每个玩家一个，fd 直接绑定)
 *     - 收到 "login:name" -> 登录，通知 GatewayActor
 *     - 收到 "chat:text"  -> 发给 GatewayActor 请求广播
 *     - 收到其他 PlayerActor 转发的聊天 -> SendToNetwork 发给自己的客户端
 *
 * 数据流（玩家聊天）：
 *   Client1 --TCP("chat:hello")--> PlayerActor1
 *     --SendToActor(广播请求)--> GatewayActor
 *     --SendToActor(转发)--> PlayerActor2
 *     --SendToNetwork(TCP)--> Client2
 *
 * 编译: make -f Makefile.test
 * 运行: ./run_test.sh
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

static const int TEST_PORT = 19527;

// ============================================================
//  PlayerActor: 每个玩家一个（服务端）
// ============================================================
class PlayerActor : public Actor
{
public:
    uint32_t gatewayActorId = 0;   // 网关 Actor ID
    int fd_ = -1;                  // 该玩家的 TCP fd
    std::string name_;             // 玩家名
    std::atomic<int> recvCount{0}; // 收到的网络消息数
    std::atomic<int> chatRecvCount{0}; // 收到的聊天转发数

    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        switch (msg.type) {
        case MsgType::Connected:
            fd_ = msg.fd;
            std::cout << "[PlayerActor:" << GetActorId() << "] connected, fd=" << fd_ << std::endl;
            break;

        case MsgType::NetworkRecv: {
            recvCount.fetch_add(1);
            std::cout << "[PlayerActor:" << GetActorId() << "] recv: " << msg.data << std::endl;

            // 解析客户端命令
            if (msg.data.find("login:") == 0) {
                // 登录：提取玩家名，回复欢迎消息
                name_ = msg.data.substr(6);
                std::string welcome = "welcome:" + name_;
                SendToNetwork(fd_, welcome.c_str(), welcome.size());
                std::cout << "[PlayerActor:" << GetActorId() << "] " << name_ << " logged in" << std::endl;

                // 通知 GatewayActor 玩家已登录（用于广播列表）
                SendToActor(gatewayActorId,
                    ActorMessage{MsgType::UserMessage, 0, fd_, "player_ready:" + name_});

            } else if (msg.data.find("chat:") == 0) {
                // 聊天：发给 GatewayActor 请求广播给其他玩家
                std::string chatMsg = name_ + ":" + msg.data.substr(5);
                std::cout << "[PlayerActor:" << GetActorId() << "] " << name_
                          << " requests broadcast: " << chatMsg << std::endl;
                SendToActor(gatewayActorId,
                    ActorMessage{MsgType::UserMessage, 0, fd_, "broadcast:" + chatMsg});
            }
            break;
        }

        case MsgType::UserMessage: {
            // 收到 GatewayActor 转发的其他玩家的聊天消息
            chatRecvCount.fetch_add(1);
            std::cout << "[PlayerActor:" << GetActorId() << "] forwarding chat to client fd="
                      << fd_ << ": " << msg.data << std::endl;
            // 通过网络发给该玩家的客户端
            SendToNetwork(fd_, msg.data.c_str(), msg.data.size());
            break;
        }

        case MsgType::Disconnected:
            std::cout << "[PlayerActor:" << GetActorId() << "] " << name_
                      << " disconnected, fd=" << fd_ << std::endl;
            // 通知 GatewayActor 移除该玩家
            SendToActor(gatewayActorId,
                ActorMessage{MsgType::UserMessage, 0, fd_, "player_leave"});
            break;

        default:
            break;
        }
        co_return;
    }
};

// ============================================================
//  GatewayActor: 网关（服务端，单例）
// ============================================================
class GatewayActor : public Actor
{
public:
    // 在线玩家列表：actorId -> PlayerActor*（用于广播）
    std::vector<uint32_t> playerActorIds_;
    std::atomic<int> connCount{0};
    std::atomic<int> broadcastCount{0};

    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        switch (msg.type) {
        case MsgType::Connected: {
            connCount.fetch_add(1);
            std::cout << "[GatewayActor] new connection fd=" << msg.fd
                      << ", creating PlayerActor..." << std::endl;

            // 为每个新连接创建一个 PlayerActor
            auto playerPtr = std::make_unique<PlayerActor>();
            playerPtr->gatewayActorId = GetActorId();
            uint32_t playerId = GetSystem()->RegisterActor(std::move(playerPtr));

            // 重新绑定 fd 到 PlayerActor（后续该 fd 的网络数据直接发给 PlayerActor）
            GetSystem()->BindFdToActor(msg.fd, playerId);

            std::cout << "[GatewayActor] PlayerActor(id=" << playerId
                      << ") created, fd=" << msg.fd << " rebound" << std::endl;

            // 通知 PlayerActor 连接信息
            SendToActor(playerId, ActorMessage{MsgType::Connected, 0, msg.fd, ""});
            break;
        }

        case MsgType::UserMessage: {
            // 处理 PlayerActor 发来的消息
            if (msg.data.find("player_ready:") == 0) {
                // 玩家登录完成，加入广播列表
                playerActorIds_.push_back(msg.sourceId);
                std::cout << "[GatewayActor] player registered, actorId=" << msg.sourceId
                          << ", online=" << playerActorIds_.size() << std::endl;

            } else if (msg.data.find("broadcast:") == 0) {
                // 广播聊天消息给所有其他玩家
                std::string chatData = msg.data.substr(10); // 去掉 "broadcast:" 前缀
                uint32_t senderId = msg.sourceId;
                int count = 0;
                for (uint32_t pid : playerActorIds_) {
                    if (pid != senderId) {
                        // 转发给其他 PlayerActor
                        SendToActor(pid,
                            ActorMessage{MsgType::UserMessage, 0, -1, "chat:" + chatData});
                        count++;
                    }
                }
                broadcastCount.fetch_add(1);
                std::cout << "[GatewayActor] broadcast from actorId=" << senderId
                          << " to " << count << " players: " << chatData << std::endl;

            } else if (msg.data == "player_leave") {
                // 玩家离线，从列表移除
                auto it = std::find(playerActorIds_.begin(), playerActorIds_.end(), msg.sourceId);
                if (it != playerActorIds_.end()) {
                    playerActorIds_.erase(it);
                }
                std::cout << "[GatewayActor] player left, actorId=" << msg.sourceId
                          << ", online=" << playerActorIds_.size() << std::endl;
            }
            break;
        }

        default:
            break;
        }
        co_return;
    }
};

// ============================================================
//  TestClientActor: 测试客户端
// ============================================================
class TestClientActor : public Actor
{
public:
    std::atomic<int> clientFd{-1};
    std::atomic<bool> connected{false};
    std::atomic<int> recvCount{0};
    bllsll::SpinLockQueue<std::string> responses;  // 收到的所有响应

    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        switch (msg.type) {
        case MsgType::Connected:
            clientFd.store(msg.fd);
            connected.store(true);
            std::cout << "[TestClient:" << GetActorId() << "] connected, fd=" << msg.fd << std::endl;
            break;

        case MsgType::NetworkRecv: {
            recvCount.fetch_add(1);
            std::cout << "[TestClient:" << GetActorId() << "] recv: " << msg.data << std::endl;
            responses.push(msg.data);
            break;
        }

        case MsgType::Disconnected:
            std::cout << "[TestClient:" << GetActorId() << "] disconnected" << std::endl;
            connected.store(false);
            break;

        default:
            break;
        }
        co_return;
    }

    // 等待收到指定数量的消息
    bool WaitForMessages(int count, int timeoutMs = 5000)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (recvCount.load() < count && std::chrono::steady_clock::now() < deadline) {
            usleep(10000);
        }
        return recvCount.load() >= count;
    }
};

// ============================================================
//  辅助：等待连接建立
// ============================================================
static bool WaitConnected(TestClientActor* client, int timeoutMs = 5000)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!client->connected.load() && std::chrono::steady_clock::now() < deadline) {
        usleep(10000);
    }
    return client->connected.load();
}

// 辅助：通过 EventLoop 发送 TCP 数据
static void SendTcp(EventLoop& loop, int fd, const std::string& data)
{
    auto* conn = loop.GetConnection(fd);
    if (conn) {
        conn->Send(data.c_str(), data.size());
        std::cout << "[SendTcp] fd=" << fd << ", data=" << data << std::endl;
    } else {
        std::cout << "[SendTcp] ERROR: connection lost, fd=" << fd << std::endl;
    }
}

// ============================================================
//  测试主函数
// ============================================================
int main()
{
    std::cout << "======================================" << std::endl;
    std::cout << "  MMO Actor Test: Per-Player Actor" << std::endl;
    std::cout << "======================================" << std::endl;

    // -------------------------------------------------------
    //  1. 启动服务器
    // -------------------------------------------------------
    std::cout << "\n[Setup] Starting server..." << std::endl;

    EventLoop serverLoop;
    ActorSystem serverSys;

    serverLoop.Create();
    serverSys.Start(4, &serverLoop);
    serverLoop.SetActorSystem(&serverSys);

    // 创建 GatewayActor（网关，接收所有新连接）
    auto gatewayPtr = std::make_unique<GatewayActor>();
    GatewayActor* gateway = gatewayPtr.get();
    uint32_t gatewayId = serverSys.RegisterActor(std::move(gatewayPtr));

    // 启动 Acceptor，新连接先发给 GatewayActor
    new Acceptor(TEST_PORT, &serverLoop, gatewayId);

    std::cout << "[Setup] Server started on port " << TEST_PORT << std::endl;
    std::cout << "[Setup] GatewayActor(id=" << gatewayId << ") waiting for players..." << std::endl;

    usleep(100000);  // 等待服务端就绪

    // -------------------------------------------------------
    //  2. 启动 Client1（玩家1）
    // -------------------------------------------------------
    std::cout << "\n[Setup] Starting Client1 (player1)..." << std::endl;

    EventLoop client1Loop;
    ActorSystem client1Sys;
    client1Loop.Create();
    client1Sys.Start(2, &client1Loop);
    client1Loop.SetActorSystem(&client1Sys);

    auto client1Ptr = std::make_unique<TestClientActor>();
    TestClientActor* client1 = client1Ptr.get();
    uint32_t client1Id = client1Sys.RegisterActor(std::move(client1Ptr));
    new Connector(&client1Loop, TEST_PORT, "127.0.0.1", client1Id);

    if (!WaitConnected(client1)) {
        std::cout << "[FAIL] Client1 connect timeout!" << std::endl;
        return 1;
    }
    int fd1 = client1->clientFd.load();
    std::cout << "[Setup] Client1 connected, fd=" << fd1 << std::endl;

    // -------------------------------------------------------
    //  3. 启动 Client2（玩家2）
    // -------------------------------------------------------
    std::cout << "\n[Setup] Starting Client2 (player2)..." << std::endl;

    EventLoop client2Loop;
    ActorSystem client2Sys;
    client2Loop.Create();
    client2Sys.Start(2, &client2Loop);
    client2Loop.SetActorSystem(&client2Sys);

    auto client2Ptr = std::make_unique<TestClientActor>();
    TestClientActor* client2 = client2Ptr.get();
    uint32_t client2Id = client2Sys.RegisterActor(std::move(client2Ptr));
    new Connector(&client2Loop, TEST_PORT, "127.0.0.1", client2Id);

    if (!WaitConnected(client2)) {
        std::cout << "[FAIL] Client2 connect timeout!" << std::endl;
        return 1;
    }
    int fd2 = client2->clientFd.load();
    std::cout << "[Setup] Client2 connected, fd=" << fd2 << std::endl;

    usleep(100000);  // 等待连接处理完成

    // -------------------------------------------------------
    //  4. 两个玩家登录
    // -------------------------------------------------------
    std::cout << "\n[Test] Player1 logging in..." << std::endl;
    SendTcp(client1Loop, fd1, "login:player1");
    usleep(200000);  // 等待登录处理

    std::cout << "[Test] Player2 logging in..." << std::endl;
    SendTcp(client2Loop, fd2, "login:player2");
    usleep(200000);  // 等待登录处理

    // 验证收到 welcome 消息
    if (!client1->WaitForMessages(1, 3000)) {
        std::cout << "[FAIL] Client1 did not receive welcome!" << std::endl;
    }
    if (!client2->WaitForMessages(1, 3000)) {
        std::cout << "[FAIL] Client2 did not receive welcome!" << std::endl;
    }

    std::cout << "[Test] Both players logged in. Online=" << gateway->playerActorIds_.size() << std::endl;

    // -------------------------------------------------------
    //  5. Player1 发送聊天 -> Player2 应该收到
    // -------------------------------------------------------
    std::cout << "\n[Test] Player1 sends chat 'hello from p1'..." << std::endl;
    int client2RecvBefore = client2->recvCount.load();
    SendTcp(client1Loop, fd1, "chat:hello from p1");
    usleep(300000);  // 等待广播

    // Player2 应该收到来自 Player1 的聊天
    if (!client2->WaitForMessages(client2RecvBefore + 1, 3000)) {
        std::cout << "[WARN] Client2 may not have received Player1's chat" << std::endl;
    }

    // -------------------------------------------------------
    //  6. Player2 发送聊天 -> Player1 应该收到
    // -------------------------------------------------------
    std::cout << "\n[Test] Player2 sends chat 'hi from p2'..." << std::endl;
    int client1RecvBefore = client1->recvCount.load();
    SendTcp(client2Loop, fd2, "chat:hi from p2");
    usleep(300000);  // 等待广播

    // Player1 应该收到来自 Player2 的聊天
    if (!client1->WaitForMessages(client1RecvBefore + 1, 3000)) {
        std::cout << "[WARN] Client1 may not have received Player2's chat" << std::endl;
    }

    // -------------------------------------------------------
    //  7. 验证结果
    // -------------------------------------------------------
    std::cout << "\n[Verify] Checking results..." << std::endl;

    bool ok = true;

    // 服务端：应该有 2 个连接
    int connCount = gateway->connCount.load();
    if (connCount != 2) {
        std::cout << "[FAIL] GatewayActor connCount=" << connCount << ", expected=2" << std::endl;
        ok = false;
    }

    // 服务端：应该有 2 个在线玩家
    int online = (int)gateway->playerActorIds_.size();
    if (online != 2) {
        std::cout << "[FAIL] online players=" << online << ", expected=2" << std::endl;
        ok = false;
    }

    // 服务端：应该有 2 次广播（player1 和 player2 各发了一条聊天）
    int broadcasts = gateway->broadcastCount.load();
    if (broadcasts != 2) {
        std::cout << "[FAIL] broadcastCount=" << broadcasts << ", expected=2" << std::endl;
        ok = false;
    }

    // Client1: 应该收到 welcome + player2 的聊天 = 至少 2 条
    int c1Recv = client1->recvCount.load();
    std::cout << "  Client1 recvCount=" << c1Recv << std::endl;
    if (c1Recv < 2) {
        std::cout << "[FAIL] Client1 recvCount=" << c1Recv << ", expected >= 2" << std::endl;
        ok = false;
    }

    // Client2: 应该收到 welcome + player1 的聊天 = 至少 2 条
    int c2Recv = client2->recvCount.load();
    std::cout << "  Client2 recvCount=" << c2Recv << std::endl;
    if (c2Recv < 2) {
        std::cout << "[FAIL] Client2 recvCount=" << c2Recv << ", expected >= 2" << std::endl;
        ok = false;
    }

    // 验证 Client1 收到的消息内容
    std::cout << "\n  Client1 received messages:" << std::endl;
    bool c1HasWelcome = false, c1HasChat = false;
    auto r1 = client1->responses.pop();
    while (r1) {
        std::cout << "    -> " << *r1 << std::endl;
        if (r1->find("welcome:") != std::string::npos) c1HasWelcome = true;
        if (r1->find("chat:") != std::string::npos && r1->find("p2") != std::string::npos) c1HasChat = true;
        r1 = client1->responses.pop();
    }

    // 验证 Client2 收到的消息内容
    std::cout << "  Client2 received messages:" << std::endl;
    bool c2HasWelcome = false, c2HasChat = false;
    auto r2 = client2->responses.pop();
    while (r2) {
        std::cout << "    -> " << *r2 << std::endl;
        if (r2->find("welcome:") != std::string::npos) c2HasWelcome = true;
        if (r2->find("chat:") != std::string::npos && r2->find("p1") != std::string::npos) c2HasChat = true;
        r2 = client2->responses.pop();
    }

    if (!c1HasWelcome) { std::cout << "[FAIL] Client1 missing welcome" << std::endl; ok = false; }
    if (!c2HasWelcome) { std::cout << "[FAIL] Client2 missing welcome" << std::endl; ok = false; }
    if (!c1HasChat) { std::cout << "[FAIL] Client1 missing chat from p2" << std::endl; ok = false; }
    if (!c2HasChat) { std::cout << "[FAIL] Client2 missing chat from p1" << std::endl; ok = false; }

    // -------------------------------------------------------
    //  8. 清理
    // -------------------------------------------------------
    std::cout << "\n[Cleanup] Stopping..." << std::endl;
    client1Sys.Stop();
    client2Sys.Stop();
    serverSys.Stop();

    // -------------------------------------------------------
    //  9. 输出结果
    // -------------------------------------------------------
    std::cout << "\n======================================" << std::endl;
    if (ok) {
        std::cout << "  [PASS] MMO Actor Test passed!" << std::endl;
        std::cout << std::endl;
        std::cout << "  Architecture:" << std::endl;
        std::cout << "    GatewayActor (gateway, one per server)" << std::endl;
        std::cout << "      +-- PlayerActor1 (per-player, fd rebound)" << std::endl;
        std::cout << "      +-- PlayerActor2 (per-player, fd rebound)" << std::endl;
        std::cout << std::endl;
        std::cout << "  Verified flows:" << std::endl;
        std::cout << "    1. New connection -> GatewayActor creates PlayerActor, rebinds fd" << std::endl;
        std::cout << "    2. Client sends 'login:name' -> PlayerActor handles directly" << std::endl;
        std::cout << "    3. Client sends 'chat:text' -> PlayerActor -> GatewayActor (broadcast)" << std::endl;
        std::cout << "       -> other PlayerActors -> SendToNetwork -> other Clients" << std::endl;
        std::cout << "    4. Bidirectional: PlayerActor <-> GatewayActor <-> PlayerActor" << std::endl;
    } else {
        std::cout << "  [FAIL] Some checks failed!" << std::endl;
    }
    std::cout << "======================================" << std::endl;

    return ok ? 0 : 1;
}
