/**
@file: test_actor_lua_bench.cc
@brief: Phase 4 — Lua actor.call 吞吐 / 延迟基准

N 次 Lua actor.call 往返：caller 发 query，service 回 processed:<data>。
对比纯 C++ co_await Call 同等链路，看 Lua 桥接层的开销。
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

static const int kRounds = 1000;
static std::atomic<int> g_luaDone{0};
static std::atomic<int> g_cppDone{0};

// C++ 对照组：普通 Actor，co_await Call
class CppBenchCaller : public Actor
{
public:
    uint32_t serviceId = 0;
    std::string packedReq;
    std::string expectedResp;

    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        if (msg.type != MsgType::UserMessage || msg.data != "trigger") co_return;
        for (int i = 0; i < kRounds; ++i) {
            auto req = ActorMessage::Make(MsgType::UserMessage, GetActorId(), 0,
                                          packedReq.data(), packedReq.size());
            auto resp = co_await Call(serviceId, std::move(req), 5000);
            if (resp.error != CallError::Ok || resp.DataEmpty()) {
                std::cerr << "[CppBench] iter " << i << " failed" << std::endl;
                co_return;
            }
        }
        g_cppDone.store(1);
        co_return;
    }
};

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << "  LuaActor Phase 4 — Benchmark" << std::endl;
    std::cout << "  rounds=" << kRounds << " per scenario" << std::endl;
    std::cout << "======================================" << std::endl;

    EventLoop loop; loop.Create();
    ActorSystem sys; sys.Start(4, &loop); loop.SetActorSystem(&sys);

    // 服务
    auto service = std::make_unique<LuaActor>("bench_service");
    LuaActor* servicePtr = service.get();
    uint32_t serviceId = sys.RegisterActor(std::unique_ptr<Actor>(std::move(service)));

    // ===== Lua benchmark =====
    auto luaCaller = std::make_unique<LuaActor>("bench_caller");
    LuaActor* luaCallerPtr = luaCaller.get();
    uint32_t luaCallerId = sys.RegisterActor(std::unique_ptr<Actor>(std::move(luaCaller)));
    luaCallerPtr->Lua().set_function("test_record", [](const std::string& cat) {
        if (cat == "done") g_luaDone.store(1);
        else if (cat == "err") g_luaDone.store(-1);
    });
    luaCallerPtr->Lua()["serviceId"] = serviceId;
    // 预打包 trigger {cmd="trigger", n=kRounds}
    sol::table trigger = luaCallerPtr->Lua().create_table();
    trigger["cmd"] = "trigger";
    trigger["n"] = kRounds;
    std::string trigPacked = luaCallerPtr->Lua()["cmsgpack"]["pack"](trigger);

    auto t1 = std::chrono::steady_clock::now();
    sys.Send(luaCallerId, ActorMessage::Make(MsgType::UserMessage, 0, 0,
                                              trigPacked.data(), trigPacked.size()));
    auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (g_luaDone.load() == 0 && std::chrono::steady_clock::now() < dl) usleep(1000);
    auto t2 = std::chrono::steady_clock::now();
    double luaMs = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count() / 1000.0;
    int luaStatus = g_luaDone.load();

    // ===== C++ benchmark =====
    auto cppCaller = std::make_unique<CppBenchCaller>();
    cppCaller->serviceId = serviceId;
    // 用 service 的 Lua 状态打包 query 请求和预期响应
    sol::table q = servicePtr->Lua().create_table();
    q["cmd"] = "query"; q["data"] = "bench";
    cppCaller->packedReq = servicePtr->Lua()["cmsgpack"]["pack"](q);
    sol::table expR = servicePtr->Lua().create_table();
    expR["result"] = "processed:bench";
    cppCaller->expectedResp = servicePtr->Lua()["cmsgpack"]["pack"](expR);
    CppBenchCaller* cppCallerPtr = cppCaller.get();
    uint32_t cppCallerId = sys.RegisterActor(std::unique_ptr<Actor>(std::move(cppCaller)));

    g_cppDone.store(0);
    auto t3 = std::chrono::steady_clock::now();
    sys.Send(cppCallerId, ActorMessage{MsgType::UserMessage, 0, -1, "trigger"});
    auto dl2 = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (g_cppDone.load() == 0 && std::chrono::steady_clock::now() < dl2) usleep(1000);
    auto t4 = std::chrono::steady_clock::now();
    double cppMs = std::chrono::duration_cast<std::chrono::microseconds>(t4 - t3).count() / 1000.0;

    sys.Stop();

    double luaPerOpUs = (luaMs * 1000) / kRounds;
    double cppPerOpUs = (cppMs * 1000) / kRounds;
    double luaOps = kRounds / (luaMs / 1000.0);
    double cppOps = kRounds / (cppMs / 1000.0);

    std::cout << "\n[Bench] Lua actor.call  : "
              << (luaStatus == 1 ? "OK  " : "FAIL")
              << "  total=" << luaMs << " ms  per-op=" << luaPerOpUs << " us"
              << "  ops/sec=" << (int)luaOps << std::endl;
    std::cout << "[Bench] C++ co_await Call: "
              << (g_cppDone.load() == 1 ? "OK  " : "FAIL")
              << "  total=" << cppMs << " ms  per-op=" << cppPerOpUs << " us"
              << "  ops/sec=" << (int)cppOps << std::endl;
    if (luaStatus == 1 && g_cppDone.load() == 1 && cppPerOpUs > 0) {
        std::cout << "[Bench] Lua/C++ latency ratio: "
                  << luaPerOpUs / cppPerOpUs << "x" << std::endl;
    }

    bool pass = (luaStatus == 1 && g_cppDone.load() == 1);
    std::cout << "======================================" << std::endl;
    std::cout << (pass ? "  [PASS] Benchmark completed."
                       : "  [FAIL] Benchmark failed.")
              << std::endl;
    std::cout << "======================================" << std::endl;
    return pass ? 0 : 1;
}
