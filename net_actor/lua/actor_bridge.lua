--[[
@brief: LuaActor 框架桥接层（纯 Lua 部分）

由 RegisterBridges() 通过 require "actor_bridge" 加载，每个 LuaActor 的
sol::state 各加载一次。

依赖（加载时必须已就绪，由 C++ 侧保证）：
  - 全局 actor 表     —— RegisterBridges 内 create_named_table 先建，再 require 本文件
  - cmsgpack          —— LuaEnv 构造时 luaL_requiref 预加载为全局
  - coroutine         —— LuaEnv 构造时 open_libraries 打开

本文件只定义 actor 表上的纯 Lua 函数；C 函数（self/send/respond/respond_remote）
由 C++ 侧 sol2 set_function 注册。
--]]

-- actor.call(target, req) — 纯 Lua 实现，cmsgpack.pack + coroutine.yield
--   C++ 侧 lua_resume 捕获 yield 信号 {action="wait", target, payload}，
--   走 co_await Call 拿响应，再 lua_resume 把响应 bytes 注入。
--   超时/错误时 C++ 推 nil，函数返回 (nil, "timeout or error")。
function actor.call(target, req)
    local packed = cmsgpack.pack(req)
    local respPacked = coroutine.yield({action = "wait", target = target, payload = packed})
    if respPacked == nil then
        return nil, "timeout or error"
    end
    return cmsgpack.unpack(respPacked)
end

-- Phase 4: actor.cluster_call(node, name, req)
--   跨进程 RPC：yield {action="cluster_call", nodeId, actorName, payload}
--   C++ 侧走 co_await ClusterCall(nodeId, actorName, msg)
--   注意：调用方 Actor 必须已通过 ActorSystem::RegisterName 注册名字
function actor.cluster_call(node, name, req)
    local packed = cmsgpack.pack(req)
    local respPacked = coroutine.yield({
        action = "cluster_call",
        nodeId = node,
        actorName = name,
        payload = packed
    })
    if respPacked == nil then
        return nil, "timeout or error"
    end
    return cmsgpack.unpack(respPacked)
end

-- actor.run_co(fn, ...) — 在 OnMessage 内运行嵌套 Lua 协程
--   背景：Lua 嵌套协程的 yield 只回到最近的 coroutine.resume，不自动穿回 C++。
--   此 helper 自动把嵌套协程的 yield 信号 propagate 到顶层 OnMessage（C++ 看到），
--   C++ co_await Call 完成后推响应回来，本 helper 再把响应喂回嵌套协程。
--   用法：
--     function OnMessage(payload, src, sid, ...)
--         local result = actor.run_co(function()
--             local r1 = actor.call(svcA, {cmd="q"})       -- 嵌套 yield 自动 propagate
--             local r2 = actor.cluster_call("nodeB", "svcB", {cmd="q"})
--             return {a = r1.x, b = r2.y}
--         end)
--         actor.respond(src, sid, result)
--     end
function actor.run_co(fn, ...)
    local co = coroutine.create(fn)
    local ok, sig = coroutine.resume(co, ...)
    if not ok then
        error("run_co: " .. tostring(sig), 2)
    end
    while coroutine.status(co) ~= "dead" do
        local respPacked = coroutine.yield(sig)   -- propagate 到 C++
        ok, sig = coroutine.resume(co, respPacked) -- 响应回来喂给嵌套协程
        if not ok then
            error("run_co: " .. tostring(sig), 2)
        end
    end
    return sig  -- 嵌套协程的 return 值
end
