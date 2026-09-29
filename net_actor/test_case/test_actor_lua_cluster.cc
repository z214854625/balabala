/**
@file: test_actor_lua_cluster.cc
@brief: Phase 4 — 跨进程 Lua actor.cluster_call 验证

链路（单进程双节点 + LoopbackTransport，cmsgpack 贯通）：
  nodeA.lua_caller ── ClusterCall("nodeB", "lua_service", req) ──> nodeB.lua_service
    C++ ClusterCallAwaiter 把 packed 请求经 transportB 路由给 nodeB 的 ClusterReceiver
    nodeB.lua_service.OnMessage(query, ..., is_remote=true, src_node="nodeA", src_name="lua_caller")
      → actor.respond_remote(src_node, src_name, sid, {result="processed:"..data})
    nodeA.lua_caller 的 actor.cluster_call 从 yield 处返回响应
    → 验证 resp.result == "processed:"..data → test_record("ok")

通过标准：g_okCount == N（默认 5）
注意：调用方 Actor 必须已 RegisterName，否则远端无法回程路由。
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

static const int kRounds = 5;
static std::atomic<int> g_okCount{0};
static std::atomic<int> g_badRespCount{0};
static std::atomic<int> g_errCount{0};

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << "  LuaActor Phase 4 — Cluster Call Test" << std::endl;
    std::cout << "  (Lua cluster_call ↔ C++ co_await ClusterCall)" << std::endl;
    std::cout << "======================================" << std::endl;

    // ===== nodeB（服务端） =====
    EventLoop loopB; loopB.Create();
    ActorSystem sysB; sysB.Start(2, &loopB); loopB.SetActorSystem(&sysB);
    LoopbackTransport transportB;
    transportB.SetLocalNodeId("nodeB");
    sysB.RegisterTransport(&transportB);

    auto service = std::make_unique<LuaActor>("cluster_service");
    LuaActor* servicePtr = service.get();
    uint32_t serviceId = sysB.RegisterActor(
        std::unique_ptr<Actor>(std::move(service)));
    sysB.RegisterName("lua_service", serviceId);

    // ===== nodeA（调用端） =====
    EventLoop loopA; loopA.Create();
    ActorSystem sysA; sysA.Start(2, &loopA); loopA.SetActorSystem(&sysA);
    LoopbackTransport transportA;
    transportA.SetLocalNodeId("nodeA");
    sysA.RegisterTransport(&transportA);

    auto caller = std::make_unique<LuaActor>("cluster_caller");
    LuaActor* callerPtr = caller.get();
    uint32_t callerId = sysA.RegisterActor(
        std::unique_ptr<Actor>(std::move(caller)));
    // ClusterCall 要求调用方注册名字（远端按名字回程路由）
    sysA.RegisterName("lua_caller", callerId);

    // ===== LoopbackTransport 双向连通 =====
    ClusterReceiver receiverA(&sysA);
    ClusterReceiver receiverB(&sysB);
    transportA.RegisterRemoteHandler("nodeB",
        [&receiverB](const ClusterPacket& p) { receiverB.OnPacketReceived(p); });
    transportB.RegisterRemoteHandler("nodeA",
        [&receiverA](const ClusterPacket& p) { receiverA.OnPacketReceived(p); });

    // 注册 test_record（仅 caller 用得到）
    callerPtr->Lua().set_function("test_record",
        [](const std::string& cat) {
            if (cat == "ok") g_okCount.fetch_add(1);
            else if (cat == "bad_resp") g_badRespCount.fetch_add(1);
            else if (cat == "err") g_errCount.fetch_add(1);
        });

    std::cout << "[Setup] nodeA.caller=" << callerId
              << " (name=lua_caller)  nodeB.service=" << serviceId
              << " (name=lua_service)  rounds=" << kRounds << std::endl;

    // 用 caller 的 Lua 状态预打包 kRounds 个 trigger 请求
    for (int i = 0; i < kRounds; ++i) {
        sol::table trigger = callerPtr->Lua().create_table();
        trigger["cmd"] = "trigger";
        trigger["data"] = std::string("hi") + std::to_string(i);
        std::string packed = callerPtr->Lua()["cmsgpack"]["pack"](trigger);
        sysA.Send(callerId, ActorMessage::Make(MsgType::UserMessage, 0, 0,
                                                packed.data(), packed.size()));
    }

    // 轮询 okCount
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (g_okCount.load() < kRounds &&
           std::chrono::steady_clock::now() < deadline) {
        usleep(10000);
    }

    int ok = g_okCount.load();
    int bad = g_badRespCount.load();
    int err = g_errCount.load();
    bool pass = (ok == kRounds && bad == 0 && err == 0);

    std::cout << "[Verify] ok=" << ok << "/" << kRounds
              << " bad_resp=" << bad << " err=" << err << std::endl;

    sysA.Stop();
    sysB.Stop();

    std::cout << "======================================" << std::endl;
    std::cout << (pass ? "  [PASS] Phase 4 cluster_call roundtrip OK!"
                       : "  [FAIL] Phase 4 test failed!")
              << std::endl;
    std::cout << "======================================" << std::endl;

    return pass ? 0 : 1;
}
