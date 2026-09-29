#pragma once
/**
@auther: chencaiyu
@date: 2026.8
@brief: 派生自 Actor 的 Lua 执行器（Phase 1 同步版）

设计文档 Phase 1：
  - 每 LuaActor 实例持一份独立 LuaEnv（sol::state）
  - 覆写 OnCoroutineMessage：收到 UserMessage → cmsgpack.unpack → 调脚本 OnMessage
  - 不在 OnCoroutineMessage 内部 co_await（纯同步驱动）
  - 串行不变量保证 worker 全程独占 lua_State，无需锁

脚本侧接口（约定）：
  function OnMessage(payload, sourceId, sessionId, fd) ... end
  payload：cmsgpack.unpack 出来的 table
  可调用 actor_self() / actor_send(target, table)

析构顺序约定（设计文档第六章"不跨挂起点持 sol 引用"）：
  onMessage_ / unpackFn_ 必须在 env_ 之前析构（释放 Lua ref 后再 lua_close）
  成员声明顺序：env_, onMessage_, unpackFn_ → 反向析构满足约束
*/

#include "Actor.h"
#include "LuaEnv.h"
#include <sol/sol.hpp>
#include <string>

namespace bllsll {

class LuaActor : public Actor {
    friend void RegisterBridges(sol::state_view lua, LuaActor* self);
public:
    // 按逻辑模块名加载业务脚本（经 package.path 解析，见 LuaEnv.h）：
    //   LuaActor("call_service") → <root>/test_case/lua/call_service.lua
    //   LuaActor("some_service") → <root>/lua/some_service.lua
    // 脚本顶层的 function OnMessage(...) 会成为该 state 的全局 OnMessage
    // （require 以 _ENV = 全局表执行 chunk）。每个 LuaActor 持独立 LuaEnv
    // （独立 package.loaded），故模块每 actor 加载一次。
    explicit LuaActor(std::string moduleName);
    ~LuaActor() override = default;

    ActorTask OnCoroutineMessage(ActorMessage msg) override;

    // 测试 / 外部读取 Lua 状态（仅 main 线程、注册前/无消息窗口内安全使用）
    sol::state& Lua() { return env_.State(); }

private:
    LuaEnv env_;
    sol::protected_function onMessage_;
    sol::protected_function unpackFn_;
};

}  // namespace bllsll
