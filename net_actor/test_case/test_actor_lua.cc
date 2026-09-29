/**
@file: test_actor_lua.cc
@brief: Phase 1 LuaActor 同步版验证 — 多 actor 间 cmsgpack 消息往返

链路（无 TCP 层，纯 actor-to-actor）：
  main ── Send(clientId, {cmd="start"}) ──> Client LuaActor
    Client.OnMessage(start) → 循环 10 次 actor_send(serverId, {cmd="ping", seq=i})
      Server.OnMessage(ping) → actor_send(src, {cmd="pong", seq=seq}) + test_record("ping")
    Client.OnMessage(pong) → test_record("pong")

通过标准：g_pingCount == 10 && g_pongCount == 10
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

static std::atomic<int> g_pingCount{0};
static std::atomic<int> g_pongCount{0};

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << "  LuaActor Sync Test (cmsgpack roundtrip)" << std::endl;
    std::cout << "======================================" << std::endl;

    // ----- 1. 启动单 EventLoop + ActorSystem（无 TCP，单系统即可） -----
    EventLoop loop;
    loop.Create();

    ActorSystem sys;
    sys.Start(4, &loop);
    loop.SetActorSystem(&sys);

    // ----- 2. 注册 Server / Client LuaActor -----
    //   构造在 main 线程：sol::state 在 ctor 里 open_libs + 预加载 cmsgpack + require 脚本
    auto server = std::make_unique<LuaActor>("ping_server");
    auto client = std::make_unique<LuaActor>("ping_client");
    LuaActor* serverPtr = server.get();
    LuaActor* clientPtr = client.get();

    // 显式转 unique_ptr<Actor>，避免 RegisterActor 的 unique_ptr/shared_ptr 重载歧义
    uint32_t serverId = sys.RegisterActor(std::unique_ptr<Actor>(std::move(server)));
    uint32_t clientId  = sys.RegisterActor(std::unique_ptr<Actor>(std::move(client)));

    std::cout << "[Setup] serverId=" << serverId << " clientId=" << clientId
              << std::endl;

    // ----- 3. 在发消息前（worker 还没碰过 actor）设置 Lua 全局 / 桥接 -----
    //   串行不变量保证此窗口内 main 独占 sol::state
    serverPtr->Lua().set_function("test_record",
        [](const std::string& cat) {
            if (cat == "ping") g_pingCount.fetch_add(1);
        });
    clientPtr->Lua().set_function("test_record",
        [](const std::string& cat) {
            if (cat == "pong") g_pongCount.fetch_add(1);
        });
    clientPtr->Lua()["serverId"] = serverId;

    // ----- 4. 用 client 的 sol::state 打包 kickoff（仍在 main，安全） -----
    sol::table kickoff = clientPtr->Lua().create_table();
    kickoff["cmd"] = "start";
    sol::protected_function_result packRes =
        clientPtr->Lua()["cmsgpack"]["pack"](kickoff);
    if (!packRes.valid()) {
        std::cerr << "[FAIL] kickoff pack failed" << std::endl;
        sys.Stop();
        return 1;
    }
    std::string kickoffPacked = packRes.get<std::string>();

    // ----- 5. Send kickoff → 此后 worker 可开始驱动 Client -----
    sys.Send(clientId, ActorMessage::Make(MsgType::UserMessage, 0, 0,
                                          kickoffPacked.data(),
                                          kickoffPacked.size()));
    std::cout << "[Test] kickoff sent, waiting for 10 pings + 10 pongs..."
              << std::endl;

    // ----- 6. 轮询计数器，期望 10/10 -----
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((g_pingCount.load() < 10 || g_pongCount.load() < 10) &&
           std::chrono::steady_clock::now() < deadline) {
        usleep(10000);  // 10ms
    }

    int ping = g_pingCount.load();
    int pong = g_pongCount.load();
    bool ok = (ping == 10 && pong == 10);

    std::cout << "[Verify] ping=" << ping << " pong=" << pong << std::endl;

    sys.Stop();

    std::cout << "======================================" << std::endl;
    std::cout << (ok ? "  [PASS] All checks passed!" : "  [FAIL] Some checks failed!")
              << std::endl;
    std::cout << "======================================" << std::endl;

    return ok ? 0 : 1;
}
