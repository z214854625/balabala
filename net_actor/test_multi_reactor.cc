/**
 * @file test_multi_reactor.cc
 * @brief 主从 Reactor + Gateway 路由 + 多业务 Actor 测试
 *
 * 架构：
 *   Main EventLoop (Acceptor 线程)
 *       │ accept 新 fd
 *       ▼
 *   SubReactor 0..N (N 个 sub loop 线程，I/O 并行)
 *       │ HandleRead 投递 NetworkRecv
 *       ▼
 *   GatewayActor （所有连接的网关）
 *       │ 按 fd 维护 inputBuf 解半包/粘包
 *       │ 按协议头 cmd 路由
 *       │   cmd ∈ 1000~1999 →  SceneActor (module=1)
 *       │   cmd ∈ 2000~2999 →  ChatActor  (module=2)
 *       ▼
 *   SceneActor / ChatActor （业务 Actor 并行处理）
 *       │ 按 cmd 处理具体业务（这里简化为 echo）
 *       │ SendToNetwork(fd, ...) 原样回包
 *       ▼
 *   Actor::SendToNetwork → 按 fd 找 sub loop → write socket
 *
 * 协议格式：
 *   +----------+----------+--------------+
 *   | len(4B)  | cmd(2B)  | payload      |
 *   +----------+----------+--------------+
 *   len = 2 + payload.size()  （len 包含 cmd 自身）
 *
 * 编译: make -f Makefile.multi_reactor
 * 运行: ./run_test_multi_reactor.sh
 */

#include "precompiled.h"
#include "Actor.h"
#include "ActorSystem.h"
#include "EventLoop.h"
#include "EventLoopThreadPool.h"
#include "Acceptor.h"
#include "Connector.h"
#include "Message.h"

#include <cassert>
#include <chrono>
#include <sstream>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

using namespace bllsll;
using namespace std::chrono;

// ============================================================
//  测试配置
// ============================================================
static const int TEST_PORT       = 19528;
static const int SUB_REACTOR_NUM = 4;       // SubReactor 数量
static const int WORKER_NUM      = 8;       // ActorSystem worker 数量
static const int CLIENT_NUM      = 16;      // 并发客户端
static const int SCENE_MSG_PER_CLIENT = 500;  // 每客户端发的场景消息数
static const int CHAT_MSG_PER_CLIENT  = 300;  // 每客户端发的聊天消息数

// 命令字段（按千位段分模块）
//   1xxx → 场景模块
//   2xxx → 聊天模块
//   3xxx → 战斗模块（预留）
//   4xxx → 好友模块（预留）
enum Cmd : uint16_t {
    // 场景模块 1000 ~ 1999
    SCENE_MOVE       = 1000,
    SCENE_PICK_ITEM  = 1001,
    SCENE_USE_SKILL  = 1002,

    // 聊天模块 2000 ~ 2999
    CHAT_WORLD       = 2000,
    CHAT_GUILD       = 2001,
    CHAT_PRIVATE     = 2002,
};

// 按千位段提取模块号：cmd / 1000
//   1000 ~ 1999 → 1
//   2000 ~ 2999 → 2
//   3000 ~ 3999 → 3
static inline uint16_t cmdModule(uint16_t cmd) { return cmd / 1000; }

// ============================================================
//  帧编解码
//  +----------+----------+--------------+
//  | len(4B)  | cmd(2B)  | payload      |
//  +----------+----------+--------------+
//  len = 2 + payload.size()，全部网络字节序
// ============================================================

static std::string encodeFrame(uint16_t cmd, const std::string& payload) {
    uint32_t bodyLen = 2 + (uint32_t)payload.size();
    uint32_t netLen = htonl(bodyLen);
    uint16_t netCmd = htons(cmd);
    std::string frame;
    frame.reserve(4 + bodyLen);
    frame.append(reinterpret_cast<const char*>(&netLen), 4);
    frame.append(reinterpret_cast<const char*>(&netCmd), 2);
    frame.append(payload);
    return frame;
}

// 从一段已校验完整的帧数据中取出 cmd
static uint16_t parseCmd(const std::string& fullFrame) {
    if (fullFrame.size() < 6) return 0;
    uint16_t netCmd;
    std::memcpy(&netCmd, fullFrame.data() + 4, 2);
    return ntohs(netCmd);
}

// ============================================================
//  全局指标（业务 Actor 统计）
// ============================================================

struct BizMetrics {
    std::atomic<int> framesRecv{0};
    std::atomic<int> framesSent{0};
    std::atomic<uint64_t> bytesRecv{0};
    std::atomic<uint64_t> bytesSent{0};
    // 各 cmd 计数
    std::atomic<int> cnt[8]{};   // 简化：用前 8 个槽位
};

// ============================================================
//  SceneActor: 场景业务
//   处理 1000~1999 的命令字（场景模块）
// ============================================================
class SceneActor : public Actor
{
public:
    BizMetrics metrics;

    ActorTask OnCoroutineMessage(ActorMessage msg) override {
        if (msg.type == MsgType::NetworkRecv) {
            uint16_t cmd = parseCmd(msg.data);
            metrics.framesRecv.fetch_add(1);
            metrics.bytesRecv.fetch_add(msg.data.size());

            // 简化业务：原样回传给客户端（典型场景：移动结果广播、技能结果等）
            SendToNetwork(msg.fd, msg.data.data(), (int)msg.data.size());
            metrics.framesSent.fetch_add(1);
            metrics.bytesSent.fetch_add(msg.data.size());

            // 按 cmd 计数
            switch (cmd) {
                case SCENE_MOVE:      metrics.cnt[0].fetch_add(1); break;
                case SCENE_PICK_ITEM: metrics.cnt[1].fetch_add(1); break;
                case SCENE_USE_SKILL: metrics.cnt[2].fetch_add(1); break;
                default: break;
            }
        }
        co_return;
    }
};

// ============================================================
//  ChatActor: 聊天业务
//   处理 2000~2999 的命令字（聊天模块）
// ============================================================
class ChatActor : public Actor
{
public:
    BizMetrics metrics;

    ActorTask OnCoroutineMessage(ActorMessage msg) override {
        if (msg.type == MsgType::NetworkRecv) {
            uint16_t cmd = parseCmd(msg.data);
            metrics.framesRecv.fetch_add(1);
            metrics.bytesRecv.fetch_add(msg.data.size());

            // 简化业务：原样回传（典型场景：聊天广播）
            SendToNetwork(msg.fd, msg.data.data(), (int)msg.data.size());
            metrics.framesSent.fetch_add(1);
            metrics.bytesSent.fetch_add(msg.data.size());

            switch (cmd) {
                case CHAT_WORLD:   metrics.cnt[0].fetch_add(1); break;
                case CHAT_GUILD:   metrics.cnt[1].fetch_add(1); break;
                case CHAT_PRIVATE: metrics.cnt[2].fetch_add(1); break;
                default: break;
            }
        }
        co_return;
    }
};

// ============================================================
//  GatewayActor: 网关，负责
//   - 按 fd 维护 inputBuf，处理 TCP 粘包/半包
//   - 解协议头，按 cmd 千位段 (module = cmd/1000) 路由到对应业务 Actor
//   - 维护连接计数（Connected/Disconnected）
//
//  路由表（moduleRoutes_）：
//    module (cmd/1000) → actorId
//    新增业务模块只需调 RegisterModule(module, actorId)，无需改路由逻辑。
//
//  关键约束：fd 仍绑在 GatewayActor 上（不改绑）。所有 NetworkRecv
//  都进 Gateway，Gateway 完整解出包后转发给业务 Actor。
//  业务 Actor 处理完用 SendToNetwork(fd, ...) 回包，回包路径不经
//  Gateway，直接通过 EventLoop 写回。
// ============================================================
class GatewayActor : public Actor
{
public:
    // module (cmd/1000) → 业务 Actor ID 映射
    // 例：1 → SceneActor, 2 → ChatActor, 3 → BattleActor ...
    std::unordered_map<uint16_t, uint32_t> moduleRoutes_;

    // 注册业务模块路由（启动期调用）
    void RegisterModule(uint16_t module, uint32_t actorId) {
        moduleRoutes_[module] = actorId;
        std::cout << "[Gateway] register module " << module
                  << " (cmd " << (module * 1000) << "~" << (module * 1000 + 999) << ")"
                  << " → actorId=" << actorId << std::endl;
    }

    std::atomic<int> connCount{0};
    std::atomic<int> framesRouted{0};
    std::atomic<int> badFrames{0};
    // 按 module 统计转发次数
    std::unordered_map<uint16_t, std::atomic<int>> routedByModule_;

    // 每个 fd 的接收累积缓冲
    // 同一 Actor 的 OnCoroutineMessage 串行执行，无需加锁
    std::unordered_map<int, std::string> fdInputBuf_;

    ActorTask OnCoroutineMessage(ActorMessage msg) override {
        switch (msg.type) {
            case MsgType::Connected: {
                connCount.fetch_add(1);
                fdInputBuf_[msg.fd] = {};
                std::cout << "[Gateway] new client fd=" << msg.fd
                          << ", total=" << connCount.load() << std::endl;
                break;
            }
            case MsgType::NetworkRecv: {
                onRecv(msg.fd, msg.data);
                break;
            }
            case MsgType::Disconnected: {
                connCount.fetch_sub(1);
                fdInputBuf_.erase(msg.fd);
                std::cout << "[Gateway] client disconnected fd=" << msg.fd
                          << ", remain=" << connCount.load() << std::endl;
                break;
            }
            default: break;
        }
        co_return;
    }

private:
    void onRecv(int fd, const std::string& data) {
        auto& buf = fdInputBuf_[fd];
        buf.append(data);

        // 循环解出所有完整帧
        while (true) {
            if (buf.size() < 4) break;  // 头不全
            uint32_t netLen;
            std::memcpy(&netLen, buf.data(), 4);
            uint32_t bodyLen = ntohl(netLen);
            uint32_t totalLen = 4 + bodyLen;
            if (buf.size() < totalLen) break;  // 体不全

            // 取出完整帧（含 4 字节 len + 2 字节 cmd + payload）
            std::string fullFrame = buf.substr(0, totalLen);
            buf.erase(0, totalLen);

            routeFrame(fd, std::move(fullFrame));
        }
    }

    // 按 cmd 路由到业务 Actor
    void routeFrame(int fd, std::string&& fullFrame) {
        if (fullFrame.size() < 6) {
            badFrames.fetch_add(1);
            return;
        }
        uint16_t cmd = parseCmd(fullFrame);
        uint16_t module = cmdModule(cmd);

        // 查路由表
        auto it = moduleRoutes_.find(module);
        if (it == moduleRoutes_.end()) {
            badFrames.fetch_add(1);
            std::cerr << "[Gateway] no route for cmd=" << cmd
                      << " module=" << module << std::endl;
            return;
        }
        uint32_t targetActorId = it->second;

        // 把完整帧装成 NetworkRecv 消息转发给目标 Actor
        // 注意：保留 fd，让业务 Actor 知道回包给谁
        ActorMessage forward(MsgType::NetworkRecv, GetActorId(), fd, std::move(fullFrame));
        GetSystem()->Send(targetActorId, std::move(forward));

        framesRouted.fetch_add(1);
        // 注意：unordered_map<uint16_t, atomic<int>> 用 [] 在新 key 时会值初始化为 0，OK
        routedByModule_[module].fetch_add(1);
    }
};

// ============================================================
//  Server 启动
// ============================================================

struct ServerContext {
    EventLoop mainLoop;
    EventLoopThreadPool pool;
    ActorSystem actorSys;
    Acceptor* acceptor = nullptr;

    GatewayActor* gateway = nullptr;
    SceneActor*   scene   = nullptr;
    ChatActor*    chat    = nullptr;

    uint32_t gatewayId = 0;
    uint32_t sceneId = 0;
    uint32_t chatId = 0;
};

void startServer(ServerContext& ctx) {
    std::cout << "========================================" << std::endl;
    std::cout << "  Multi-Reactor + Gateway Routing Test" << std::endl;
    std::cout << "  SubReactor=" << SUB_REACTOR_NUM
              << ", Worker=" << WORKER_NUM
              << ", Port=" << TEST_PORT << std::endl;
    std::cout << "========================================" << std::endl;

    // 1. 主 EventLoop
    ctx.mainLoop.Create();

    // 2. SubReactor 池
    ctx.pool.SetThreadNum(SUB_REACTOR_NUM);
    ctx.pool.Start(&ctx.actorSys);

    // 3. ActorSystem
    ctx.actorSys.Start(WORKER_NUM, &ctx.mainLoop);
    ctx.mainLoop.SetActorSystem(&ctx.actorSys);

    // 4. 注册业务 Actor
    auto scene = std::make_unique<SceneActor>();
    ctx.scene = scene.get();
    ctx.sceneId = ctx.actorSys.RegisterActor(std::move(scene));

    auto chat = std::make_unique<ChatActor>();
    ctx.chat = chat.get();
    ctx.chatId = ctx.actorSys.RegisterActor(std::move(chat));

    // 5. 注册 Gateway，注册模块路由表
    auto gateway = std::make_unique<GatewayActor>();
    ctx.gateway = gateway.get();
    ctx.gatewayId = ctx.actorSys.RegisterActor(std::move(gateway));

    // 注册业务模块到 Gateway 的路由表
    // 新增业务模块只需在此处加一行 RegisterModule，无需修改 GatewayActor 代码
    ctx.gateway->RegisterModule(1, ctx.sceneId);  // 场景模块 (cmd 1000~1999)
    ctx.gateway->RegisterModule(2, ctx.chatId);   // 聊天模块 (cmd 2000~2999)
    // 后续可继续加：
    // ctx.gateway->RegisterModule(3, battleActorId);  // 战斗模块 (cmd 3000~3999)
    // ctx.gateway->RegisterModule(4, friendActorId);  // 好友模块 (cmd 4000~4999)

    std::cout << "[Server] actors: Gateway=" << ctx.gatewayId
              << ", Scene=" << ctx.sceneId
              << ", Chat=" << ctx.chatId << std::endl;

    // 6. Acceptor：所有新连接通过 Gateway 接管
    ctx.acceptor = new Acceptor(TEST_PORT, &ctx.mainLoop, ctx.gatewayId);
    ctx.acceptor->SetThreadPool(&ctx.pool);

    std::cout << "[Server] ready, waiting clients..." << std::endl;
}

// ============================================================
//  客户端 — 混合发送场景 + 聊天消息
// ============================================================

struct ClientStat {
    int clientId = 0;
    int sceneSent = 0;
    int sceneRecv = 0;
    int chatSent = 0;
    int chatRecv = 0;
    bool ok = false;
    std::string err;
};

static bool readN(int fd, char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd, buf + got, n - got, 0);
        if (r > 0) got += r;
        else if (r == 0) return false;
        else if (errno == EINTR) continue;
        else return false;
    }
    return true;
}

static bool writeN(int fd, const char* buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = ::send(fd, buf + sent, n - sent, 0);
        if (w > 0) sent += w;
        else if (w == 0) return false;
        else if (errno == EINTR) continue;
        else return false;
    }
    return true;
}

// 发一帧，等回包（同 cmd），校验内容
static bool sendRecvFrame(int fd, uint16_t cmd, const std::string& payload) {
    std::string frame = encodeFrame(cmd, payload);
    if (!writeN(fd, frame.data(), frame.size())) return false;

    // 收一帧：先 4 字节 len，再 bodyLen 字节 body
    char header[4];
    if (!readN(fd, header, 4)) return false;
    uint32_t netLen;
    std::memcpy(&netLen, header, 4);
    uint32_t bodyLen = ntohl(netLen);
    if (bodyLen < 2 || bodyLen > 64 * 1024) return false;  // 协议异常

    std::string body(bodyLen, '\0');
    if (!readN(fd, &body[0], bodyLen)) return false;

    // 校验：body 的前 2 字节是 cmd（网络序）
    uint16_t recvCmd;
    std::memcpy(&recvCmd, body.data(), 2);
    recvCmd = ntohs(recvCmd);
    if (recvCmd != cmd) return false;

    // payload 是否一致
    std::string recvPayload(body.data() + 2, bodyLen - 2);
    return recvPayload == payload;
}

void clientThread(int clientId, ClientStat& stat) {
    stat.clientId = clientId;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { stat.err = "socket"; return; }

    sockaddr_in svrAddr{};
    svrAddr.sin_family = AF_INET;
    svrAddr.sin_port = htons(TEST_PORT);
    inet_pton(AF_INET, "127.0.0.1", &svrAddr.sin_addr);

    if (::connect(fd, (sockaddr*)&svrAddr, sizeof(svrAddr)) < 0) {
        stat.err = "connect errno=" + std::to_string(errno);
        ::close(fd); return;
    }

    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 三种场景命令循环用，三种聊天命令循环用
    uint16_t sceneCmds[] = { SCENE_MOVE, SCENE_PICK_ITEM, SCENE_USE_SKILL };
    uint16_t chatCmds[]  = { CHAT_WORLD, CHAT_GUILD, CHAT_PRIVATE };

    // 阶段 1: 场景消息
    for (int i = 0; i < SCENE_MSG_PER_CLIENT; ++i) {
        uint16_t cmd = sceneCmds[i % 3];
        std::ostringstream oss;
        oss << "scene_c" << clientId << "_i" << i << "_cmd" << cmd;
        if (!sendRecvFrame(fd, cmd, oss.str())) {
            stat.err = "scene fail at #" + std::to_string(i);
            ::close(fd); return;
        }
        stat.sceneSent++; stat.sceneRecv++;
    }

    // 阶段 2: 聊天消息
    for (int i = 0; i < CHAT_MSG_PER_CLIENT; ++i) {
        uint16_t cmd = chatCmds[i % 3];
        std::ostringstream oss;
        oss << "chat_c" << clientId << "_i" << i << "_cmd" << cmd;
        if (!sendRecvFrame(fd, cmd, oss.str())) {
            stat.err = "chat fail at #" + std::to_string(i);
            ::close(fd); return;
        }
        stat.chatSent++; stat.chatRecv++;
    }

    // 阶段 3: 场景 / 聊天混合交叉发送（模拟真实游戏场景）
    for (int i = 0; i < 100; ++i) {
        uint16_t cmd = (i % 2 == 0) ? sceneCmds[i % 3] : chatCmds[i % 3];
        std::ostringstream oss;
        oss << "mix_c" << clientId << "_i" << i;
        if (!sendRecvFrame(fd, cmd, oss.str())) {
            stat.err = "mix fail at #" + std::to_string(i);
            ::close(fd); return;
        }
        if (cmdModule(cmd) == 1) { stat.sceneSent++; stat.sceneRecv++; }
        else                        { stat.chatSent++;  stat.chatRecv++;  }
    }

    stat.ok = true;
    ::close(fd);

    std::cout << "[Client " << clientId << "] done"
              << " scene=" << stat.sceneRecv << "/" << stat.sceneSent
              << " chat=" << stat.chatRecv << "/" << stat.chatSent
              << " ok=Y" << std::endl;
}

// ============================================================
//  main
// ============================================================

int main() {
    ServerContext ctx;
    startServer(ctx);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::cout << "========================================" << std::endl;
    std::cout << "  Launching " << CLIENT_NUM << " clients" << std::endl;
    std::cout << "  Each: scene=" << SCENE_MSG_PER_CLIENT
              << ", chat=" << CHAT_MSG_PER_CLIENT << ", mix=100" << std::endl;
    std::cout << "========================================" << std::endl;

    auto t0 = steady_clock::now();

    std::vector<std::thread> threads;
    std::vector<ClientStat> stats(CLIENT_NUM);
    for (int i = 0; i < CLIENT_NUM; ++i) {
        threads.emplace_back(clientThread, i, std::ref(stats[i]));
    }
    for (auto& t : threads) t.join();

    auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0).count();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 汇总
    int okCount = 0;
    int totalSceneS = 0, totalSceneR = 0;
    int totalChatS = 0, totalChatR = 0;
    for (auto& s : stats) {
        if (s.ok) okCount++;
        totalSceneS += s.sceneSent; totalSceneR += s.sceneRecv;
        totalChatS  += s.chatSent;  totalChatR  += s.chatRecv;
    }

    int expectedScene = totalSceneS;  // 客户端发了多少应该收到多少
    int expectedChat  = totalChatS;

    std::cout << "========================================" << std::endl;
    std::cout << "  RESULTS" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  SubReactors        : " << SUB_REACTOR_NUM << std::endl;
    std::cout << "  Workers            : " << WORKER_NUM << std::endl;
    std::cout << "  Clients            : " << CLIENT_NUM << std::endl;
    std::cout << "  ---------------------------------------" << std::endl;
    std::cout << "  Client scene sent/recv : " << totalSceneS << "/" << totalSceneR << std::endl;
    std::cout << "  Client chat  sent/recv : " << totalChatS  << "/" << totalChatR  << std::endl;
    std::cout << "  ---------------------------------------" << std::endl;
    std::cout << "  Gateway routed total   : " << ctx.gateway->framesRouted.load()
              << " (bad=" << ctx.gateway->badFrames.load() << ")" << std::endl;
    // 按模块打印路由统计
    for (auto& [module, cnt] : ctx.gateway->routedByModule_) {
        std::cout << "    module " << module << " routed=" << cnt.load() << std::endl;
    }
    std::cout << "  ---------------------------------------" << std::endl;
    std::cout << "  SceneActor frames recv/sent: "
              << ctx.scene->metrics.framesRecv.load() << "/"
              << ctx.scene->metrics.framesSent.load()
              << "  bytes="
              << ctx.scene->metrics.bytesRecv.load() << std::endl;
    std::cout << "    MOVE="   << ctx.scene->metrics.cnt[0].load()
              << " PICK="      << ctx.scene->metrics.cnt[1].load()
              << " SKILL="     << ctx.scene->metrics.cnt[2].load() << std::endl;
    std::cout << "  ChatActor frames recv/sent : "
              << ctx.chat->metrics.framesRecv.load() << "/"
              << ctx.chat->metrics.framesSent.load()
              << "  bytes="
              << ctx.chat->metrics.bytesRecv.load() << std::endl;
    std::cout << "    WORLD="  << ctx.chat->metrics.cnt[0].load()
              << " GUILD="     << ctx.chat->metrics.cnt[1].load()
              << " PRIVATE="   << ctx.chat->metrics.cnt[2].load() << std::endl;
    std::cout << "  ---------------------------------------" << std::endl;
    std::cout << "  Clients OK         : " << okCount << "/" << CLIENT_NUM << std::endl;
    std::cout << "  Elapsed            : " << elapsed << " ms" << std::endl;

    int totalFrames = ctx.scene->metrics.framesRecv.load() + ctx.chat->metrics.framesRecv.load();
    if (elapsed > 0) {
        std::cout << "  Throughput (frame/s): " << (totalFrames * 1000LL / elapsed) << std::endl;
    }

    // Pass 条件：
    //  1. 所有客户端 OK
    //  2. Gateway 路由帧数 == Scene + Chat 收到的帧数
    //  3. 业务 Actor 收到的帧数 == 客户端发送的对应类型帧数
    bool pass = (okCount == CLIENT_NUM)
              && (ctx.scene->metrics.framesRecv.load() == expectedScene)
              && (ctx.chat->metrics.framesRecv.load() == expectedChat)
              && (ctx.gateway->badFrames.load() == 0);

    std::cout << "========================================" << std::endl;
    if (pass) {
        std::cout << "  >>> TEST PASSED <<<" << std::endl;
    } else {
        std::cout << "  >>> TEST FAILED <<<" << std::endl;
        for (auto& s : stats) {
            if (!s.ok) {
                std::cout << "    Client " << s.clientId << " err: " << s.err << std::endl;
            }
        }
    }
    std::cout << "========================================" << std::endl;

    ctx.pool.Stop();
    ctx.mainLoop.Stop();

    return pass ? 0 : 1;
}
