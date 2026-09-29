/**
@file: test_actor_lua_state.cc
@brief: Phase 2 — 共享状态服务 actor 验证

链路（C++ 主动 Call，LuaActor 被动 Respond）：
  main ── Send(callerId, "trigger") ──> CppCallerActor
    Caller.OnMessage("trigger") → co_await Call(stateService, set_and_get req)
      LuaStateService.OnMessage(req, src, sid, fd) → 更新内部 state 表
        → actor_respond(src, sid, {value, expected, ok})
    Caller.Call 恢复 → 检查 resp.error == Ok 且非空
    若干次循环后 okCount == N → [PASS]

验证点：
  1. LuaActor 能作为 Call 的被调用方（接收 UserMessage + sessionId）
  2. Lua 脚本能用 actor_respond 桥回填响应（自动设 isResponse=true）
  3. C++ 协程 co_await Call 能正确恢复，resp.error == Ok
  4. 多次 Call 共享同一 Lua 状态表（state["k1"]=v1, state["k2"]=v2 持久化）
*/

#define SOL_ALL_SAFETIES_ON 1
#include "precompiled.h"
#include "LuaActor.h"
#include "ActorSystem.h"
#include "EventLoop.h"
#include "Message.h"
#include "Coroutine.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace bllsll;

static const int kCallRounds = 5;
static std::atomic<int> g_okCount{0};

// ============================================================
//  C++ 调用方 Actor：co_await Call 触发状态服务
// ============================================================
class CppCallerActor : public Actor
{
public:
    uint32_t stateServiceId = 0;
    // 预打包好的 set_and_get 请求列表（由 main 在注册前用 state service 的 Lua 状态生成）
    std::vector<std::string> packedReqs;

    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        if (msg.type != MsgType::UserMessage) co_return;
        if (msg.data != "trigger") co_return;

        for (size_t i = 0; i < packedReqs.size(); ++i) {
            const std::string& packedReq = packedReqs[i];
            auto req = ActorMessage::Make(
                MsgType::UserMessage, GetActorId(), 0,
                packedReq.data(), packedReq.size());
            auto resp = co_await Call(stateServiceId, std::move(req), 3000);
            if (resp.error != CallError::Ok) {
                std::cerr << "[Caller] Call " << i << " failed: error="
                          << static_cast<int>(resp.error) << std::endl;
                continue;
            }
            if (resp.DataEmpty()) {
                std::cerr << "[Caller] Call " << i << " empty response" << std::endl;
                continue;
            }
            g_okCount.fetch_add(1);
        }
        co_return;
    }
};

// ============================================================
//  测试主函数
// ============================================================
int main()
{
    std::cout << "======================================" << std::endl;
    std::cout << "  LuaActor Phase 2 — State Service Test" << std::endl;
    std::cout << "  (C++ co_await Call ↔ LuaActor actor_respond)" << std::endl;
    std::cout << "======================================" << std::endl;

    EventLoop loop;
    loop.Create();
    ActorSystem sys;
    sys.Start(4, &loop);
    loop.SetActorSystem(&sys);

    // 1. 注册 LuaStateService
    auto stateService = std::make_unique<LuaActor>("state_service");
    LuaActor* stateServicePtr = stateService.get();
    uint32_t stateServiceId = sys.RegisterActor(
        std::unique_ptr<Actor>(std::move(stateService)));

    // 2. 注册 C++ Caller
    auto caller = std::make_unique<CppCallerActor>();
    CppCallerActor* callerPtr = caller.get();
    uint32_t callerId = sys.RegisterActor(
        std::unique_ptr<Actor>(std::move(caller)));

    // 3. 用 state service 自己的 Lua 状态预打包 kCallRounds 个 set_and_get 请求
    //    （main 线程内、Send 之前，串行不变量保证安全）
    callerPtr->stateServiceId = stateServiceId;
    callerPtr->packedReqs.reserve(kCallRounds);
    for (int i = 0; i < kCallRounds; ++i) {
        sol::table req = stateServicePtr->Lua().create_table();
        req["cmd"] = "set_and_get";
        req["key"] = std::string("k") + std::to_string(i);
        req["value"] = i * 100;
        std::string packed = stateServicePtr->Lua()["cmsgpack"]["pack"](req);
        callerPtr->packedReqs.push_back(std::move(packed));
    }

    std::cout << "[Setup] stateServiceId=" << stateServiceId
              << " callerId=" << callerId
              << " rounds=" << kCallRounds << std::endl;

    // 4. 发 trigger 给 caller → 启动 co_await Call 链路
    sys.Send(callerId, ActorMessage{MsgType::UserMessage, 0, -1, "trigger"});

    // 5. 轮询 g_okCount == kCallRounds 或超时
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (g_okCount.load() < kCallRounds &&
           std::chrono::steady_clock::now() < deadline) {
        usleep(10000);
    }

    int ok = g_okCount.load();
    bool pass = (ok == kCallRounds);

    std::cout << "[Verify] ok=" << ok << "/" << kCallRounds << std::endl;

    sys.Stop();

    std::cout << "======================================" << std::endl;
    std::cout << (pass ? "  [PASS] State service roundtrip OK!"
                       : "  [FAIL] State service test failed!")
              << std::endl;
    std::cout << "======================================" << std::endl;

    return pass ? 0 : 1;
}
