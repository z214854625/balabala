/**
@file: test_actor_lua_battle.cc
@brief: 跨服战斗业务示例 — A 服战斗发起前到 B 服异步检查并扣道具

业务场景:
  1. C++ main 触发 A 服 battle_service 启动战斗(携带 player_id/item_id/count)
  2. A 服 battle_service (Lua):
     - actor.run_co 包裹异步流程
     - actor.cluster_call("nodeB", "item_service", check_and_deduct)
  3. B 服 item_service (Lua):
     - actor.run_co 包裹 DB 异步操作
     - actor.call(dbService, get) 读道具数量(异步)
     - 不足: 返回 {ok=false, reason="insufficient"}
     - 充足: actor.call(dbService, set) 扣减(异步)
     - actor.respond_remote 回 A 服
  4. A 服 battle_service 拿到结果,继续战斗逻辑
     - 成功: test_record("ok")
     - 失败: test_record("fail")

测试用例:
  - Round 1: p1 有 10 瓶 potion,要 3 瓶 → ok (10→7)
  - Round 2: p1 剩 7 瓶,要 5 瓶 → ok (7→2)
  - Round 3: p1 剩 2 瓶,要 10 瓶 → fail (2<10,不扣)

验证点:
  - Lua 嵌套协程 (run_co) 内 cluster_call + call 混用
  - B 服 run_co 内多个 actor.call 串联
  - 跨进程响应路由 (respond_remote)
  - DB 状态跨多次调用持久化
*/

#define SOL_ALL_SAFETIES_ON 1
#include "precompiled.h"
#include "LuaActor.h"
#include "ActorSystem.h"
#include "EventLoop.h"
#include "ClusterProxy.h"
#include "Message.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace bllsll;

static std::atomic<int> g_okCount{0};
static std::atomic<int> g_failCount{0};

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << "  Cross-Server Battle Example" << std::endl;
    std::cout << "  (A: battle_service | B: item_service + db_service)" << std::endl;
    std::cout << "======================================" << std::endl;

    // ===== nodeB (道具服务端) =====
    EventLoop loopB; loopB.Create();
    ActorSystem sysB; sysB.Start(2, &loopB); loopB.SetActorSystem(&sysB);
    LoopbackTransport transportB;
    transportB.SetLocalNodeId("nodeB");
    sysB.RegisterTransport(&transportB);

    auto dbService = std::make_unique<LuaActor>("battle_db");
    LuaActor* dbServicePtr = dbService.get();
    uint32_t dbServiceId = sysB.RegisterActor(
        std::unique_ptr<Actor>(std::move(dbService)));

    auto itemService = std::make_unique<LuaActor>("battle_item");
    LuaActor* itemServicePtr = itemService.get();
    uint32_t itemServiceId = sysB.RegisterActor(
        std::unique_ptr<Actor>(std::move(itemService)));
    sysB.RegisterName("item_service", itemServiceId);
    // 把 dbServiceId 注入到 item_service 的 Lua 全局
    itemServicePtr->Lua()["dbServiceId"] = dbServiceId;

    // ===== nodeA (战斗服务端) =====
    EventLoop loopA; loopA.Create();
    ActorSystem sysA; sysA.Start(2, &loopA); loopA.SetActorSystem(&sysA);
    LoopbackTransport transportA;
    transportA.SetLocalNodeId("nodeA");
    sysA.RegisterTransport(&transportA);

    auto battleService = std::make_unique<LuaActor>("battle_service");
    LuaActor* battleServicePtr = battleService.get();
    uint32_t battleServiceId = sysA.RegisterActor(
        std::unique_ptr<Actor>(std::move(battleService)));
    sysA.RegisterName("battle_service", battleServiceId);  // ClusterCall 要求调用方注册名字
    battleServicePtr->Lua().set_function("test_record",
        [](const std::string& cat) {
            if (cat == "ok") g_okCount.fetch_add(1);
            else if (cat == "fail") g_failCount.fetch_add(1);
        });

    // ===== LoopbackTransport 双向连通 =====
    ClusterReceiver receiverA(&sysA);
    ClusterReceiver receiverB(&sysB);
    transportA.RegisterRemoteHandler("nodeB",
        [&receiverB](const ClusterPacket& p) { receiverB.OnPacketReceived(p); });
    transportB.RegisterRemoteHandler("nodeA",
        [&receiverA](const ClusterPacket& p) { receiverA.OnPacketReceived(p); });

    // ===== 预置 DB 数据: p1 有 10 瓶 potion =====
    {
        sol::table seed = dbServicePtr->Lua().create_table();
        seed["cmd"] = "set";
        seed["key"] = "p1:potion";
        seed["value"] = 10;
        std::string packed = dbServicePtr->Lua()["cmsgpack"]["pack"](seed);
        sysB.Send(dbServiceId, ActorMessage::Make(MsgType::UserMessage, 0, 0,
                                                  packed.data(), packed.size()));
    }

    usleep(50000);  // 等 DB 初始化完成

    std::cout << "[Setup] nodeA.battle=" << battleServiceId
              << "  nodeB.item=" << itemServiceId
              << "  nodeB.db=" << dbServiceId << std::endl;

    // ===== 串行触发 3 场战斗(避免并发读 DB 导致竞态) =====
    struct BattleReq { int count; const char* expect; };
    BattleReq rounds[] = {
        {3,  "ok"},   // 10 -> 7
        {5,  "ok"},   // 7  -> 2
        {10, "fail"}, // 2 < 10, reject
    };
    for (const auto& r : rounds) {
        int prevTotal = g_okCount.load() + g_failCount.load();
        sol::table trigger = battleServicePtr->Lua().create_table();
        trigger["cmd"] = "start_battle";
        trigger["player_id"] = std::string("p1");
        trigger["item_id"] = std::string("potion");
        trigger["count"] = r.count;
        std::string packed = battleServicePtr->Lua()["cmsgpack"]["pack"](trigger);
        sysA.Send(battleServiceId, ActorMessage::Make(MsgType::UserMessage, 0, 0,
                                                       packed.data(), packed.size()));
        // 等这一轮结束再发下一轮
        auto roundDl = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (g_okCount.load() + g_failCount.load() == prevTotal &&
               std::chrono::steady_clock::now() < roundDl) {
            usleep(5000);
        }
    }

    // ===== 轮询结果 =====
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (g_okCount.load() + g_failCount.load() < 3 &&
           std::chrono::steady_clock::now() < deadline) {
        usleep(10000);
    }

    int ok = g_okCount.load();
    int fail = g_failCount.load();
    bool pass = (ok == 2 && fail == 1);

    std::cout << "[Verify] ok=" << ok << " fail=" << fail << " (expect ok=2 fail=1)"
              << std::endl;

    sysA.Stop();
    sysB.Stop();

    std::cout << "======================================" << std::endl;
    std::cout << (pass ? "  [PASS] Cross-server battle example OK!"
                       : "  [FAIL] Battle example failed!")
              << std::endl;
    std::cout << "======================================" << std::endl;

    return pass ? 0 : 1;
}
