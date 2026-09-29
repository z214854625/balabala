#pragma once
/**
@auther: chencaiyu
@date: 2026.8
@brief: 注册到 Lua 的 C 桥接函数（actor_send / actor_self）

Phase 1 只暴露同步 API：
  - actor_self()  → 返回当前 actor 的 actorId
  - actor_send(target, payloadTable) → cmsgpack.pack 后 SendToActor（fire-and-forget）

硬约束（设计文档第十一章）：
  桥接函数只入队消息 / 注册定时器，永不同步回调本 actor 的 OnCoroutineMessage
  → 防止 Lua 状态重入
*/

#include <sol/sol.hpp>

namespace bllsll {

class LuaActor;

// 在指定 sol::state 上注册桥接函数
// self：桥接函数内部访问的 LuaActor 指针（actor_self/actor_send 用）
void RegisterBridges(sol::state_view lua, LuaActor* self);

}  // namespace bllsll
