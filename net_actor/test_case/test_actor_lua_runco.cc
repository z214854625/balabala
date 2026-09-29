/**
@file: test_actor_lua_runco.cc
@brief: 验证 actor.run_co helper — C++ 协程内调 Lua,Lua 内启嵌套协程并 yield,
        信号自动 propagate 到 C++,响应回来再喂回嵌套协程。

链路:
  main ── Send(callerId, trigger) ──> Caller LuaActor
    Caller.OnMessage(trigger) → actor.run_co(fn)
      fn 内嵌套协程:
        r1 = actor.call(service, {cmd="query", data="hello"})  → yield {wait}
        r2 = actor.call(service, {cmd="query", data=r1.result}) → yield {wait}
        return {ok=true, final=r2.result}
      run_co 自动 propagate yield 到 C++
      C++ co_await Call 两次,响应依次回来
    验证 r2.result == "processed:processed:hello" → test_record("ok")
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
    std::cout << "  LuaActor — actor.run_co Nested Coroutine Test" << std::endl;
    std::cout << "  (C++ coroutine ↔ Lua nested coroutine yield propagate)" << std::endl;
    std::cout << "======================================" << std::endl;

    EventLoop loop; loop.Create();
    ActorSystem sys; sys.Start(4, &loop); loop.SetActorSystem(&sys);

    auto service = std::make_unique<LuaActor>("runco_service");
    auto caller = std::make_unique<LuaActor>("runco_caller");
    LuaActor* servicePtr = service.get();
    LuaActor* callerPtr = caller.get();

    uint32_t serviceId = sys.RegisterActor(std::unique_ptr<Actor>(std::move(service)));
    uint32_t callerId  = sys.RegisterActor(std::unique_ptr<Actor>(std::move(caller)));

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

    for (int i = 0; i < kRounds; ++i) {
        sol::table trigger = callerPtr->Lua().create_table();
        trigger["cmd"] = "trigger";
        trigger["data"] = std::string("hello") + std::to_string(i);
        std::string packed = callerPtr->Lua()["cmsgpack"]["pack"](trigger);
        sys.Send(callerId, ActorMessage::Make(MsgType::UserMessage, 0, 0,
                                              packed.data(), packed.size()));
    }

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
    std::cout << (pass ? "  [PASS] actor.run_co roundtrip OK!"
                       : "  [FAIL] run_co test failed!")
              << std::endl;
    std::cout << "======================================" << std::endl;

    return pass ? 0 : 1;
}
