/**
@file: test_actor_lua_call.cc
@brief: Phase 3 — Lua 协程与 C++ 协程融合验证

链路（Lua actor.call 挂起 → C++ co_await Call → 响应回来 resume Lua）：
  main ── Send(callerId, {cmd="trigger", data="helloN"}) ──> Caller LuaActor
    Caller.OnMessage(trigger) → actor.call(serviceId, {cmd="query", data="helloN"})
      Lua coroutine.yield({action="wait", target, payload}) ← 控制返回 C++
      C++ 侧 OnCoroutineMessage 拿到 yield 信号
      C++ co_await Call(serviceId, payload)   ← worker 释放
      Service LuaActor.OnMessage(query) → actor_respond(src, sid, {result="processed:helloN"})
      C++ co_await Call 恢复 → 推响应 bytes 到 th 栈 → lua_resume
      Lua actor.call 从 yield 处返回响应（unpack 后为 table）
    Caller 验证 resp.result == "processed:helloN" → test_record("ok")

通过标准：g_okCount == N（默认 5）
*/

#define SOL_ALL_SAFETIES_ON 1
#include "precompiled.h"
#include "LuaActor.h"
#include "ActorSystem.h"
#include "EventLoop.h"
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
    std::cout << "  LuaActor Phase 3 — actor.call Yield Test" << std::endl;
    std::cout << "  (Lua coroutine.yield ↔ C++ co_await Call)" << std::endl;
    std::cout << "======================================" << std::endl;

    EventLoop loop;
    loop.Create();
    ActorSystem sys;
    sys.Start(4, &loop);
    loop.SetActorSystem(&sys);

    auto service = std::make_unique<LuaActor>("call_service");
    auto caller = std::make_unique<LuaActor>("call_caller");
    LuaActor* servicePtr = service.get();
    LuaActor* callerPtr = caller.get();

    uint32_t serviceId = sys.RegisterActor(
        std::unique_ptr<Actor>(std::move(service)));
    uint32_t callerId = sys.RegisterActor(
        std::unique_ptr<Actor>(std::move(caller)));

    // 注册 test_record（仅 caller 用得到，service 不调）
    callerPtr->Lua().set_function("test_record",
        [](const std::string& cat) {
            if (cat == "ok") g_okCount.fetch_add(1);
            else if (cat == "bad_resp") g_badRespCount.fetch_add(1);
            else if (cat == "err") g_errCount.fetch_add(1);
        });
    callerPtr->Lua()["serviceId"] = serviceId;

    std::cout << "[Setup] serviceId=" << serviceId
              << " callerId=" << callerId
              << " rounds=" << kRounds << std::endl;

    // 用 caller 的 Lua 状态预打包 kRounds 个 trigger 请求
    for (int i = 0; i < kRounds; ++i) {
        sol::table trigger = callerPtr->Lua().create_table();
        trigger["cmd"] = "trigger";
        trigger["data"] = std::string("hello") + std::to_string(i);
        std::string packed = callerPtr->Lua()["cmsgpack"]["pack"](trigger);
        sys.Send(callerId, ActorMessage::Make(MsgType::UserMessage, 0, 0,
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

    sys.Stop();

    std::cout << "======================================" << std::endl;
    std::cout << (pass ? "  [PASS] Phase 3 actor.call roundtrip OK!"
                       : "  [FAIL] Phase 3 test failed!")
              << std::endl;
    std::cout << "======================================" << std::endl;

    return pass ? 0 : 1;
}
