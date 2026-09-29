#define SOL_ALL_SAFETIES_ON 1
#include "LuaBridge.h"
#include "LuaActor.h"
#include "Actor.h"
#include "Message.h"

#include <stdexcept>
#include <string>

namespace bllsll {

namespace {

// 桥接函数的统一错误处理。
//
// 背景：luaL_error 内部是 lua_error → LUAI_THROW。本项目链接的 liblua-5.4.so
// 为 C 编译（实测该 .so 内 __cxa_throw / __gxx_personality / _Unwind_Resume
// 均为 0 处引用，而 luaD_throw 存在），故 LUAI_THROW 是 longjmp 而非 throw。
//
// 原实现在 catch 块内直接调 luaL_error，问题是：
//   longjmp 从 catch 块内跳出，异常永远走不完 __cxa_end_catch（异常对象不释放、
//   运行时的"正在处理异常"状态不清），且 longjmp 跨过 sol2 trampoline 等中间
//   C++ 栈帧时不做栈展开，这些帧的局部对象析构被跳过。
//   （注意：try 块内的局部量如 packed 在进入 catch 前已正常析构，不是泄漏点。）
//
// 正确做法（sol2 惯用法）：桥接函数只抛 C++ 异常，由 sol2 trampoline 捕获后
// 在"无 C++ 对象存活"的边界上调用 lua_error。本项目 Lua 5.4 非 LuaJIT，
// SOL_PROPAGATE_EXCEPTIONS 为 OFF（见 sol/compatibility/lua_version.hpp:156），
// trampoline 走 try/catch 分支，其 lua_error 位于所有 catch 块之外
// （见 sol/trampoline.hpp:127）——正是安全的调用点。
template <typename F>
void GuardedBridge(const char* what, F&& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string(what) + ": " + e.what());
    }
}

}  // namespace

void RegisterBridges(sol::state_view lua, LuaActor* self) {
    // 先建 `actor` 全局表，所有桥接函数作为其字段（Lua 惯用法）
    sol::table actor = lua.create_named_table("actor");

    // actor.self() → uint32_t
    actor.set_function("self", [self]() { return self->GetActorId(); });

    // actor.send(target, payload) → cmsgpack.pack 后 SendToActor（fire-and-forget）
    actor.set_function("send",
        [self, lua](uint32_t target, sol::table payload) {
            GuardedBridge("actor.send", [&] {
                std::string packed = lua["cmsgpack"]["pack"](payload);
                auto msg = ActorMessage::Make(
                    MsgType::UserMessage, self->GetActorId(), -1,
                    packed.data(), packed.size());
                self->SendToActor(target, std::move(msg));
            });
        });

    // actor.respond(src, sid, payload) → 响应 C++ 侧的 co_await Call 请求
    // Phase 2：LuaActor 作为被调用方，收到 UserMessage 后通过此桥返回响应
    actor.set_function("respond",
        [self, lua](uint32_t src, uint32_t sid, sol::table payload) {
            GuardedBridge("actor.respond", [&] {
                std::string packed = lua["cmsgpack"]["pack"](payload);
                ActorMessage reqCtx;
                reqCtx.sourceId = src;
                reqCtx.sessionId = sid;
                auto resp = ActorMessage::Make(
                    MsgType::UserMessage, self->GetActorId(), -1,
                    packed.data(), packed.size());
                self->RespondToCall(reqCtx, std::move(resp));
            });
        });

    // Phase 4: actor.respond_remote(srcNode, srcName, sid, payload)
    //   跨进程响应：远端 LuaActor 收到 ClusterCall 请求后通过此桥回程
    //   内部走 Actor::RespondRemote，按 msg.sourceNodeId + msg.sourceActorName 路由
    actor.set_function("respond_remote",
        [self, lua](const std::string& srcNode, const std::string& srcName,
                    uint32_t sid, sol::table payload) {
            GuardedBridge("actor.respond_remote", [&] {
                std::string packed = lua["cmsgpack"]["pack"](payload);
                ActorMessage reqCtx;
                reqCtx.sourceNodeId = srcNode;
                reqCtx.sourceActorName = srcName;
                reqCtx.sessionId = sid;
                self->RespondRemote(reqCtx, packed);
            });
        });

    // 纯 Lua 部分（actor.call / actor.cluster_call / actor.run_co）已移至
    // net_actor/lua/actor_bridge.lua，经 package.path 按名 require 加载。
    //
    // 时序保证：
    //   1. package.path 在 LuaEnv 构造时已配置（LuaEnv 是 LuaActor 的成员，
    //      先于 LuaActor 构造体运行，而本函数由构造体调用）
    //   2. 全局 actor 表在上面已 create_named_table 建好，脚本里
    //      `function actor.call(...)` 才有表可挂
    //   3. cmsgpack / coroutine 均已由 LuaEnv 预加载/开库
    //
    // 加载失败直接抛异常：桥接缺失会让所有 actor.call 静默失效，
    // 必须 fail fast，不做静默降级（已无内嵌字符串可回退）。
    sol::protected_function requireFn = lua["require"];
    if (!requireFn.valid()) {
        throw std::runtime_error(
            "RegisterBridges: global 'require' missing (package lib not opened?)");
    }
    sol::protected_function_result bridgeRes = requireFn("actor_bridge");
    if (!bridgeRes.valid()) {
        sol::error err = bridgeRes;
        throw std::runtime_error(
            std::string("RegisterBridges: require 'actor_bridge' failed: ")
            + err.what());
    }
}

}  // namespace bllsll
