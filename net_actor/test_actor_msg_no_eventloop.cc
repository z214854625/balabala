/**
 * @file test_actor_msg.cc
 * @brief Actor间互相发送消息的测试用例
 * 
 * 测试场景：
 *   Test1 - PingPong: 两个Actor互相发消息，来回 N 轮
 *   Test2 - Chain:    A → B → C 链式转发
 *   Test3 - Broadcast: 一个Actor广播消息给多个Actor
 * 
 * 编译方式见 Makefile.test / run_test.sh
 */

#include "precompiled.h"
#include "Actor.h"
#include "ActorSystem.h"
#include "Message.h"

#include <cassert>
#include <chrono>
#include <sstream>

using namespace bllsll;

// ============================================================
//  辅助：解析 "ping:3" / "pong:3" 中的数字
// ============================================================
static int ParseRound(const std::string& data)
{
    auto pos = data.find(':');
    if (pos == std::string::npos) return 0;
    return std::stoi(data.substr(pos + 1));
}

// ============================================================
//  Test1: PingPong —— 两个Actor互相发消息
// ============================================================

static const int PING_PONG_ROUNDS = 10;

class PingActor : public Actor
{
public:
    uint32_t pongActorId = 0;        // 对端Actor ID
    std::atomic<int> recvCount{0};   // 收到 pong 的次数
    std::atomic<bool> done{false};   // 是否完成

    void OnMessage(ActorMessage& msg) override
    {
        if (msg.type != MsgType::UserMessage) return;

        if (msg.data == "start") {
            // 发起第一轮 ping
            SendToActor(pongActorId, ActorMessage{MsgType::UserMessage, 0, -1, "ping:1"});
        } else if (msg.data.substr(0, 4) == "pong") {
            int round = ParseRound(msg.data);
            recvCount.fetch_add(1);
            if (round < PING_PONG_ROUNDS) {
                // 继续下一轮
                std::string next = "ping:" + std::to_string(round + 1);
                SendToActor(pongActorId, ActorMessage{MsgType::UserMessage, 0, -1, std::move(next)});
            } else {
                done.store(true);
            }
        }
    }
};

class PongActor : public Actor
{
public:
    std::atomic<int> recvCount{0};

    void OnMessage(ActorMessage& msg) override
    {
        if (msg.type != MsgType::UserMessage) return;

        if (msg.data.substr(0, 4) == "ping") {
            recvCount.fetch_add(1);
            int round = ParseRound(msg.data);
            std::string reply = "pong:" + std::to_string(round);
            // 回复给来源 Actor（sourceId 在 SendToActor 中自动设置）
            SendToActor(msg.sourceId, ActorMessage{MsgType::UserMessage, 0, -1, std::move(reply)});
        }
    }
};

bool TestPingPong()
{
    std::cout << "\n========== Test1: PingPong ==========" << std::endl;

    ActorSystem sys;
    sys.Start(2, nullptr);  // 2个工作线程，无需 EventLoop

    auto pingPtr = std::make_unique<PingActor>();
    auto pongPtr = std::make_unique<PongActor>();
    PingActor* ping = pingPtr.get();
    PongActor* pong = pongPtr.get();

    uint32_t pingId = sys.RegisterActor(std::move(pingPtr));
    uint32_t pongId = sys.RegisterActor(std::move(pongPtr));

    ping->pongActorId = pongId;

    // 发送 start 消息触发 PingPong
    sys.Send(pingId, ActorMessage{MsgType::UserMessage, 0, -1, "start"});

    // 等待完成（最多 5 秒超时）
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ping->done.load() && std::chrono::steady_clock::now() < deadline) {
        usleep(1000);  // 1ms
    }

    bool ok = true;
    if (!ping->done.load()) {
        std::cout << "[FAIL] PingPong timeout!" << std::endl;
        ok = false;
    }
    if (ping->recvCount.load() != PING_PONG_ROUNDS) {
        std::cout << "[FAIL] PingActor recvCount=" << ping->recvCount.load()
                  << ", expected=" << PING_PONG_ROUNDS << std::endl;
        ok = false;
    }
    if (pong->recvCount.load() != PING_PONG_ROUNDS) {
        std::cout << "[FAIL] PongActor recvCount=" << pong->recvCount.load()
                  << ", expected=" << PING_PONG_ROUNDS << std::endl;
        ok = false;
    }

    sys.Stop();

    if (ok) {
        std::cout << "[PASS] PingPong: " << PING_PONG_ROUNDS
                  << " rounds completed. PingRecv=" << ping->recvCount.load()
                  << ", PongRecv=" << pong->recvCount.load() << std::endl;
    }
    return ok;
}

// ============================================================
//  Test2: Chain —— A → B → C 链式转发
// ============================================================

static const int CHAIN_MSG_COUNT = 20;

class ForwarderActor : public Actor
{
public:
    uint32_t nextActorId = 0;        // 下游 Actor ID
    std::atomic<int> recvCount{0};

    void OnMessage(ActorMessage& msg) override
    {
        if (msg.type != MsgType::UserMessage) return;
        recvCount.fetch_add(1);
        // 转发给下游，保留原始数据
        if (nextActorId > 0) {
            SendToActor(nextActorId, ActorMessage{MsgType::UserMessage, 0, -1, msg.data});
        }
    }
};

class CollectorActor : public Actor
{
public:
    std::atomic<int> recvCount{0};
    std::atomic<bool> done{false};
    int expectCount = 0;

    void OnMessage(ActorMessage& msg) override
    {
        if (msg.type != MsgType::UserMessage) return;
        int cnt = recvCount.fetch_add(1) + 1;
        if (cnt >= expectCount) {
            done.store(true);
        }
    }
};

bool TestChain()
{
    std::cout << "\n========== Test2: Chain (A → B → C) ==========" << std::endl;

    ActorSystem sys;
    sys.Start(3, nullptr);

    auto aPtr = std::make_unique<ForwarderActor>();
    auto bPtr = std::make_unique<ForwarderActor>();
    auto cPtr = std::make_unique<CollectorActor>();
    ForwarderActor* a = aPtr.get();
    ForwarderActor* b = bPtr.get();
    CollectorActor* c = cPtr.get();
    c->expectCount = CHAIN_MSG_COUNT;

    uint32_t aId = sys.RegisterActor(std::move(aPtr));
    uint32_t bId = sys.RegisterActor(std::move(bPtr));
    uint32_t cId = sys.RegisterActor(std::move(cPtr));

    a->nextActorId = bId;
    b->nextActorId = cId;

    // 向 A 发送 CHAIN_MSG_COUNT 条消息
    for (int i = 0; i < CHAIN_MSG_COUNT; ++i) {
        std::string data = "chain_msg:" + std::to_string(i);
        sys.Send(aId, ActorMessage{MsgType::UserMessage, 0, -1, std::move(data)});
    }

    // 等待 C 收齐所有消息
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!c->done.load() && std::chrono::steady_clock::now() < deadline) {
        usleep(1000);
    }

    bool ok = true;
    if (a->recvCount.load() != CHAIN_MSG_COUNT) {
        std::cout << "[FAIL] ForwarderA recvCount=" << a->recvCount.load()
                  << ", expected=" << CHAIN_MSG_COUNT << std::endl;
        ok = false;
    }
    if (b->recvCount.load() != CHAIN_MSG_COUNT) {
        std::cout << "[FAIL] ForwarderB recvCount=" << b->recvCount.load()
                  << ", expected=" << CHAIN_MSG_COUNT << std::endl;
        ok = false;
    }
    if (c->recvCount.load() != CHAIN_MSG_COUNT) {
        std::cout << "[FAIL] CollectorC recvCount=" << c->recvCount.load()
                  << ", expected=" << CHAIN_MSG_COUNT << std::endl;
        ok = false;
    }

    sys.Stop();

    if (ok) {
        std::cout << "[PASS] Chain: " << CHAIN_MSG_COUNT
                  << " messages forwarded through A(" << a->recvCount.load()
                  << ") → B(" << b->recvCount.load()
                  << ") → C(" << c->recvCount.load() << ")" << std::endl;
    }
    return ok;
}

// ============================================================
//  Test3: Broadcast —— 一个Actor广播给多个Actor
// ============================================================

static const int BROADCAST_RECEIVERS = 5;
static const int BROADCAST_MSG_COUNT = 10;

class BroadcasterActor : public Actor
{
public:
    std::vector<uint32_t> receiverIds;
    std::atomic<int> sendCount{0};

    void OnMessage(ActorMessage& msg) override
    {
        if (msg.type != MsgType::UserMessage && msg.data != "broadcast_start") return;

        // 收到触发消息后，向所有 receiver 广播 BROADCAST_MSG_COUNT 条消息
        for (int i = 0; i < BROADCAST_MSG_COUNT; ++i) {
            for (uint32_t rid : receiverIds) {
                std::string data = "broadcast:" + std::to_string(i);
                SendToActor(rid, ActorMessage{MsgType::UserMessage, 0, -1, std::move(data)});
                sendCount.fetch_add(1);
            }
        }
    }
};

class ReceiverActor : public Actor
{
public:
    std::atomic<int> recvCount{0};

    void OnMessage(ActorMessage& msg) override
    {
        if (msg.type != MsgType::UserMessage) return;
        recvCount.fetch_add(1);
    }
};

bool TestBroadcast()
{
    std::cout << "\n========== Test3: Broadcast (1 → N) ==========" << std::endl;

    ActorSystem sys;
    sys.Start(4, nullptr);

    auto broadcasterPtr = std::make_unique<BroadcasterActor>();
    BroadcasterActor* broadcaster = broadcasterPtr.get();

    std::vector<ReceiverActor*> receivers;
    std::vector<uint32_t> receiverIds;

    uint32_t broadcasterId = sys.RegisterActor(std::move(broadcasterPtr));

    for (int i = 0; i < BROADCAST_RECEIVERS; ++i) {
        auto rPtr = std::make_unique<ReceiverActor>();
        receivers.push_back(rPtr.get());
        uint32_t rid = sys.RegisterActor(std::move(rPtr));
        receiverIds.push_back(rid);
    }
    broadcaster->receiverIds = receiverIds;

    // 触发广播
    sys.Send(broadcasterId, ActorMessage{MsgType::UserMessage, 0, -1, "broadcast_start"});

    // 等待所有 receiver 收齐消息
    int totalExpected = BROADCAST_MSG_COUNT;  // 每个 receiver 期望收到的条数
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool allDone = false;
    while (!allDone && std::chrono::steady_clock::now() < deadline) {
        allDone = true;
        for (auto* r : receivers) {
            if (r->recvCount.load() < totalExpected) {
                allDone = false;
                break;
            }
        }
        if (!allDone) usleep(1000);
    }

    bool ok = true;
    int totalSent = broadcaster->sendCount.load();
    int expectedTotal = BROADCAST_MSG_COUNT * BROADCAST_RECEIVERS;
    if (totalSent != expectedTotal) {
        std::cout << "[FAIL] Broadcaster sendCount=" << totalSent
                  << ", expected=" << expectedTotal << std::endl;
        ok = false;
    }

    for (int i = 0; i < BROADCAST_RECEIVERS; ++i) {
        if (receivers[i]->recvCount.load() != BROADCAST_MSG_COUNT) {
            std::cout << "[FAIL] Receiver[" << i << "] recvCount="
                      << receivers[i]->recvCount.load()
                      << ", expected=" << BROADCAST_MSG_COUNT << std::endl;
            ok = false;
        }
    }

    sys.Stop();

    if (ok) {
        std::cout << "[PASS] Broadcast: " << BROADCAST_MSG_COUNT << " msgs × "
                  << BROADCAST_RECEIVERS << " receivers = " << expectedTotal
                  << " total delivered." << std::endl;
    }
    return ok;
}

// ============================================================
//  Test4: 高并发压力测试 —— 多个Actor同时互发大量消息
// ============================================================

static const int STRESS_ACTOR_COUNT = 8;
static const int STRESS_MSG_PER_PAIR = 100;

class StressActor : public Actor
{
public:
    std::atomic<int> recvCount{0};
    std::vector<uint32_t> peerIds;
    std::atomic<bool> startSending{false};

    void OnMessage(ActorMessage& msg) override
    {
        if (msg.type != MsgType::UserMessage) return;

        if (msg.data == "go") {
            // 收到 go 信号后，向所有 peer 发送消息
            for (uint32_t pid : peerIds) {
                for (int i = 0; i < STRESS_MSG_PER_PAIR; ++i) {
                    SendToActor(pid, ActorMessage{MsgType::UserMessage, 0, -1, "stress"});
                }
            }
        } else {
            recvCount.fetch_add(1);
        }
    }
};

bool TestStress()
{
    std::cout << "\n========== Test4: Stress (N actors × M msgs) ==========" << std::endl;

    ActorSystem sys;
    sys.Start(4, nullptr);

    std::vector<StressActor*> actors;
    std::vector<uint32_t> actorIds;

    for (int i = 0; i < STRESS_ACTOR_COUNT; ++i) {
        auto ptr = std::make_unique<StressActor>();
        actors.push_back(ptr.get());
        uint32_t id = sys.RegisterActor(std::move(ptr));
        actorIds.push_back(id);
    }

    // 每个 actor 的 peer 是除自己外的所有 actor
    for (int i = 0; i < STRESS_ACTOR_COUNT; ++i) {
        for (int j = 0; j < STRESS_ACTOR_COUNT; ++j) {
            if (i != j) {
                actors[i]->peerIds.push_back(actorIds[j]);
            }
        }
    }

    // 同时向所有 actor 发送 go 信号
    for (uint32_t id : actorIds) {
        sys.Send(id, ActorMessage{MsgType::UserMessage, 0, -1, "go"});
    }

    // 每个 actor 会收到来自其他 (N-1) 个 actor 各 M 条消息
    int expectedPerActor = (STRESS_ACTOR_COUNT - 1) * STRESS_MSG_PER_PAIR;
    int totalExpected = STRESS_ACTOR_COUNT * expectedPerActor;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool allDone = false;
    while (!allDone && std::chrono::steady_clock::now() < deadline) {
        allDone = true;
        for (auto* a : actors) {
            if (a->recvCount.load() < expectedPerActor) {
                allDone = false;
                break;
            }
        }
        if (!allDone) usleep(1000);
    }

    bool ok = true;
    int totalRecv = 0;
    for (int i = 0; i < STRESS_ACTOR_COUNT; ++i) {
        int cnt = actors[i]->recvCount.load();
        totalRecv += cnt;
        if (cnt != expectedPerActor) {
            std::cout << "[FAIL] StressActor[" << i << "] recvCount=" << cnt
                      << ", expected=" << expectedPerActor << std::endl;
            ok = false;
        }
    }

    sys.Stop();

    if (ok) {
        std::cout << "[PASS] Stress: " << STRESS_ACTOR_COUNT << " actors, "
                  << STRESS_MSG_PER_PAIR << " msgs/pair, total=" << totalRecv
                  << " (expected=" << totalExpected << ")" << std::endl;
    }
    return ok;
}

// ============================================================
//  main
// ============================================================

int main()
{
    std::cout << "======================================" << std::endl;
    std::cout << "  Actor Inter-Message Test Suite" << std::endl;
    std::cout << "======================================" << std::endl;

    int passed = 0;
    int failed = 0;

    if (TestPingPong()) ++passed; else ++failed;
    if (TestChain())    ++passed; else ++failed;
    if (TestBroadcast())++passed; else ++failed;
    if (TestStress())   ++passed; else ++failed;

    std::cout << "\n======================================" << std::endl;
    std::cout << "  Result: " << passed << " passed, " << failed << " failed" << std::endl;
    std::cout << "======================================" << std::endl;

    return (failed > 0) ? 1 : 0;
}
