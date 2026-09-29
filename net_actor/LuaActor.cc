#define SOL_ALL_SAFETIES_ON 1
#include "LuaActor.h"
#include "LuaBridge.h"
#include "Message.h"
#include "Coroutine.h"

#include <iostream>
#include <string>
#include <utility>

namespace bllsll {

namespace {

// RAII wrapper for Lua registry reference.
// 关键：C++ 协程在 co_await 处可能被 DestroyAllCoroutines 强制销毁，
// 此时 locals 的析构会运行（C++20 规范保证），~LuaRegRef 调 luaL_unref 释放 Lua 协程帧引用。
// 由于 ActorSystem::Stop 中 DestroyAllCoroutines 在 actors_ 仍存活时调用
// （lua_close 在更晚的 ~LuaEnv 才发生），luaL_unref 此时操作 mainL 仍安全。
class LuaRegRef {
public:
    LuaRegRef(lua_State* L, int ref) : L_(L), ref_(ref) {}
    ~LuaRegRef() {
        if (L_ && ref_ != LUA_NOREF && ref_ != LUA_REFNIL) {
            luaL_unref(L_, LUA_REGISTRYINDEX, ref_);
        }
    }
    LuaRegRef(const LuaRegRef&) = delete;
    LuaRegRef& operator=(const LuaRegRef&) = delete;
    LuaRegRef(LuaRegRef&& o) noexcept : L_(o.L_), ref_(o.ref_) {
        o.ref_ = LUA_NOREF;
        o.L_ = nullptr;
    }
    int get() const { return ref_; }
private:
    lua_State* L_ = nullptr;
    int ref_ = LUA_NOREF;
};

// yield 信号：Lua 侧 coroutine.yield 传上来的动作描述
struct YieldSignal {
    bool valid = false;
    std::string action;
    // 本地 Call
    uint32_t target = 0;
    // 跨进程 ClusterCall
    std::string nodeId;
    std::string actorName;
    // 共用 payload
    std::string payload;
};

// 从 yield 信号表里读出 action/target/payload 或 cluster_call 字段
// idx: 信号表在 th 栈上的位置（相对索引，如 -nres）
YieldSignal ReadYieldSignal(lua_State* th, int idx) {
    YieldSignal sig;
    // 归一化为绝对索引：后续 lua_getfield 会推值改变栈顶，相对索引会漂移
    int t = lua_absindex(th, idx);
    if (lua_type(th, t) != LUA_TTABLE) return sig;
    // action
    lua_getfield(th, t, "action");
    if (lua_type(th, -1) == LUA_TSTRING) {
        sig.action = lua_tostring(th, -1);
    }
    lua_pop(th, 1);
    // target (本地 Call 用)
    lua_getfield(th, t, "target");
    if (lua_type(th, -1) == LUA_TNUMBER) {
        sig.target = (uint32_t)lua_tointeger(th, -1);
    }
    lua_pop(th, 1);
    // nodeId (ClusterCall 用)
    lua_getfield(th, t, "nodeId");
    if (lua_type(th, -1) == LUA_TSTRING) {
        sig.nodeId = lua_tostring(th, -1);
    }
    lua_pop(th, 1);
    // actorName (ClusterCall 用)
    lua_getfield(th, t, "actorName");
    if (lua_type(th, -1) == LUA_TSTRING) {
        sig.actorName = lua_tostring(th, -1);
    }
    lua_pop(th, 1);
    // payload
    lua_getfield(th, t, "payload");
    if (lua_type(th, -1) == LUA_TSTRING) {
        size_t len = 0;
        const char* p = lua_tolstring(th, -1, &len);
        sig.payload.assign(p, len);
    }
    lua_pop(th, 1);
    sig.valid = (sig.action == "wait" && sig.target != 0)
                || (sig.action == "cluster_call"
                    && !sig.nodeId.empty() && !sig.actorName.empty());
    return sig;
}

}  // namespace

// 按模块名加载：经 package.path（LuaEnv 构造时已配置），require 解析为
// <root>/lua/<name>.lua 或 <root>/test_case/lua/<name>.lua。
LuaActor::LuaActor(std::string moduleName) : env_() {
    // 1. 注册桥接函数
    RegisterBridges(env_.State(), this);
    // 2. require 加载模块脚本，定义全局 OnMessage。
    //    require 以 _ENV = 全局表执行 chunk，故顶层 function OnMessage(...)
    //    成为全局 OnMessage，与旧的 safe_script 行为一致。
    sol::protected_function requireFn = env_.State()["require"];
    if (!requireFn.valid()) {
        std::cerr << "[LuaActor] global 'require' missing (package lib not opened?)"
                  << std::endl;
    } else {
        sol::protected_function_result r = requireFn(moduleName);
        if (!r.valid()) {
            sol::error err = r;
            std::cerr << "[LuaActor] require '" << moduleName
                      << "' failed: " << err.what() << std::endl;
        }
    }
    // 3. 缓存 OnMessage 引用（仅用于 validity 检查，实际 dispatch 走 lua_getglobal 取最新）
    onMessage_ = env_.State()["OnMessage"];
    if (!onMessage_.valid()) {
        std::cerr << "[LuaActor] '" << moduleName
                  << "' missing global function OnMessage" << std::endl;
    }
    unpackFn_ = env_.State()["cmsgpack"]["unpack"];
}

ActorTask LuaActor::OnCoroutineMessage(ActorMessage msg) {
    if (msg.type != MsgType::UserMessage) {
        co_return;
    }
    if (!onMessage_.valid() || !unpackFn_.valid()) {
        co_return;
    }

    lua_State* mainL = env_.L();

    // 1. 创建新 Lua 协程（lua_newthread），注册到 registry 防 GC
    lua_State* th = lua_newthread(mainL);
    if (!th) co_return;
    lua_pushvalue(mainL, -1);  // dup thread，留给 luaL_ref
    LuaRegRef thRef(mainL, luaL_ref(mainL, LUA_REGISTRYINDEX));
    lua_pop(mainL, 1);  // 弹掉原始 thread 引用，现在仅在 registry

    // 2. 把 OnMessage 函数压到 th 栈顶
    lua_getglobal(th, "OnMessage");
    if (lua_isnil(th, -1) || !lua_isfunction(th, -1)) {
        std::cerr << "[LuaActor] OnMessage not a function" << std::endl;
        lua_pop(th, 1);
        co_return;
    }

    // 3. 在 th 上 cmsgpack.unpack(data) → payload table
    //    栈推进：[OnMessage] → [OnMessage, cmsgpack, unpack, data] → [OnMessage, payload]
    lua_getglobal(th, "cmsgpack");
    if (lua_isnil(th, -1)) {
        std::cerr << "[LuaActor] cmsgpack not loaded" << std::endl;
        lua_pop(th, 2);  // pop cmsgpack nil + OnMessage
        co_return;
    }
    lua_getfield(th, -1, "unpack");
    lua_pushlstring(th, msg.Data(), msg.Size());
    lua_remove(th, -3);  // 删掉 cmsgpack table
    if (lua_pcall(th, 1, 1, 0) != LUA_OK) {
        const char* err = lua_tostring(th, -1);
        std::cerr << "[LuaActor] cmsgpack.unpack failed: "
                  << (err ? err : "(unknown)") << std::endl;
        lua_pop(th, 2);  // pop error + OnMessage
        co_return;
    }
    // 栈：[OnMessage, payload]

    // 4. 压 src / sid / fd + 跨进程路由信息（Phase 4）
    bool isRemote = msg.IsRemote();
    lua_pushinteger(th, (lua_Integer)msg.sourceId);
    lua_pushinteger(th, (lua_Integer)msg.sessionId);
    lua_pushinteger(th, (lua_Integer)msg.fd);
    lua_pushboolean(th, isRemote ? 1 : 0);
    lua_pushstring(th, msg.sourceNodeId.c_str());
    lua_pushstring(th, msg.sourceActorName.c_str());
    // 栈：[OnMessage, payload, src, sid, fd, is_remote, src_node, src_name]

    // 5. 初始 resume（nargs=7）— from=mainL（resuming state），C++ 上下文
    int nres = 0;
    int status = lua_resume(th, mainL, 7, &nres);

    // 6. yield 循环
    while (status == LUA_YIELD) {
        // nres = yield 带回的值个数，必须按实际个数清栈：
        //   nres == 0（业务写 coroutine.yield()）→ 不能弹，否则栈下溢
        //   nres >= 2（业务写 coroutine.yield(a, b)）→ 栈顶是最后一个值，
        //     信号表不在 -1；且只弹 1 个会每轮泄漏栈槽
        // 约定：信号表是 yield 的第一个值，位于 -nres
        YieldSignal sig;
        if (nres >= 1) {
            sig = ReadYieldSignal(th, -nres);
        }
        lua_pop(th, nres);  // 按实际个数清空 yield 返回值
        nres = 0;

        if (!sig.valid) {
            std::cerr << "[LuaActor] invalid yield signal (action='"
                      << sig.action << "')" << std::endl;
            status = LUA_OK;  // nres 已在上面清零，栈已清空
            break;
        }

        // C++ 协程在此挂起；worker 释放；响应到达后从这里恢复
        // fd 传 -1（ActorMessage 默认值，表示"无关联连接"）；
        // 0 是 stdin 的合法 fd，会让下游误判为有效连接
        auto req = ActorMessage::Make(
            MsgType::UserMessage, GetActorId(), -1,
            sig.payload.data(), sig.payload.size());

        ActorMessage resp;
        if (sig.action == "wait") {
            // 本地 Call
            resp = co_await Call(sig.target, std::move(req));
        } else {
            // cluster_call — 跨进程 ClusterCall
            resp = co_await ClusterCall(sig.nodeId, sig.actorName, std::move(req));
        }

        // 把响应 bytes（或 nil on error）压到 th 栈顶
        if (resp.error != CallError::Ok || resp.DataEmpty()) {
            lua_pushnil(th);
        } else {
            lua_pushlstring(th, resp.Data(), resp.Size());
        }

        // 继续 resume，OnMessage 从 coroutine.yield 处返回
        status = lua_resume(th, mainL, 1, &nres);
    }

    // 7. 终态处理
    if (status == LUA_OK) {
        lua_pop(th, nres);  // 弹掉返回值
    } else {
        const char* err = lua_tostring(th, -1);
        std::cerr << "[LuaActor] dispatch error: "
                  << (err ? err : "(no error message)") << std::endl;
        lua_pop(th, 1);
    }

    // 8. RAII ~LuaRegRef 释放 registry 引用，Lua 协程帧由后续 GC 回收
    co_return;
}

}  // namespace bllsll
